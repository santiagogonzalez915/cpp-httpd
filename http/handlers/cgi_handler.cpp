#include "handlers/cgi_handler.hpp"

#include "core/http_request.hpp"
#include "core/http_response.hpp"
#include "core/request_handler_utils.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

CgiHandler::CgiHandler(const Config& config) : config(config) {}

std::pair<std::vector<std::string>, std::vector<char*>> CgiHandler::build_cgi_environment(
    const HttpRequest& request,
    const VirtualHost* vhost,
    const std::string* client_remote_addr) const {
    std::string query_string = split_uri(request.uri).second;
    std::unordered_map<std::string, std::string> env_map;

    env_map["REQUEST_METHOD"] = request.get_method();
    env_map["QUERY_STRING"] = query_string;
    env_map["SERVER_PROTOCOL"] = "HTTP/1.1";
    env_map["SERVER_PORT"] = std::to_string(config.listen_port);

    if (vhost) {
        env_map["SERVER_NAME"] = vhost->server_name;
    }

    if (request.get_method() == "POST") {
        env_map["CONTENT_LENGTH"] = std::to_string(request.get_body().size());
        std::string content_type = get_header(request.headers, "content-type");
        if (!content_type.empty()) {
            env_map["CONTENT_TYPE"] = content_type;
        }
    } else {
        env_map["CONTENT_LENGTH"] = "0";
        env_map["CONTENT_TYPE"] = "";
    }

    if (client_remote_addr && !client_remote_addr->empty()) {
        env_map["REMOTE_ADDR"] = *client_remote_addr;
        env_map["REMOTE_HOST"] = *client_remote_addr;
    } else {
        env_map["REMOTE_ADDR"] = "";
        env_map["REMOTE_HOST"] = "";
    }

    // Build envp for execve() - strings in env_strs must stay alive until execve is called
    std::vector<std::string> env_strs;
    env_strs.reserve(env_map.size());
    for (const auto& [key, value] : env_map) {
        env_strs.push_back(key + "=" + value);
    }

    std::vector<char*> envp;
    envp.reserve(env_strs.size() + 1);
    for (auto& str : env_strs) {
        envp.push_back(const_cast<char*>(str.c_str()));
    }
    envp.push_back(nullptr);

    return std::make_pair(std::move(env_strs), std::move(envp));
}

bool CgiHandler::setup_cgi_pipes(int stdin_pipe[2], int stdout_pipe[2]) const {
    if (pipe(stdin_pipe) != 0) {
        return false;
    }
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }
    return true;
}

bool CgiHandler::write_post_body_to_child(int stdin_fd, const std::string& body, pid_t pid) const {
    const char* buffer = body.data();
    size_t total_bytes = body.size();
    size_t bytes_written = 0;

    while (bytes_written < total_bytes) {
        ssize_t n = write(stdin_fd, buffer + bytes_written, total_bytes - bytes_written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(stdin_fd);
            int status;
            (void)waitpid(pid, &status, 0);
            return false;
        }
        bytes_written += static_cast<size_t>(n);
    }
    return true;
}

std::pair<bool, std::string> CgiHandler::read_child_output(int stdout_fd, pid_t pid) const {
    std::string cgi_output;
    char read_buf[4096];
    const int timeout_sec = config.timeout_seconds > 0 ? config.timeout_seconds : 30;

    while (true) {
        // Use select() with a deadline so a hung CGI script doesn't block forever.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(stdout_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        int ready = select(stdout_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready == 0) {
            // Timeout — kill the child and report 504.
            kill(pid, SIGKILL);
            int status;
            (void)waitpid(pid, &status, 0);
            return std::make_pair(false, "");
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            kill(pid, SIGKILL);
            int status;
            (void)waitpid(pid, &status, 0);
            return std::make_pair(false, "");
        }

        ssize_t n = read(stdout_fd, read_buf, sizeof(read_buf));
        if (n == 0) {
            break;
        } else if (n < 0) {
            if (errno == EINTR) continue;
            kill(pid, SIGKILL);
            int status;
            (void)waitpid(pid, &status, 0);
            return std::make_pair(false, "");
        }
        cgi_output.append(read_buf, static_cast<size_t>(n));
    }
    return std::make_pair(true, cgi_output);
}

std::pair<bool, int> CgiHandler::wait_for_child(pid_t pid) const {
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        return std::make_pair(false, 0);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::make_pair(false, WEXITSTATUS(status));
    }
    return std::make_pair(true, 0);
}

HttpResponse CgiHandler::parse_cgi_output(const std::string& cgi_output) const {
    HttpResponse response(StatusCode::Ok);
    response.status_message = "OK";

    size_t sep_length = 4;
    size_t bl_pos = cgi_output.find("\r\n\r\n");

    if (bl_pos == std::string::npos) {
        bl_pos = cgi_output.find("\n\n");
        sep_length = 2;
    }

    if (bl_pos == std::string::npos) {
        return HttpResponse(StatusCode::InternalServerError);
    }

    std::string header_block = cgi_output.substr(0, bl_pos);
    std::string body = cgi_output.substr(bl_pos + sep_length);
    std::istringstream stream(header_block);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;

        if (line.rfind("Status: ", 0) == 0) {
            std::istringstream status(line.substr(8));
            int code;
            std::string message;
            status >> code;
            std::getline(status, message);

            message = trim(message, " ");
            response.status_code = static_cast<StatusCode>(code);
            response.status_message = message;
        } else {
            auto [name, value] = parse_header_line(line, false);
            if (name.empty()) continue;
            if (name == "Connection" || name == "Transfer-Encoding" || name == "Content-Length") continue;
            response.headers[name] = value;
        }
    }
    response.body = body;
    response.headers["Transfer-Encoding"] = "chunked";
    return response;
}

HttpResponse CgiHandler::run_cgi(const std::string& resolved_path,
                                const HttpRequest& request,
                                const VirtualHost* vhost,
                                const std::string* client_remote_addr) const {
    // Build CGI environment - keep env_strs alive until execve is called
    auto [env_strs, envp] = build_cgi_environment(request, vhost, client_remote_addr);

    // Setup pipes
    int stdin_pipe[2];
    int stdout_pipe[2];
    if (!setup_cgi_pipes(stdin_pipe, stdout_pipe)) {
        return HttpResponse(StatusCode::InternalServerError);
    }

    // Fork and execute child
    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return HttpResponse(StatusCode::InternalServerError);
    }

    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);

        if (dup2(stdin_pipe[0], STDIN_FILENO) == -1 || dup2(stdout_pipe[1], STDOUT_FILENO) == -1) {
            _exit(1);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        char* argv[] = {const_cast<char*>(resolved_path.c_str()), nullptr};
        execve(resolved_path.c_str(), argv, envp.data());

        _exit(1);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    // Write POST body if needed
    if (request.get_method() == "POST") {
        if (!write_post_body_to_child(stdin_pipe[1], request.get_body(), pid)) {
            close(stdout_pipe[0]);
            return HttpResponse(StatusCode::InternalServerError);
        }
    }
    close(stdin_pipe[1]);

    // Read child output (may kill the child and return false on timeout)
    auto [read_success, cgi_output] = read_child_output(stdout_pipe[0], pid);
    close(stdout_pipe[0]);
    if (!read_success) {
        return HttpResponse(StatusCode::GatewayTimeout);
    }

    // Wait for child and check exit status
    auto [wait_success, exit_code] = wait_for_child(pid);
    (void)exit_code;
    if (!wait_success) {
        return HttpResponse(StatusCode::BadGateway);
    }

    // Parse CGI output into HttpResponse
    return parse_cgi_output(cgi_output);
}


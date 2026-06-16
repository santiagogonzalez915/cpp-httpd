#include "connection/connection_manager.hpp"

#include "core/http_request.hpp"
#include "core/http_response.hpp"
#include "core/request_parser.hpp"
#include "handlers/request_handler.hpp"
#include "core/request_handler_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
#else
#  include <sys/sendfile.h>
#endif

namespace {
bool wants_close(const HttpRequest& req) {
    std::string conn = get_header(req.headers, "connection");
    if (conn.empty()) {
        return false;  // HTTP/1.1 defaults to keep-alive
    }
    std::string conn_lower = to_lower(conn);
    return conn_lower.find("close") != std::string::npos;
}
}  // namespace

std::pair<int, std::string> accept_connection(int listen_fd) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (client_fd < 0) return {-1, ""};

    char addr_buf[INET_ADDRSTRLEN];
    std::string client_ip;
    if (inet_ntop(AF_INET, &client_addr.sin_addr, addr_buf, sizeof(addr_buf))) {
        client_ip = std::string(addr_buf);
    }
    return {client_fd, std::move(client_ip)};
}

void write_all(int fd, const std::string& data) {
    ssize_t sent = 0;
    const ssize_t total = static_cast<ssize_t>(data.size());
    while (sent < total) {
        ssize_t w = write(fd, data.data() + sent, static_cast<size_t>(total - sent));
        if (w <= 0) {
            return;
        }
        sent += w;
    }
}

ResponsePackage process_request(const HttpRequest& req,
                                RequestHandler& handler,
                                const std::string* client_ip,
                                bool accepting_new_connections) {
    HttpResponse resp = handler.handle(req, accepting_new_connections, client_ip);
    bool client_wants_close = wants_close(req);
    resp.headers["Connection"] = client_wants_close ? "close" : "keep-alive";

    ResponsePackage out;
    out.close_after = client_wants_close;

    if (resp.body_fd >= 0) {
        // Transfer sendfile ownership to the ResponsePackage.
        out.bytes          = resp.headers_to_bytes();
        out.body_fd        = resp.body_fd;
        out.body_fd_offset = resp.body_fd_offset;
        out.body_fd_size   = resp.body_fd_size;
        resp.body_fd = -1;  // prevent double-close
    } else {
        out.bytes = resp.to_bytes();
    }
    return out;
}

// Write headers and, if body_fd is set, use sendfile() for the body.
static void write_response(int client_fd, ResponsePackage& pkg) {
    write_all(client_fd, pkg.bytes);

    if (pkg.body_fd < 0) return;

    off_t offset = pkg.body_fd_offset;
    off_t remaining = pkg.body_fd_size;
    while (remaining > 0) {
#ifdef __APPLE__
        off_t chunk = remaining;
        int r = sendfile(pkg.body_fd, client_fd, offset, &chunk, nullptr, 0);
        if (chunk > 0) { offset += chunk; remaining -= chunk; }
        if (r < 0 && errno != EAGAIN && errno != EINTR) break;
#else
        ssize_t sent = sendfile(client_fd, pkg.body_fd, &offset, static_cast<size_t>(remaining));
        if (sent > 0) remaining -= sent;
        else if (sent < 0 && errno != EAGAIN && errno != EINTR) break;
#endif
    }
    close(pkg.body_fd);
    pkg.body_fd = -1;
}

void handle_connection(int client_fd, const std::string& client_ip,
                       RequestHandler& handler, std::atomic<bool>& accepting_connections,
                       int timeout_seconds) {
    // Set socket read timeout so keep-alive connections don't block forever
    struct timeval tv;
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char read_buf[8192];
    RequestParser parser;  // Reuse parser across multiple requests in this connection

    while (true) {
        bool got_complete_request = false;
        bool connection_closed = false;

        if (parser.has_request()) {
            got_complete_request = true;
        } else if (parser.has_error()) {
            StatusCode err_code = (parser.get_error() == ParseError::PayloadTooLarge)
                ? StatusCode::PayloadTooLarge
                : StatusCode::BadRequest;
            HttpResponse error_resp(err_code);
            error_resp.headers["Connection"] = "close";
            write_all(client_fd, error_resp.to_bytes());
            return;
        }

        // Read until we have a complete request or error
        while (!got_complete_request && !connection_closed) {
            ssize_t n = read(client_fd, read_buf, sizeof(read_buf));

            if (n == 0) {
                // Client closed connection
                connection_closed = true;
                break;
            }
            if (n < 0) {
                // Read error or timeout
                if (errno == EINTR) continue;
                connection_closed = true;
                break;
            }

            parser.feed(read_buf, static_cast<size_t>(n));
            if (parser.has_request()) {
                got_complete_request = true;
            } else if (parser.has_error()) {
                StatusCode err_code = (parser.get_error() == ParseError::PayloadTooLarge)
                    ? StatusCode::PayloadTooLarge
                    : StatusCode::BadRequest;
                HttpResponse error_resp(err_code);
                error_resp.headers["Connection"] = "close";
                write_all(client_fd, error_resp.to_bytes());
                return;
            }
        }

        if (connection_closed) {
            return;
        }

        if (!got_complete_request) {
            return;
        }

        // Process the request
        HttpRequest req = parser.get_request();
        ResponsePackage pkg = process_request(
            req, handler,
            client_ip.empty() ? nullptr : &client_ip,
            accepting_connections.load());

        write_response(client_fd, pkg);

        if (pkg.close_after) {
            return;
        }
    }
}


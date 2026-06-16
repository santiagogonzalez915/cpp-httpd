#ifndef CGI_HANDLER_HPP
#define CGI_HANDLER_HPP

#include "config/config.hpp"

#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

struct HttpRequest;
struct HttpResponse;

class CgiHandler {
    const Config& config;

public:
    explicit CgiHandler(const Config& config);

    HttpResponse run_cgi(const std::string& resolved_path,
                         const HttpRequest& request,
                         const VirtualHost* vhost,
                         const std::string* client_remote_addr = nullptr) const;

private:
    // CGI helper functions
    std::pair<std::vector<std::string>, std::vector<char*>> build_cgi_environment(
        const HttpRequest& request,
        const VirtualHost* vhost,
        const std::string* client_remote_addr) const;

    bool setup_cgi_pipes(int stdin_pipe[2], int stdout_pipe[2]) const;
    bool write_post_body_to_child(int stdin_fd, const std::string& body, pid_t pid) const;
    std::pair<bool, std::string> read_child_output(int stdout_fd, pid_t pid) const;
    std::pair<bool, int> wait_for_child(pid_t pid) const;
    HttpResponse parse_cgi_output(const std::string& cgi_output) const;
};

#endif


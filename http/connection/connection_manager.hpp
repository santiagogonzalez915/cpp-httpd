#ifndef CONNECTION_MANAGER_HPP
#define CONNECTION_MANAGER_HPP

#include "core/request_parser.hpp"
#include <atomic>
#include <string>
#include <sys/types.h>
#include <utility>

class RequestHandler;
struct HttpRequest;

struct ConnState {
    int fd = -1;
    std::string client_ip;
    RequestParser parser;
    enum Phase { Reading, Writing };
    Phase phase = Reading;
    std::string write_buf;
    size_t write_offset = 0;
    time_t accepted_at = 0;
    bool close_after_write = false;

    // sendfile state: -1 means no pending file body
    int   sendfile_fd     = -1;
    off_t sendfile_offset = 0;
    off_t sendfile_size   = 0;
};

std::pair<int, std::string> accept_connection(int listen_fd);

void write_all(int fd, const std::string& data);

struct ResponsePackage {
    std::string bytes;
    bool close_after = false;

    // If body_fd >= 0, send bytes first (headers), then sendfile body_fd_size
    // bytes starting at body_fd_offset.  The caller is responsible for closing.
    int   body_fd        = -1;
    off_t body_fd_offset = 0;
    off_t body_fd_size   = 0;
};

ResponsePackage process_request(const HttpRequest& req,
                                RequestHandler& handler,
                                const std::string* client_ip,
                                bool accepting_new_connections);

void handle_connection(int client_fd, const std::string& client_ip,
                       RequestHandler& handler, std::atomic<bool>& accepting_connections,
                       int timeout_seconds);

#endif


#ifndef SERVER_HPP
#define SERVER_HPP

#include <atomic>
#include "config/config.hpp"

class HttpServer {
    Config config;
    int server_fd;
    std::atomic<bool> shutdown_requested;
    std::atomic<bool> accepting_connections;

public:
    explicit HttpServer(const Config& config);
    int run();

private:
    int run_thread_pool(int listen_fd, int shutdown_pipe_read, class RequestHandler& handler);
    int run_select_loops(int listen_fd, int shutdown_pipe_read, class RequestHandler& handler);
};

#endif

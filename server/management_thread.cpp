#include "server/management_thread.hpp"

#include "http/core/request_handler_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/select.h>
#include <unistd.h>

void management_thread_func(std::atomic<bool>* accepting_connections, int pipe_write_fd) {
    std::string line;
    while (std::getline(std::cin, line)) {
        line = trim(line);
        line = to_lower(line);
        if (line == "shutdown") {
            accepting_connections->store(false);
            char b = 0;
            (void)write(pipe_write_fd, &b, 1);
            return;
        }
    }
}

std::pair<bool, bool> wait_for_accept_or_shutdown(int listen_fd, int shutdown_pipe_read) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listen_fd, &read_fds);
    FD_SET(shutdown_pipe_read, &read_fds);
    int nfds = std::max(listen_fd, shutdown_pipe_read) + 1;

    int r = select(nfds, &read_fds, nullptr, nullptr, nullptr);
    if (r < 0) {
        if (errno == EINTR) {
            return {false, false};  // Continue loop
        }
        return {false, true};  // Error, treat as shutdown
    }

    if (FD_ISSET(shutdown_pipe_read, &read_fds)) {
        char b;
        (void)read(shutdown_pipe_read, &b, 1);
        return {false, true};  // Shutdown requested
    }

    bool listen_readable = FD_ISSET(listen_fd, &read_fds) != 0;
    return {listen_readable, false};
}


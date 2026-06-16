#ifndef MANAGEMENT_THREAD_HPP
#define MANAGEMENT_THREAD_HPP

#include <atomic>
#include <string>
#include <utility>

// Read stdin; on "shutdown" set accepting_connections and write to pipe, then exit.
void management_thread_func(std::atomic<bool>* accepting_connections, int pipe_write_fd);

// Wait for listen_fd or shutdown_pipe_read to become readable.
std::pair<bool, bool> wait_for_accept_or_shutdown(int listen_fd, int shutdown_pipe_read);

#endif


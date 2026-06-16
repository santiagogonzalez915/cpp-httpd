#include "server/server.hpp"
#include "server/poller.hpp"

#include "http/connection/connection_manager.hpp"
#include "http/core/http_request.hpp"
#include "http/core/http_response.hpp"
#include "server/management_thread.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef __APPLE__
#  include <sys/sendfile.h>
#endif

namespace {
void acceptor_loop_select(int listen_fd, int shutdown_pipe_read, int wake_pipe_write,
                          std::queue<std::pair<int, std::string>>& client_queue,
                          std::mutex& queue_mutex, std::atomic<bool>& accepting_connections) {
    while (true) {
        auto [listen_readable, shutdown_requested] =
            wait_for_accept_or_shutdown(listen_fd, shutdown_pipe_read);
        if (shutdown_requested) {
            break;
        }
        if (listen_readable && accepting_connections.load()) {
            auto [client_fd, client_ip] = accept_connection(listen_fd);
            if (client_fd >= 0) {
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    client_queue.push({client_fd, client_ip});
                }
                char byte = 0;
                (void)write(wake_pipe_write, &byte, 1);
            }
        }
    }
}

// Pop one (client_fd, client_ip) from queue, set fd non-blocking, add to poller and conns.
void try_accept_from_queue(std::queue<std::pair<int, std::string>>& client_queue,
                           std::mutex& queue_mutex,
                           std::unordered_map<int, ConnState>& conns,
                           Poller& poller) {
    int client_fd = -1;
    std::string client_ip;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (client_queue.empty()) return;
        auto p = client_queue.front();
        client_queue.pop();
        client_fd = p.first;
        client_ip = p.second;
    }
    if (client_fd < 0) return;
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    }
    ConnState state;
    state.fd = client_fd;
    state.client_ip = std::move(client_ip);
    state.phase = ConnState::Reading;
    state.accepted_at = time(nullptr);
    poller.add(client_fd, true, false);
    conns[client_fd] = std::move(state);
}

void reset_conn_to_reading(ConnState& state, Poller& poller) {
    if (state.sendfile_fd >= 0) { close(state.sendfile_fd); state.sendfile_fd = -1; }
    state.parser.reset();
    state.phase             = ConnState::Reading;
    state.write_buf.clear();
    state.write_offset      = 0;
    state.sendfile_offset   = 0;
    state.sendfile_size     = 0;
    state.close_after_write = false;
    poller.modify(state.fd, true, false);
}

// Returns true if the connection should be closed.
bool process_readable(ConnState& state, char* read_buf, size_t buf_size,
                      RequestHandler& handler, std::atomic<bool>& accepting_connections,
                      Poller& poller) {
    ssize_t n = read(state.fd, read_buf, buf_size);
    if (n == 0) return true;
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        return true;
    }
    state.parser.feed(read_buf, static_cast<size_t>(n));
    if (state.parser.has_request()) {
        HttpRequest req = state.parser.get_request();
        ResponsePackage pkg = process_request(
            req, handler,
            state.client_ip.empty() ? nullptr : &state.client_ip,
            accepting_connections.load());

        state.close_after_write = pkg.close_after;
        state.write_buf        = std::move(pkg.bytes);
        state.write_offset     = 0;
        state.sendfile_fd      = pkg.body_fd;
        state.sendfile_offset  = pkg.body_fd_offset;   // file start position
        state.sendfile_size    = pkg.body_fd_size;      // bytes remaining to send
        state.phase            = ConnState::Writing;
        poller.modify(state.fd, false, true);
        return false;
    }
    if (state.parser.has_error()) {
        // Send 400/413 and close — use write_all for simplicity on the error path.
        StatusCode err_code = (state.parser.get_error() == ParseError::PayloadTooLarge)
            ? StatusCode::PayloadTooLarge
            : StatusCode::BadRequest;
        HttpResponse err_resp(err_code);
        err_resp.headers["Connection"] = "close";
        write_all(state.fd, err_resp.to_bytes());
        return true;
    }
    return false;
}

// Returns true if the connection should be closed.
bool process_writable(ConnState& state, Poller& poller) {
    // Phase 1: drain the header/body string buffer.
    size_t remaining = state.write_buf.size() - state.write_offset;
    if (remaining > 0) {
        ssize_t w = write(state.fd, state.write_buf.data() + state.write_offset, remaining);
        if (w <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            return true;
        }
        state.write_offset += static_cast<size_t>(w);
        if (state.write_offset < state.write_buf.size()) return false;
    }

    // Phase 2: sendfile body if one was attached.
    // sendfile_offset = current file position; sendfile_size = bytes still to send.
    if (state.sendfile_fd >= 0) {
        while (state.sendfile_size > 0) {
#ifdef __APPLE__
            off_t chunk = state.sendfile_size;
            int r = sendfile(state.sendfile_fd, state.fd, state.sendfile_offset, &chunk, nullptr, 0);
            if (chunk > 0) {
                state.sendfile_offset += chunk;
                state.sendfile_size   -= chunk;
            }
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
                close(state.sendfile_fd);
                state.sendfile_fd = -1;
                return true;
            }
#else
            ssize_t sent = sendfile(state.fd, state.sendfile_fd,
                                    &state.sendfile_offset, static_cast<size_t>(state.sendfile_size));
            if (sent > 0) {
                state.sendfile_size -= sent;
            } else if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
                close(state.sendfile_fd);
                state.sendfile_fd = -1;
                return true;
            }
#endif
        }
        close(state.sendfile_fd);
        state.sendfile_fd = -1;
    }

    // Done writing everything.
    if (state.close_after_write) return true;
    reset_conn_to_reading(state, poller);
    return false;
}

void run_event_loop_worker(int wake_pipe_read, int shutdown_pipe_read,
                           std::queue<std::pair<int, std::string>>& client_queue,
                           std::mutex& queue_mutex, RequestHandler& handler,
                           int timeout_seconds, std::atomic<bool>& accepting_connections) {
    DefaultPoller poller;
    poller.add(wake_pipe_read,     true, false);
    poller.add(shutdown_pipe_read, true, false);

    std::unordered_map<int, ConnState> conns;
    char read_buf[8192];
    const size_t read_buf_size = sizeof(read_buf);

    while (true) {
        std::vector<PollEvent> events = poller.wait(1000);

        bool shutdown = false;
        for (const auto& ev : events) {
            if (ev.fd == shutdown_pipe_read && ev.readable) {
                char b;
                (void)read(shutdown_pipe_read, &b, 1);
                shutdown = true;
                break;
            }
        }
        if (shutdown) break;

        for (const auto& ev : events) {
            if (ev.fd == wake_pipe_read && ev.readable) {
                // Drain all wake bytes — each byte represents one new connection.
                char buf[64];
                ssize_t n;
                while ((n = read(wake_pipe_read, buf, sizeof(buf))) > 0) {
                    for (ssize_t i = 0; i < n; ++i) {
                        try_accept_from_queue(client_queue, queue_mutex, conns, poller);
                    }
                }
            }
        }

        time_t now = time(nullptr);
        std::vector<int> to_remove;

        for (const auto& ev : events) {
            if (ev.fd == wake_pipe_read || ev.fd == shutdown_pipe_read) continue;

            auto it = conns.find(ev.fd);
            if (it == conns.end()) continue;
            ConnState& state = it->second;

            if (state.phase == ConnState::Reading && ev.readable) {
                if (process_readable(state, read_buf, read_buf_size, handler, accepting_connections, poller)) {
                    to_remove.push_back(ev.fd);
                }
            } else if (state.phase == ConnState::Writing && ev.writable) {
                if (process_writable(state, poller)) {
                    to_remove.push_back(ev.fd);
                }
            }
        }

        // Timeout idle reading connections that haven't sent a complete request.
        if (timeout_seconds > 0) {
            for (auto& [fd, state] : conns) {
                if (state.phase == ConnState::Reading &&
                    (now - state.accepted_at) >= timeout_seconds) {
                    to_remove.push_back(fd);
                }
            }
        }

        for (int fd : to_remove) {
            auto it = conns.find(fd);
            if (it != conns.end()) {
                if (it->second.sendfile_fd >= 0) close(it->second.sendfile_fd);
                poller.remove(fd);
                close(fd);
                conns.erase(it);
            }
        }
    }

    for (auto& [fd, state] : conns) {
        if (state.sendfile_fd >= 0) close(state.sendfile_fd);
        close(fd);
    }
}
}  // namespace

int HttpServer::run_select_loops(int listen_fd, int shutdown_pipe_read, RequestHandler& handler) {
    int wake_pipe[2];
    if (pipe(wake_pipe) < 0) {
        std::cerr << "pipe failed\n";
        return 1;
    }

    {
        int flags = fcntl(wake_pipe[0], F_GETFL, 0);
        if (flags >= 0) (void)fcntl(wake_pipe[0], F_SETFL, flags | O_NONBLOCK);
    }

    std::queue<std::pair<int, std::string>> client_queue;
    std::mutex queue_mutex;

    const int n_workers = std::max(1, config.n_select_loops);
    const int timeout_seconds = config.timeout_seconds;

    std::thread acceptor(acceptor_loop_select, listen_fd, shutdown_pipe_read, wake_pipe[1],
                         std::ref(client_queue), std::ref(queue_mutex), std::ref(accepting_connections));

    std::vector<std::thread> workers;
    for (int w = 0; w < n_workers; w++) {
        workers.push_back(std::thread(run_event_loop_worker, wake_pipe[0], shutdown_pipe_read,
                                      std::ref(client_queue), std::ref(queue_mutex),
                                      std::ref(handler), timeout_seconds, std::ref(accepting_connections)));
    }

    acceptor.join();
    for (auto& t : workers) {
        t.join();
    }
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    return 0;
}

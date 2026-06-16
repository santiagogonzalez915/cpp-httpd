#include "server/server.hpp"

#include "http/connection/connection_manager.hpp"
#include "server/management_thread.hpp"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
void acceptor_loop_thread_pool(int listen_fd, int shutdown_pipe_read,
                               std::queue<std::pair<int, std::string>>& queue,
                               std::mutex& queue_mutex, std::condition_variable& queue_cv) {
    while (true) {
        auto [listen_readable, shutdown_requested] =
            wait_for_accept_or_shutdown(listen_fd, shutdown_pipe_read);
        if (shutdown_requested) {
            break;
        }
        if (listen_readable) {
            auto [client_fd, client_ip] = accept_connection(listen_fd);
            if (client_fd >= 0) {
                std::lock_guard<std::mutex> lock(queue_mutex);
                queue.push({client_fd, client_ip});
                queue_cv.notify_one();
            }
        }
    }
}

void worker_loop_thread_pool(std::queue<std::pair<int, std::string>>& queue,
                             std::mutex& queue_mutex, std::condition_variable& queue_cv,
                             RequestHandler& handler, int timeout_seconds,
                             std::atomic<bool>& accepting_connections) {
    while (true) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        while (queue.empty()) {
            queue_cv.wait(lock);
        }
        auto [client_fd, client_ip] = queue.front();
        queue.pop();
        lock.unlock();
        if (client_fd == -1) break;
        handle_connection(client_fd, client_ip, handler, accepting_connections, timeout_seconds);
        close(client_fd);
    }
}
}  // namespace

int HttpServer::run_thread_pool(int listen_fd, int shutdown_pipe_read, RequestHandler& handler) {
    std::queue<std::pair<int, std::string>> queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    std::thread acceptor_thread(acceptor_loop_thread_pool, listen_fd, shutdown_pipe_read,
                                std::ref(queue), std::ref(queue_mutex), std::ref(queue_cv));

    std::vector<std::thread> workers;
    const int timeout_seconds = config.timeout_seconds;
    for (int i = 0; i < config.n_threads; i++) {
        workers.push_back(std::thread(worker_loop_thread_pool, std::ref(queue), std::ref(queue_mutex),
                                      std::ref(queue_cv), std::ref(handler), timeout_seconds,
                                      std::ref(accepting_connections)));
    }

    acceptor_thread.join();
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        for (int i = 0; i < config.n_threads; i++) {
            queue.push({-1, ""});
        }
    }
    queue_cv.notify_all();
    for (auto& w : workers) {
        w.join();
    }
    return 0;
}


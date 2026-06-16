#include "server/server.hpp"

#include "http/connection/connection_manager.hpp"
#include "http/handlers/request_handler.hpp"
#include "server/management_thread.hpp"
#include "server/socket_setup.hpp"
#include "server/tls.hpp"

#include <iostream>
#include <thread>
#include <unistd.h>

HttpServer::HttpServer(const Config& config)
    : config(config),
    server_fd(-1),
    shutdown_requested(false),
    accepting_connections(true) {
    std::cout << "HttpServer initialized\n";
}

// Accept TLS connections on tls_fd in a simple thread-per-connection loop.
// Runs until the listen socket is closed.
static void tls_acceptor_loop(int tls_fd, SSL_CTX* ssl_ctx,
                               RequestHandler* handler,
                               std::atomic<bool>* accepting_connections,
                               int timeout_seconds) {
    while (true) {
        auto result = accept_connection(tls_fd);
        int         cfd = result.first;
        std::string cip = std::move(result.second);
        if (cfd < 0) break;
        std::thread([cfd, cip, ssl_ctx, handler, accepting_connections, timeout_seconds]() {
            handle_tls_connection(cfd, cip, ssl_ctx,
                                  *handler, *accepting_connections, timeout_seconds);
            close(cfd);
        }).detach();
    }
}

int HttpServer::run() {
    int port = config.listen_port;
    server_fd = create_listen_socket(port, 15);
    if (server_fd < 0) {
        server_fd = -1;
        return 1;
    }

    std::cout << "Server listening on port " << port << "\n";
    RequestHandler handler(config);

    int shutdown_pipe[2];
    if (pipe(shutdown_pipe) < 0) {
        std::cerr << "Error creating management thread \n";
        close(server_fd);
        server_fd = -1;
        return 1;
    }

    // Optional HTTPS listener — starts only when TLSCertFile + TLSKeyFile are set.
    int tls_fd = -1;
    SslServerContext ssl_ctx;
    std::thread tls_thread;

    if (!config.tls_cert_file.empty() && !config.tls_key_file.empty()) {
        if (!ssl_ctx.init(config.tls_cert_file, config.tls_key_file)) {
            std::cerr << "TLS init failed: " << SslServerContext::last_openssl_error() << "\n";
        } else {
            tls_fd = create_listen_socket(config.tls_port, 15);
            if (tls_fd >= 0) {
                std::cout << "HTTPS listening on port " << config.tls_port << "\n";
                tls_thread = std::thread(tls_acceptor_loop, tls_fd, ssl_ctx.ctx(),
                                         &handler, &accepting_connections,
                                         config.timeout_seconds);
            }
        }
    }

    std::thread mgmt_thread(management_thread_func, &accepting_connections, shutdown_pipe[1]);

    int result;
    if (config.mode == Config::Mode::Threads) {
        result = run_thread_pool(server_fd, shutdown_pipe[0], handler);
    } else {
        result = run_select_loops(server_fd, shutdown_pipe[0], handler);
    }

    if (server_fd >= 0) { close(server_fd); server_fd = -1; }
    if (tls_fd >= 0)    { close(tls_fd); tls_fd = -1; }
    if (tls_thread.joinable()) tls_thread.join();

    close(shutdown_pipe[0]);
    close(shutdown_pipe[1]);
    mgmt_thread.join();

    return result;
}

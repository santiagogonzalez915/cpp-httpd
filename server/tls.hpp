#ifndef TLS_HPP
#define TLS_HPP

#include <atomic>
#include <openssl/ssl.h>
#include <string>

class RequestHandler;

// Owns an SSL_CTX* for server-side TLS. One instance shared across all workers.
class SslServerContext {
    SSL_CTX* ctx_ = nullptr;

public:
    SslServerContext() = default;
    ~SslServerContext();

    SslServerContext(const SslServerContext&)            = delete;
    SslServerContext& operator=(const SslServerContext&) = delete;

    bool init(const std::string& cert_path, const std::string& key_path);
    SSL_CTX* ctx() const { return ctx_; }
    static std::string last_openssl_error();
};

// Mirrors handle_connection() but uses SSL_read / SSL_write throughout.
// sendfile() cannot be used over TLS — file bodies are read in 64 KB chunks
// and passed through SSL_write.
void handle_tls_connection(int client_fd, const std::string& client_ip,
                           SSL_CTX* ssl_ctx,
                           RequestHandler& handler,
                           std::atomic<bool>& accepting_connections,
                           int timeout_seconds);

#endif

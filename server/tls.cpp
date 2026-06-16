#include "server/tls.hpp"

#include "http/connection/connection_manager.hpp"
#include "http/core/http_request.hpp"
#include "http/core/http_response.hpp"
#include "http/core/request_parser.hpp"
#include "http/handlers/request_handler.hpp"
#include "http/core/request_handler_utils.hpp"

#include <cerrno>
#include <cstring>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

// ── SslServerContext ──────────────────────────────────────────────────────────

SslServerContext::~SslServerContext() {
    if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
}

bool SslServerContext::init(const std::string& cert_path, const std::string& key_path) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return false;

    // TLS 1.2+ only — reject obsolete versions.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(ctx, cert_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx); return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx); return false;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        SSL_CTX_free(ctx); return false;
    }

    ctx_ = ctx;
    return true;
}

std::string SslServerContext::last_openssl_error() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return buf;
}

// ── TLS I/O helpers ───────────────────────────────────────────────────────────

namespace {

// Write all bytes to the TLS channel; returns false on error.
bool ssl_write_all(SSL* ssl, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        int w = SSL_write(ssl, data + written, static_cast<int>(len - written));
        if (w <= 0) {
            int err = SSL_get_error(ssl, w);
            if (err == SSL_ERROR_WANT_WRITE) continue;
            return false;
        }
        written += static_cast<size_t>(w);
    }
    return true;
}

bool ssl_write_all(SSL* ssl, const std::string& s) {
    return ssl_write_all(ssl, s.data(), s.size());
}

// Send response through TLS. File bodies are read in chunks — sendfile()
// operates at the kernel level and cannot encrypt data in transit.
void ssl_write_response(SSL* ssl, ResponsePackage& pkg) {
    ssl_write_all(ssl, pkg.bytes);

    if (pkg.body_fd < 0) return;

    static constexpr size_t CHUNK = 65536;
    std::vector<char> buf(CHUNK);
    off_t remaining = pkg.body_fd_size;

    if (pkg.body_fd_offset > 0) {
        lseek(pkg.body_fd, pkg.body_fd_offset, SEEK_SET);
    }

    while (remaining > 0) {
        size_t to_read = static_cast<size_t>(remaining < (off_t)CHUNK ? remaining : CHUNK);
        ssize_t n = read(pkg.body_fd, buf.data(), to_read);
        if (n <= 0) break;
        if (!ssl_write_all(ssl, buf.data(), static_cast<size_t>(n))) break;
        remaining -= n;
    }

    close(pkg.body_fd);
    pkg.body_fd = -1;
}

bool ssl_wants_close(const HttpRequest& req) {
    std::string conn = get_header(req.headers, "connection");
    return !conn.empty() && to_lower(conn).find("close") != std::string::npos;
}

}  // namespace

// ── handle_tls_connection ─────────────────────────────────────────────────────

void handle_tls_connection(int client_fd, const std::string& client_ip,
                           SSL_CTX* ssl_ctx,
                           RequestHandler& handler,
                           std::atomic<bool>& accepting_connections,
                           int timeout_seconds) {
    // Apply socket read timeout for the TLS handshake and subsequent reads.
    if (timeout_seconds > 0) {
        struct timeval tv { timeout_seconds, 0 };
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    SSL* ssl = SSL_new(ssl_ctx);
    if (!ssl) return;
    SSL_set_fd(ssl, client_fd);

    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        return;
    }

    char read_buf[8192];
    RequestParser parser;

    while (true) {
        bool got_request = false;
        bool closed      = false;

        if (parser.has_request()) {
            got_request = true;
        } else if (parser.has_error()) {
            StatusCode code = (parser.get_error() == ParseError::PayloadTooLarge)
                ? StatusCode::PayloadTooLarge : StatusCode::BadRequest;
            HttpResponse err(code);
            err.headers["Connection"] = "close";
            ssl_write_all(ssl, err.to_bytes());
            break;
        }

        while (!got_request && !closed) {
            int n = SSL_read(ssl, read_buf, static_cast<int>(sizeof(read_buf)));
            if (n == 0) { closed = true; break; }
            if (n < 0) {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ) continue;
                closed = true; break;
            }
            parser.feed(read_buf, static_cast<size_t>(n));
            if      (parser.has_request()) got_request = true;
            else if (parser.has_error()) {
                StatusCode code = (parser.get_error() == ParseError::PayloadTooLarge)
                    ? StatusCode::PayloadTooLarge : StatusCode::BadRequest;
                HttpResponse err_resp(code);
                err_resp.headers["Connection"] = "close";
                ssl_write_all(ssl, err_resp.to_bytes());
                closed = true; break;
            }
        }

        if (closed || !got_request) break;

        HttpRequest req = parser.get_request();
        bool wants_close = ssl_wants_close(req);

        HttpResponse resp = handler.handle(req, accepting_connections.load(),
                                           client_ip.empty() ? nullptr : &client_ip);
        resp.headers["Connection"] = wants_close ? "close" : "keep-alive";

        ResponsePackage pkg;
        pkg.close_after = wants_close;
        if (resp.body_fd >= 0) {
            pkg.bytes          = resp.headers_to_bytes();
            pkg.body_fd        = resp.body_fd;
            pkg.body_fd_offset = resp.body_fd_offset;
            pkg.body_fd_size   = resp.body_fd_size;
            resp.body_fd       = -1;
        } else {
            pkg.bytes = resp.to_bytes();
        }

        ssl_write_response(ssl, pkg);

        if (pkg.close_after) break;
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
}

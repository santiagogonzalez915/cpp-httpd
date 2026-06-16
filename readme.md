# cpp-httpd

A C++17 HTTP/1.1 server built from the OS up. Supports static file serving, virtual hosting, CGI execution, Basic Auth, persistent connections, kqueue/epoll I/O multiplexing, `sendfile()` zero-copy transfer, and TLS via OpenSSL — two concurrency models selectable at config time.

Benchmarked at **73% of nginx throughput** (28.5k vs 38.9k RPS) on 4 KB static files, and **matches nginx on 1 MB files** where both modes are memory-bandwidth bound.

---

## Features

- HTTP/1.1 GET, POST, HEAD, OPTIONS
- Range requests (206 Partial Content) with `sendfile()` offset
- ETag generation (`inode-size-mtime`) and `If-None-Match` → 304
- `Cache-Control: max-age=3600` on static responses
- Virtual hosting — `Host` header selects DocumentRoot
- CGI `fork`/`exec` with configurable subprocess timeout (504 on hang)
- Basic Auth via `.htaccess`
- Keep-alive with per-connection read timeout
- Auto-generated directory listings via `opendir`/`readdir`
- Apache Combined Log Format access logging to configurable file
- Path traversal protection
- 10 MB request body cap (413 Payload Too Large)
- TLS 1.2+ via OpenSSL — HTTP and HTTPS run on separate ports from one process
- kqueue (macOS) / epoll (Linux) I/O multiplexing replacing `select()`
- Thread-pool mode (mutex + queue, blocking I/O) as an alternative
- ASAN + UBSan build targets
- Catch2 unit tests (138 assertions across 22 test cases)
- libFuzzer target for `RequestParser` + ASAN mutation driver (2 000 mutations, 0 crashes)

---

## Directory structure

```
cpp-httpd/
├── config/                    # Config file parser
│   ├── config.cpp
│   └── config.hpp
├── http/
│   ├── core/                  # Request/response types, parser, logger, utils
│   │   ├── http_request.cpp/hpp
│   │   ├── http_response.cpp/hpp
│   │   ├── request_parser.cpp/hpp
│   │   ├── request_handler_utils.cpp/hpp
│   │   ├── logger.hpp         # Thread-safe Apache Combined Log Format writer
│   │   ├── phase_context.hpp
│   │   └── auth_result.hpp
│   ├── handlers/              # Phase handlers
│   │   ├── request_handler.cpp/hpp   # Pipeline orchestrator
│   │   ├── auth_handler.cpp/hpp
│   │   ├── static_file_handler.cpp/hpp
│   │   ├── directory_handler.cpp/hpp
│   │   └── cgi_handler.cpp/hpp
│   ├── connection/
│   │   └── connection_manager.cpp/hpp
│   └── routing/
│       └── vhost_resolver.cpp/hpp
├── server/
│   ├── server.cpp/hpp         # Top-level: listen socket + optional TLS listener
│   ├── socket_setup.cpp/hpp
│   ├── management_thread.cpp/hpp
│   ├── thread_pool_mode.cpp   # Blocking I/O, mutex + queue
│   ├── select_mode.cpp        # kqueue/epoll event loop
│   ├── poller.hpp             # Poller abstraction (KqueuePoller / EpollPoller)
│   ├── tls.cpp/hpp            # SslServerContext + handle_tls_connection()
│   └── management_thread.cpp/hpp
├── fuzz/
│   ├── fuzz_request_parser.cpp  # libFuzzer entry point
│   ├── fuzz_driver.cpp          # Standalone ASAN stdin driver (AFL++ compatible)
│   └── corpus/                  # Seed inputs (12 cases)
├── tests/
│   ├── test_request_parser.cpp
│   └── test_http_response.cpp
├── bench/
│   ├── plot_results.py          # matplotlib benchmark charts
│   ├── rps_comparison.png
│   ├── latency_comparison.png
│   └── efficiency.png
├── scripts/
│   └── gen_cert.sh              # Self-signed TLS cert generator
├── tls/
│   ├── cert.pem
│   └── key.pem
├── .github/workflows/ci.yml     # Build, test, ASAN, fuzz corpus, clang-tidy
├── main.cpp
├── Makefile
└── httpd.conf                   # Sample config
```

---

## Architecture

```
                  ┌─────────────────────────────────────┐
                  │             HttpServer               │
                  │  listen socket  +  acceptor thread   │
                  │  optional TLS socket (port 4443)     │
                  └──────────────┬──────────────────────┘
                                 │ accept()
                  ┌──────────────▼──────────────────────┐
                  │          Concurrency Layer           │
                  │  ┌─────────────────┐                │
                  │  │  Thread Pool    │  mutex+queue,  │
                  │  │  (N workers)    │  blocking I/O  │
                  │  └─────────────────┘                │
                  │  ┌─────────────────┐                │
                  │  │  kqueue/epoll   │  event loop,  │
                  │  │  Event Loop     │  nonblocking  │
                  │  └─────────────────┘                │
                  └──────────────┬──────────────────────┘
                                 │ raw bytes
                  ┌──────────────▼──────────────────────┐
                  │           RequestParser              │
                  │  incremental, keep-alive aware       │
                  └──────────────┬──────────────────────┘
                                 │ HttpRequest
                  ┌──────────────▼──────────────────────┐
                  │         4-Phase Pipeline             │
                  │                                      │
                  │  1. FindConfig  ── VHostResolver     │
                  │       │                              │
                  │  2. Access      ── AuthHandler       │
                  │       │                              │
                  │  3. Content     ── StaticFile        │
                  │                    Directory         │
                  │                    CGI               │
                  │       │                              │
                  │  4. Log         ── Logger (file)     │
                  └──────────────┬──────────────────────┘
                                 │ HttpResponse
                  ┌──────────────▼──────────────────────┐
                  │         Response Dispatch            │
                  │  sendfile() for static files         │
                  │  SSL_write() for TLS connections     │
                  │  chunked encoding for CGI output     │
                  └─────────────────────────────────────┘
```

---

## Main classes

| Class / Struct | Role |
|---|---|
| **Config** | Holds listen port, virtual hosts, concurrency mode, thread/loop counts, timeout, log file path, and TLS cert/key paths. Loaded from a config file. |
| **HttpServer** | Creates the listen socket, optionally starts a TLS listener on a second port, starts the management thread, and dispatches to thread-pool or kqueue/epoll workers. |
| **RequestParser** | Incrementally parses raw bytes into `HttpRequest`: request line, headers, and body. Handles keep-alive by yielding one request per `reset()` cycle. Enforces a 10 MB body cap. |
| **HttpRequest** | Parsed request: method (GET/POST/HEAD/OPTIONS), URI, headers map, content length, body. |
| **HttpResponse** | Response: status code, headers map, body string, and `body_fd` for sendfile transfer. `headers_to_bytes()` separates header serialization from body for the sendfile path. |
| **PhaseContext** | Per-request context threaded through the 4-phase pipeline. Any phase may set `response_ready` to short-circuit the rest. |
| **RequestHandler** | Orchestrates the FindConfig → Access → Content → Log pipeline. Owns a `Logger` instance. |
| **VHostResolver** | Matches the `Host` header to a `VirtualHost` and maps the URI to a filesystem path, enforcing DocumentRoot confinement. |
| **AuthHandler** | Reads `.htaccess`; validates `Authorization: Basic` against stored credentials. Returns 401 with `WWW-Authenticate` on failure. |
| **StaticFileHandler** | Serves files via `open()` + `sendfile()`. Generates ETags (`inode-size-mtime`), honors `If-None-Match` (304) and `If-Modified-Since`, handles `Range` headers (206). Sets `Cache-Control: max-age=3600`. |
| **DirectoryHandler** | Serves `index.html` / `index_m.html` if present; otherwise generates an HTML directory listing via `opendir`/`readdir` with size, mtime, and sorted entries. |
| **CgiHandler** | Builds the CGI environment, `fork`/`exec`s the script, pipes the POST body to stdin, and uses `select()` with a timeout on the child's stdout — returns 504 if the subprocess hangs. |
| **Logger** | Thread-safe Apache Combined Log Format writer (`std::ofstream` + `std::mutex`). Falls back to stderr if `LogFile` is not configured. |
| **Poller** | Abstract interface over `kqueue` (macOS) and `epoll` (Linux). `KqueuePoller` and `EpollPoller` implement `add`, `modify`, `remove`, `wait`. Selected at compile time via `#ifdef __APPLE__`. |
| **SslServerContext** | Owns an `SSL_CTX*`. Loads cert/key, enforces TLS 1.2+. Workers call `handle_tls_connection()` which mirrors the plain path but uses `SSL_read`/`SSL_write`. |
| **ConnState** | Per-connection state in the event loop: fd, parser, read/write phase, write buffer, and `sendfile_fd`/`sendfile_offset`/`sendfile_size` for non-blocking file transfer. |

---

## Server flow

1. **Startup** — `main` loads `Config` from the path given on the command line, resolves relative DocumentRoots to absolute paths, and constructs `HttpServer(config)`.

2. **Server run** — `HttpServer::run()` creates the listen socket and, if `TLSCertFile` and `TLSKeyFile` are set, a second listen socket on `TLSPort`. A management thread reads stdin for `shutdown`. Workers enter either thread-pool or kqueue/epoll mode.

3. **Accept and connection handling** — Each accepted client is handled by `handle_connection` (plain) or `handle_tls_connection` (TLS). The connection manager reads bytes, feeds them to `RequestParser`. On a complete request it calls `RequestHandler::handle` and dispatches the response. For plain connections, static file bodies are sent via `sendfile()`; for TLS, the file is read in 64 KB chunks through `SSL_write()` since the kernel cannot encrypt in transit. Keep-alive is supported on both paths.

4. **4-phase pipeline** — `RequestHandler` builds a `PhaseContext` and runs:
   - **FindConfig** — `VHostResolver` picks the `VirtualHost` from the `Host` header and splits the URI into path and query string.
   - **Access** — `resolve_path` maps the URI to a filesystem path and enforces DocumentRoot confinement. The path is `stat()`'d; missing paths yield 404, traversal yields 403. `AuthHandler` checks `.htaccess` and returns 401 on bad credentials.
   - **Content** — Executable files go to `CgiHandler`. GET/HEAD to a regular file goes to `StaticFileHandler` (ETag, Range, sendfile). HEAD clears the body while preserving headers. Directories go to `DirectoryHandler`. OPTIONS returns 204 + `Allow`. POST to a non-CGI resource returns 405.
   - **Log** — `Logger` writes one Apache Combined Log Format line.

5. **Response** — `HttpResponse::headers_to_bytes()` is sent first; if `body_fd >= 0`, `sendfile()` (or `SSL_write` for TLS) transfers the file body without copying it through userspace.

---

## Demo

The `demo/` directory contains a self-contained site that exercises every feature from a browser. Run it from the project root:

```bash
make
./build/server_bin demo/demo.conf
# open http://localhost:6789
```

| Page | URL | What it demonstrates |
|---|---|---|
| Homepage | `/` | Feature index |
| Static files | `/static/` | ETag, `Cache-Control`, `sendfile()` — inspect headers in DevTools |
| Server status | `/cgi-bin/status.sh` | CGI `fork`/`exec`; shows live request count from access log |
| Request inspector | `/cgi-bin/inspect.sh` | Parser output via CGI env; try `?key=value` query strings |
| Admin area | `/admin/` | Basic Auth — browser prompts for credentials (`admin` / `demo`) |
| Directory listing | `/files/` | Auto-generated via `opendir`/`readdir`; no `index.html` in that dir |

Access log writes to `/tmp/cpp-httpd-demo-access.log`. The status page reads line count from that file on every request.

---

## Build

```bash
make              # standard build → build/server_bin
make asan         # AddressSanitizer + UBSan → build/server_bin_asan
make ubsan        # UBSan only → build/server_bin_ubsan
make test         # Catch2 unit tests (requires: brew install catch2)
make fuzz_driver  # ASAN stdin driver (AFL++-compatible)
make fuzz_parser  # libFuzzer target (requires: brew install llvm)
```

Run:

```bash
./build/server_bin httpd.conf
```

---

## Configuration

```apache
Listen        6789
nThreads      4          # thread-pool mode; use nSelectLoops for kqueue/epoll
Timeout       10
LogFile       /var/log/cpp-httpd/access.log

# TLS — all three required to enable HTTPS
TLSCertFile   tls/cert.pem
TLSKeyFile    tls/key.pem
TLSPort       4443

<VirtualHost *:6789>
  DocumentRoot /var/www/html
  ServerName   example.com
</VirtualHost>
```

Generate a self-signed cert for local use:

```bash
bash scripts/gen_cert.sh
```

---

## Performance

Measured on Apple M-series, macOS 15. `wrk -t4 -c50 -d15s`. See `bench/` for charts.

| Mode | File | RPS | Avg latency | vs nginx |
|---|---|---|---|---|
| Thread-pool (blocking I/O) | 4 KB | 23,690 | 0.17 ms | 61% |
| kqueue + sendfile() | 4 KB | 28,451 | 1.68 ms | **73%** |
| kqueue + sendfile() | 1 MB | 2,699 | 17.6 ms | **101%** |
| nginx 1.31.1 baseline | 4 KB | 38,895 | 1.23 ms | 100% |
| nginx 1.31.1 baseline | 1 MB | 2,680 | 17.9 ms | 100% |

The 1 MB result shows `sendfile()` working as intended: both server and nginx are constrained by memory bandwidth, not software overhead.

The thread-pool mode reports lower average latency on 4 KB files than the kqueue mode at this concurrency level — with 50 connections and 4 threads, blocking I/O rarely queues. The kqueue mode is expected to outperform the thread-pool as concurrency scales past the thread count.

---

## Testing

```bash
make test
# build/test_runner
# All tests passed (138 assertions in 22 test cases)
```

Covers: parser state machine, keep-alive reset, incremental feeding, error paths (PayloadTooLarge, MethodNotAllowed, malformed lines), caching headers, response serialization, HEAD/OPTIONS behavior, path traversal, null bytes in body.

### Fuzzing

```bash
# ASAN stdin driver — run every corpus seed:
make fuzz_driver
for f in fuzz/corpus/*.txt; do ./build/fuzz_driver < "$f"; done

# libFuzzer (requires brew install llvm):
make fuzz_parser
./build/fuzz_parser fuzz/corpus/ -max_total_time=300
```

2 000 randomly mutated inputs under ASAN produced 0 crashes.

---

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and pull request:

- `make` — build
- `make test` — unit tests
- `make asan` — sanitizer build
- ASAN integration smoke test (GET, HEAD, OPTIONS over live server)
- Fuzz corpus run through ASAN driver
- `clang-tidy` (advisory)

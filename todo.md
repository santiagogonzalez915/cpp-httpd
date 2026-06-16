# HTTP Server Improvement Roadmap

## Goal

Transform this project from a university assignment into a portfolio-quality systems project that demonstrates OS, kernel, and concurrency knowledge — suitable for low-level, systems, and high-performance new grad roles.

---

# Phase 1: Repository and Build Infrastructure

## Create a Dedicated Repository

Move from `cpsc4330/http_server` to a standalone repo named `cpp-httpd`.

Avoid names like `mini-nginx` — that implies imitation. `cpp-httpd` implies authorship.

## Rewrite README

### Overview

```
A C++17 HTTP/1.1 server supporting static file serving, virtual hosting, CGI
execution, Basic Authentication, persistent connections, kqueue/epoll I/O
multiplexing, and sendfile() zero-copy file transfer.
```

### Architecture Diagram

The diagram must reflect the real implementation, not a simplified version:

```
                  ┌─────────────────────────────────────┐
                  │             HttpServer               │
                  │  listen socket  +  acceptor thread   │
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
                  │  1. FindConfig  -- VHostResolver     │
                  │       |                              │
                  │  2. Access      -- AuthHandler       │
                  │       |                              │
                  │  3. Content     -- StaticFile        │
                  │                    Directory         │
                  │                    CGI               │
                  │       |                              │
                  │  4. Log         -- access log file   │
                  └──────────────┬──────────────────────┘
                                 │ HttpResponse
                  ┌──────────────▼──────────────────────┐
                  │         Response Dispatch            │
                  │  sendfile() for static files         │
                  │  chunked encoding for CGI output     │
                  └─────────────────────────────────────┘
```

### Features

- Static file serving with ETag, Cache-Control, If-Modified-Since
- Range requests (206 Partial Content)
- HTTP/1.1 Keep-Alive with timeout and per-connection request limits
- CGI execution with subprocess timeout enforcement
- Basic Authentication via `.htaccess`
- Virtual hosting
- kqueue (macOS) / epoll (Linux) I/O multiplexing
- Thread pool mode (alternative concurrency model)
- sendfile() zero-copy file transfer
- Auto-generated directory listings
- Custom error pages
- Path traversal protection
- Apache Combined Log Format access logging

### Build

```bash
make           # standard build
make asan      # AddressSanitizer + UBSan build
./build/server_bin config/server.conf
```

### Performance

Measured on Apple M-series (macOS), `wrk -t4 -c50 -d15s`.

| Mode | File | RPS | Avg Latency | vs nginx |
| ---- | ---- | --- | ----------- | -------- |
| thread-pool | 4 KB | 23,690 | 165 µs | 61% |
| kqueue + sendfile | 4 KB | 28,451 | 1.68 ms | **73%** |
| kqueue + sendfile | 1 MB | 2,699 | 17.6 ms | 101% |
| nginx 1.31.1 baseline | 4 KB | 38,895 | 1.23 ms | 100% |
| nginx 1.31.1 baseline | 1 MB | 2,680 | 17.9 ms | 100% |

**kqueue + sendfile() matches nginx throughput on 1 MB files and reaches 73% on 4 KB.**

## Add Sanitizer Build Targets

Wire these into the Makefile now, not at the CI phase:

```makefile
asan:
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer ...

ubsan:
	$(CXX) $(CXXFLAGS) -fsanitize=undefined ...
```

Running `make asan` throughout development catches bugs as they are introduced.

## Add CMakeLists.txt

Add CMake alongside the existing Makefile. Many systems shops use CMake and it
unlocks easy integration of Catch2 and libFuzzer targets without Makefile
gymnastics.

---

# Phase 2: HTTP Protocol Completeness ✓ DONE

## Add HEAD Method

```http
HEAD /index.html HTTP/1.1
```

- Same headers as GET
- No response body
- Required for HTTP/1.1 compliance (RFC 7231)

## Add OPTIONS Method

```http
OPTIONS / HTTP/1.1
```

Returns:

```http
HTTP/1.1 204 No Content
Allow: GET, HEAD, POST, OPTIONS
```

Trivial to implement; completes HTTP/1.1 method coverage.

## Add ETag and Cache-Control Headers

The static file handler already sets `Last-Modified`. Complete the caching story:

- Generate ETag as `"<inode>-<size>-<mtime>"` (same formula Apache uses)
- Honor `If-None-Match` header; return 304 if ETag matches
- Add `Cache-Control: max-age=3600` for static assets

## Add Range Requests

```http
Range: bytes=0-1023
```

Returns:

```http
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-1023/4096
```

Benefits:

- Real-world HTTP feature (video streaming, download resumption)
- Natural stepping stone to sendfile() with offset in Phase 3
- Strong interview discussion topic

## Improve Keep-Alive Behavior

- Configurable connection timeout
- Maximum requests per connection limit
- Proper cleanup on timeout

## Enforce Request Limits

- Maximum header count
- Maximum header line size
- Maximum body size (Content-Length cap)

Return appropriate errors before any allocation:

```http
413 Payload Too Large
400 Bad Request
```

---

# Phase 3: OS Primitives ✓ DONE

This is the highest-signal phase for systems engineering roles. Do this before
testing so the event loop is stable when tests are written.

## Replace select() with kqueue/epoll

The existing `select_mode.cpp` has a clean per-worker loop but hits two hard
limits: the 1024-fd ceiling and O(n) fd_set rebuild on every call.

Approach:

- Abstract behind a `Poller` interface with `add_fd`, `modify_fd`, `remove_fd`,
  `wait` methods
- Provide `KqueuePoller` (macOS) and `EpollPoller` (Linux), selected at compile
  time via `#ifdef __APPLE__`
- `run_select_worker` becomes `run_event_loop_worker`; `ConnState` structure is
  unchanged

Key talking point: kqueue and epoll are O(1) per event, have no fd ceiling, and
return only the fds that are ready — no scanning required.

Files to modify:

- `server/select_mode.cpp` — primary rewrite target
- `server/server.hpp` — add `Poller` base class

## Add sendfile() Zero-Copy

Currently, static files are loaded into a heap `std::string` via `read()`, then
copied into the response buffer, then `write()`d. Three copies.

`sendfile()` lets the kernel move bytes from the page cache directly to the
socket buffer with zero user-space copies.

- macOS: `sendfile(fd, sockfd, offset, &len, nullptr, 0)`
- Linux: `sendfile(out_fd, in_fd, &offset, count)`

Implementation notes:

- Headers must be sent first (separate `write()` or `writev()`)
- Add a flag to `PhaseContext` so the event loop sends the file fd directly
  rather than buffering the body
- The Range implementation from Phase 2 makes offset-based sendfile natural

File to modify: `http/handlers/static_file_handler.cpp` — replace
`read_file_to_string` call with sendfile path.

## Fix CGI Subprocess Timeout

Currently `read_child_output` blocks forever on `read()`. A hung CGI script
hangs the entire worker.

Fix: after `fork()`, use `select()` with a configurable timeout on the CGI
stdout pipe fd. On timeout: `kill(pid, SIGKILL)`, `waitpid`, return 504
Gateway Timeout.

File to modify: `http/handlers/cgi_handler.cpp` — `read_child_output` function.

---

# Phase 4: Security and Hardening ✓ DONE

## Fuzz the HTTP Parser with libFuzzer

The `RequestParser` is the attack surface for every connection. Create a fuzz
target:

```cpp
// fuzz/fuzz_request_parser.cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    RequestParser p;
    p.feed(reinterpret_cast<const char*>(data), size);
    return 0;
}
```

Build:

```bash
clang++ -std=c++17 -fsanitize=fuzzer,address -I. -Ihttp \
    fuzz/fuzz_request_parser.cpp http/core/request_parser.cpp ... \
    -o fuzz/fuzz_parser

mkdir fuzz/corpus && cp http-meeting\ 2/test-cases.txt fuzz/corpus/
./fuzz/fuzz_parser fuzz/corpus/
```

Any crash under AddressSanitizer is a real bug. Run until no new crashes appear
for 30 minutes.

## Run Clean Under ASAN + UBSan

Start the server with `make asan`, run the integration test suite and wrk load
test against it, and resolve every sanitizer finding before proceeding.

---

# Phase 5: Observability ✓ DONE

## Access Log to File

Add Apache Combined Log Format logging to a configurable file:

```
127.0.0.1 - - [10/Oct/2000:13:55:36 -0700] "GET /index.html HTTP/1.1" 200 2326
```

Implementation:

- Add `Logger` class with `std::ofstream` + `std::mutex`
- Add `LogFile /var/log/httpd/access.log` directive to config parser
- Fall back to stderr if `LogFile` is not configured (preserves current behavior)
- Use `O_APPEND` when opening — POSIX guarantees single-write atomicity for
  appends, which avoids log corruption at the kernel level

The log file enables `wc -l access.log` as a ground-truth request count after
benchmarking runs, independent of wrk's own counters.

---

# Phase 6: Directory Listing ✓ DONE

Replace the current 404-on-missing-index with an auto-generated HTML listing.

Use `opendir`/`readdir` to enumerate directory entries, then emit a minimal HTML
table with filename, file size, and last-modified date. Sort directories first,
then files alphabetically.

This fills the most visible gap in the server's behavior and demonstrates that
`DirectoryHandler` is fully implemented, not a stub.

---

# Phase 7: Testing ✓ DONE

## Unit Tests with Catch2

Add Catch2 v3 via `FetchContent` in CMakeLists.txt (no source copy needed).
Test target: `make test` runs all unit and integration tests.

### HTTP Parser

- GET and POST round-trips
- Malformed request lines
- Missing CRLF, extra whitespace
- Oversized headers (assert 400, not a crash)
- Multiple requests on one parser instance (keep-alive pipelining)

### Caching Headers

- ETag generation is deterministic for same file
- If-None-Match matching returns 304 with no body
- Range response byte offsets are exact

### Security

- Path traversal: `GET /../../../etc/passwd` resolves to Forbidden
- Auth: missing credentials vs. wrong credentials vs. correct credentials
- Request limits: body exceeding max size returns 413

### Responses

- HEAD response has identical headers to GET but zero body bytes
- OPTIONS response includes correct `Allow` header

## Integration Tests

Use Python's `http.client` (no extra dependency). Start the server as a
subprocess, fire real HTTP requests over TCP, assert on response codes and
headers.

Cover:

- Static file round-trip (serve file, assert Content-Type and body)
- CGI execution (POST body piped to stdin, assert output in response)
- Keep-Alive (two requests on one TCP connection, assert both succeed)
- Range request (assert 206, Content-Range header, exact byte slice)
- Authentication (assert 401 on missing credentials, 200 on correct)

---

# Phase 8: Benchmarking ✓ DONE

## Tools

- `wrk` (primary)
- `ab` (secondary, for comparison)

## Methodology

Run nginx on the same machine serving the same static file as a baseline. Without
a baseline, the numbers are uninterpretable.

```bash
wrk -t4 -c200 -d30s --latency http://localhost:8080/test4k.html
```

## Results Table

Measured on Apple M-series (macOS), 4 wrk threads, 50 connections, 15s run.
`-` = not tested in this configuration.

| Mode | Connections | File | RPS | Avg Latency | Transfer/s |
| ---- | ----------- | ---- | --- | ----------- | ---------- |
| thread-pool | 50 | 4 KB | 23,690 | 165 µs | 98 MB/s |
| thread-pool | 50 | 1 MB | 2,503 | 1.56 ms | 2.44 GB/s |
| kqueue + sendfile | 50 | 4 KB | 28,451 | 1.68 ms | 118 MB/s |
| kqueue + sendfile | 50 | 1 MB | 2,699 | 17.6 ms | 2.64 GB/s |
| **nginx baseline** | 50 | 4 KB | **38,895** | 1.23 ms | 161 MB/s |
| **nginx baseline** | 50 | 1 MB | **2,680** | 17.9 ms | 2.62 GB/s |

**Takeaways:**
- kqueue + sendfile achieves **73% of nginx RPS** on 4 KB files
- On 1 MB files, cpp-httpd **matches nginx** (2,699 vs 2,680 RPS) — both are I/O bound
- Thread-pool achieves 61% of nginx on 4 KB at this concurrency level
- At 1 MB the thread-pool's blocking I/O still performs well (latency-dominated by transfer)

Add headline to README: "Achieves 73% of nginx throughput (28.5k vs 38.9k RPS)
on 4 KB static files at 50 concurrent connections using kqueue + sendfile()."

---

# Phase 9: CI/CD ✓ DONE

## GitHub Actions

### Build and Test

```yaml
- run: make
- run: make test
```

### Sanitizer Build

```yaml
- run: make asan
- run: ./build/server_bin_asan config/server.conf &
- run: python3 tests/integration/run_all.py
```

### Static Analysis

```yaml
- run: clang-tidy http/**/*.cpp -- -std=c++17 -I. -Ihttp
- run: cppcheck --enable=all --std=c++17 http/ server/ config/
```

### Fuzzing (nightly or manual trigger)

```yaml
- run: make fuzz_parser
- run: ./fuzz/fuzz_parser fuzz/corpus/ -max_total_time=300
```

---

# Phase 10: Demo Website

The website exists only to demonstrate server functionality. Do not build a
frontend application.

## Pages

### Homepage

Links to all demos. Lists implemented features with checkmarks.

### Static File Demo

Serve HTML, CSS, JS, images, and a downloadable file. Demonstrates MIME type
handling and caching headers (inspect with DevTools).

### CGI Status Page

Display:

- Current time (generated dynamically)
- Server PID
- Request count from access log

Demonstrates dynamic content via fork/exec.

### Authentication Demo

Protect `/admin` with Basic Auth. Show the browser credential prompt.

### Directory Listing Demo

Point a route at a directory with no index.html. Show the auto-generated listing.

### Request Inspector

CGI script that echoes the parsed request back as HTML: method, path, headers,
User-Agent. Visibly demonstrates parser correctness.

---

# Optional Enhancements — Phase 11 (TLS ✓ DONE)

Only consider these after completing Phases 1–9.

## TLS / HTTPS via OpenSSL

Wrap the socket layer with SSL_read/SSL_write. Requires restructuring the
connection layer to hold an SSL context per fd. High effort; real-world signal.

## HTTP/2 via nghttp2

HTTP/2 support requires multiplexing streams within a single TCP connection — a
fundamentally different model from HTTP/1.1. nghttp2 provides the framing layer.
Use this as a stretch goal after TLS, since HTTP/2 requires HTTPS in browsers.

## io_uring (Linux Only)

io_uring replaces the kqueue/epoll event loop with a submission/completion ring
that eliminates syscall overhead for accept, read, write, and sendfile. Requires
Linux 5.1+ and liburing. Extremely impressive on a resume; a natural evolution
of the kqueue/epoll work.

---

# Resume Target

After completing Phases 1–8:

> Built a C++17 HTTP/1.1 server from scratch with a kqueue/epoll event loop
> replacing select(), sendfile() zero-copy for static file transfer, virtual
> hosting, CGI fork/exec with subprocess timeout enforcement, and configurable
> thread pool and event-loop concurrency modes. Hardened the HTTP parser with
> corpus-based mutation fuzzing under AddressSanitizer (2,000+ mutations,
> zero crashes); enforced 10 MB request size limits and byte-range (206)
> responses. Benchmarked at 73% of nginx throughput (28.5k vs 38.9k RPS) at
> 50 concurrent connections on 4 KB files; matches nginx on 1 MB files where
> both modes are transfer-bound.

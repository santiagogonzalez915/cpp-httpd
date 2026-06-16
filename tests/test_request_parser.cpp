#include <catch2/catch_all.hpp>
#include "core/request_parser.hpp"

// Feed a complete HTTP request string and return the parsed result.
static HttpRequest parse_one(const std::string& raw) {
    RequestParser p;
    p.feed(raw.data(), raw.size());
    REQUIRE(p.has_request());
    return p.get_request();
}

// Feed and expect a specific error.
static ParseError parse_err(const std::string& raw) {
    RequestParser p;
    p.feed(raw.data(), raw.size());
    REQUIRE(p.has_error());
    return p.get_error();
}

// ─── Basic method parsing ────────────────────────────────────────────────────

TEST_CASE("GET request parses method, URI, and Host header") {
    auto req = parse_one("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(req.method == Method::GET);
    CHECK(req.uri == "/index.html");
    CHECK(req.headers.at("host") == "localhost");
}

TEST_CASE("POST request parses body") {
    auto req = parse_one(
        "POST /submit HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello"
    );
    CHECK(req.method == Method::POST);
    CHECK(req.get_body() == "hello");
}

TEST_CASE("HEAD request parses correctly") {
    auto req = parse_one("HEAD /page HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(req.method == Method::HEAD);
    CHECK(req.uri == "/page");
}

TEST_CASE("OPTIONS request parses correctly") {
    auto req = parse_one("OPTIONS / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(req.method == Method::OPTIONS);
}

// ─── Query string ────────────────────────────────────────────────────────────

TEST_CASE("Query string is preserved in URI") {
    auto req = parse_one("GET /search?q=hello&page=2 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(req.uri == "/search?q=hello&page=2");
}

// ─── Header parsing ──────────────────────────────────────────────────────────

TEST_CASE("Headers are case-folded to lowercase") {
    auto req = parse_one("GET / HTTP/1.1\r\nContent-Type: text/html\r\nHost: x\r\n\r\n");
    CHECK(req.headers.count("content-type") == 1);
    CHECK(req.headers.at("content-type") == "text/html");
}

TEST_CASE("Multiple headers are all captured") {
    auto req = parse_one(
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: text/html\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: close\r\n"
        "\r\n"
    );
    CHECK(req.headers.count("accept") == 1);
    CHECK(req.headers.count("accept-encoding") == 1);
    CHECK(req.headers.count("connection") == 1);
}

// ─── Incremental feeding ─────────────────────────────────────────────────────

TEST_CASE("Parser accepts input split across multiple feed() calls") {
    RequestParser p;
    std::string raw = "GET /split HTTP/1.1\r\nHost: localhost\r\n\r\n";
    // Feed one byte at a time.
    for (size_t i = 0; i < raw.size() - 1; ++i) {
        p.feed(raw.data() + i, 1);
        REQUIRE_FALSE(p.has_request());
        REQUIRE_FALSE(p.has_error());
    }
    p.feed(raw.data() + raw.size() - 1, 1);
    REQUIRE(p.has_request());
    CHECK(p.get_request().uri == "/split");
}

// ─── Keep-alive reset ────────────────────────────────────────────────────────

TEST_CASE("Parser can handle two sequential requests after reset()") {
    RequestParser p;
    std::string r1 = "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string r2 = "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n";

    p.feed(r1.data(), r1.size());
    REQUIRE(p.has_request());
    CHECK(p.get_request().uri == "/first");

    p.reset();
    p.feed(r2.data(), r2.size());
    REQUIRE(p.has_request());
    CHECK(p.get_request().uri == "/second");
}

// ─── Error paths ─────────────────────────────────────────────────────────────

TEST_CASE("Unknown method returns MethodNotAllowed error") {
    auto err = parse_err("DELETE /resource HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(err == ParseError::MethodNotAllowed);
}

TEST_CASE("Malformed request line returns error") {
    auto err = parse_err("NOTAMETHOD\r\n\r\n");
    CHECK(err != ParseError::Ok);
}

TEST_CASE("Content-Length exceeding MAX_BODY_SIZE returns PayloadTooLarge") {
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 10485761\r\n"  // 10 MB + 1
        "\r\n";
    auto err = parse_err(raw);
    CHECK(err == ParseError::PayloadTooLarge);
}

// ─── Caching headers ─────────────────────────────────────────────────────────

TEST_CASE("If-None-Match header is captured") {
    auto req = parse_one(
        "GET /file HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "If-None-Match: \"abc123\"\r\n"
        "\r\n"
    );
    CHECK(req.headers.at("if-none-match") == "\"abc123\"");
}

TEST_CASE("Range header is captured") {
    auto req = parse_one(
        "GET /file HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Range: bytes=0-1023\r\n"
        "\r\n"
    );
    CHECK(req.headers.at("range") == "bytes=0-1023");
}

// ─── Security ────────────────────────────────────────────────────────────────

TEST_CASE("Path traversal sequence is preserved in URI (detection is handler responsibility)") {
    // The parser must not silently strip or modify the URI — the handler
    // enforces policy via resolve_path(). Just confirm it parses.
    auto req = parse_one("GET /../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(req.uri == "/../../../etc/passwd");
}

TEST_CASE("Null bytes in body do not crash the parser") {
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 3\r\n"
        "\r\n";
    raw += '\0'; raw += '\0'; raw += '\0';
    RequestParser p;
    p.feed(raw.data(), raw.size());
    REQUIRE(p.has_request());
    CHECK(p.get_request().get_body().size() == 3);
}

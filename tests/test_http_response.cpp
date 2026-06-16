#include <catch2/catch_all.hpp>
#include "core/http_response.hpp"

TEST_CASE("HttpResponse default-constructs to 200 OK") {
    HttpResponse resp(StatusCode::Ok);
    CHECK(resp.status_code == StatusCode::Ok);
    CHECK(resp.status_message == "OK");
}

TEST_CASE("headers_to_bytes includes status line and all headers") {
    HttpResponse resp(StatusCode::Ok);
    resp.headers["Content-Type"]   = "text/plain";
    resp.headers["Content-Length"] = "5";
    resp.body = "hello";

    std::string h = resp.headers_to_bytes();
    CHECK(h.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(h.find("Content-Type: text/plain") != std::string::npos);
    CHECK(h.find("Content-Length: 5") != std::string::npos);
    // Must end with blank line.
    CHECK(h.substr(h.size() - 4) == "\r\n\r\n");
}

TEST_CASE("to_bytes includes body after blank line") {
    HttpResponse resp(StatusCode::Ok);
    resp.body = "hello";
    resp.headers["Content-Length"] = "5";

    std::string raw = resp.to_bytes();
    auto sep = raw.find("\r\n\r\n");
    REQUIRE(sep != std::string::npos);
    CHECK(raw.substr(sep + 4) == "hello");
}

TEST_CASE("HEAD response: headers_to_bytes without body") {
    HttpResponse resp(StatusCode::Ok);
    resp.headers["Content-Length"] = "16651";
    resp.body.clear();  // HEAD clears body

    std::string h = resp.headers_to_bytes();
    CHECK(h.find("Content-Length: 16651") != std::string::npos);
    // No body after headers.
    auto sep = h.find("\r\n\r\n");
    REQUIRE(sep != std::string::npos);
    CHECK(h.size() == sep + 4);
}

TEST_CASE("Status codes serialize correctly") {
    CHECK(HttpResponse(StatusCode::NotFound).status_message    == "Not Found");
    CHECK(HttpResponse(StatusCode::PartialContent).status_message == "Partial Content");
    CHECK(HttpResponse(StatusCode::NotModified).status_message == "Not Modified");
    CHECK(HttpResponse(StatusCode::PayloadTooLarge).status_message == "Payload Too Large");
    CHECK(HttpResponse(StatusCode::MethodNotAllowed).status_message == "Method Not Allowed");
    CHECK(HttpResponse(StatusCode::GatewayTimeout).status_message == "Gateway Timeout");
}

TEST_CASE("OPTIONS response has correct Allow header format") {
    HttpResponse resp(StatusCode::NoContent);
    resp.headers["Allow"] = "GET, HEAD, POST, OPTIONS";
    std::string raw = resp.headers_to_bytes();
    CHECK(raw.find("Allow: GET, HEAD, POST, OPTIONS") != std::string::npos);
    CHECK(raw.find("HTTP/1.1 204") != std::string::npos);
}

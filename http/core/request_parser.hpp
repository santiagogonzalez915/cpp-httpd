#ifndef REQUEST_PARSER_HPP
#define REQUEST_PARSER_HPP

#include <cstddef>
#include <string>
#include "core/http_request.hpp"

enum class ParseError {
    Ok,
    Incomplete,
    BadRequest,
    MethodNotAllowed,
    InvalidRequestLine,
    InvalidHeader,
    PayloadTooLarge
};

class RequestParser {
public:
    RequestParser() = default;

    void feed(const char* data, size_t len);

    bool has_request() const;

    HttpRequest get_request();

    bool has_error() const;

    ParseError get_error() const;
    const std::string& get_error_message() const;

    bool need_more() const { return !has_request() && !has_error(); }

    void reset();

private:
    enum class State { RequestLine, Headers, Body, Done, Error };

    std::string buffer_;
    State state_ = State::RequestLine;
    HttpRequest request_;
    ParseError error_ = ParseError::Ok;
    std::string error_message_;
    size_t body_bytes_remaining_ = 0;

    void advance();
    bool process_request_line();
    bool process_headers();
    bool process_body();
};

#endif
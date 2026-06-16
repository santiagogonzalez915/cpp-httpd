#include "core/request_parser.hpp"
#include "core/request_handler_utils.hpp"
#include <algorithm>
#include <cctype>
#include <limits>

namespace {

constexpr size_t MAX_REQUEST_LINE = 8192;
constexpr size_t MAX_HEADER_LINE = 8192;
constexpr size_t MAX_BODY_SIZE   = 10 * 1024 * 1024;  // 10 MB

constexpr size_t CRLF_TERMINATOR_LEN = 2;  // "\r\n"
constexpr size_t LF_TERMINATOR_LEN = 1;    // "\n"

size_t find_line_end(const std::string& buffer, size_t start) {
    const size_t newline_pos = buffer.find('\n', start);
    if (newline_pos == std::string::npos) {
        return std::string::npos;
    }
    // If we have \r\n, return the position of \r so the line excludes both.
    const bool has_cr_before = (newline_pos > 0 && buffer[newline_pos - 1] == '\r');
    if (has_cr_before) {
        return newline_pos - 1;
    }
    return newline_pos;
}

// Returns the length of the line terminator at the given position: 2 for \r\n, 1 for \n.
size_t line_terminator_len(const std::string& buffer, size_t terminator_start) {
    if (terminator_start >= buffer.size()) {
        return 0;
    }
    const bool has_cr = (buffer[terminator_start] == '\r');
    const bool has_lf_after = (terminator_start + 1 < buffer.size() &&
                               buffer[terminator_start + 1] == '\n');
    if (has_cr && has_lf_after) {
        return CRLF_TERMINATOR_LEN;
    }
    return LF_TERMINATOR_LEN;
}

} // namespace

void RequestParser::feed(const char* data, size_t len) {
    if (state_ == State::Done || state_ == State::Error) return;
    buffer_.append(data, len);
    advance();
}

void RequestParser::advance() {
    while (state_ != State::Done && state_ != State::Error) {
        bool progress = false;
        if (state_ == State::RequestLine) {
            progress = process_request_line();
        } else if (state_ == State::Headers) {
            progress = process_headers();
        } else if (state_ == State::Body) {
            progress = process_body();
        }
        if (!progress) {
            return;
        }
    }
}

bool RequestParser::process_request_line() {
    size_t line_end = find_line_end(buffer_, 0);
    if (line_end == std::string::npos) {
        if (buffer_.size() > MAX_REQUEST_LINE) {
            state_ = State::Error;
            error_ = ParseError::BadRequest;
            error_message_ = "Request line too long";
        }
        return false;
    }

    size_t term_len = line_terminator_len(buffer_, line_end);
    std::string line = buffer_.substr(0, line_end);
    buffer_.erase(0, line_end + term_len);

    // Parse "METHOD URI HTTP/1.x"
    size_t s1 = line.find(' ');  // separate method from URI
    if (s1 == std::string::npos) {
        state_ = State::Error;
        error_ = ParseError::InvalidRequestLine;
        error_message_ = "Missing space in request line";
        return false;
    }

    size_t s2 = line.find(' ', s1 + 1);  // separate URI from version
    if (s2 == std::string::npos) {
        state_ = State::Error;
        error_ = ParseError::InvalidRequestLine;
        error_message_ = "Missing HTTP version in request line";
        return false;
    }

    if (line.find(' ', s2 + 1) != std::string::npos) {
        state_ = State::Error;
        error_ = ParseError::InvalidRequestLine;
        error_message_ = "Invalid request line format";
        return false;
    }

    std::string method_str = line.substr(0, s1);
    request_.uri = line.substr(s1 + 1, s2 - (s1 + 1));
    std::string version = trim(line.substr(s2 + 1));

    if (version.empty() || method_str.empty() || request_.uri.empty()) {
        state_ = State::Error;
        error_ = ParseError::InvalidRequestLine;
        error_message_ = "Empty method, URI, or version";
        return false;
    }

    if (method_str == "GET") {
        request_.method = Method::GET;
        request_.method_str = "GET";
    } else if (method_str == "POST") {
        request_.method = Method::POST;
        request_.method_str = "POST";
    } else if (method_str == "HEAD") {
        request_.method = Method::HEAD;
        request_.method_str = "HEAD";
    } else if (method_str == "OPTIONS") {
        request_.method = Method::OPTIONS;
        request_.method_str = "OPTIONS";
    } else {
        state_ = State::Error;
        error_ = ParseError::MethodNotAllowed;
        error_message_ = "Method not allowed";
        return false;
    }

    state_ = State::Headers;
    return true;
}

bool RequestParser::process_headers() {
    size_t line_end = find_line_end(buffer_, 0);
    if (line_end == std::string::npos) {
        if (buffer_.size() > MAX_HEADER_LINE) {
            state_ = State::Error;
            error_ = ParseError::InvalidHeader;
            error_message_ = "Header line too long";
        }
        return false;
    }

    size_t term_len = line_terminator_len(buffer_, line_end);
    std::string line = buffer_.substr(0, line_end);
    buffer_.erase(0, line_end + term_len);

    if (line.empty()) {
        // End of headers
        auto it = request_.headers.find("content-length");
        size_t cl = 0;
        if (it != request_.headers.end()) {
            try {
                long long v = std::stoll(it->second);
                if (v < 0) {
                    state_ = State::Error;
                    error_ = ParseError::BadRequest;
                    error_message_ = "Invalid Content-Length";
                    return false;
                }
                cl = static_cast<size_t>(v);
            } catch (...) {
                state_ = State::Error;
                error_ = ParseError::BadRequest;
                error_message_ = "Invalid Content-Length";
                return false;
            }
        }

        if (cl > MAX_BODY_SIZE) {
            state_ = State::Error;
            error_ = ParseError::PayloadTooLarge;
            error_message_ = "Request body exceeds maximum allowed size";
            return false;
        }

        request_.content_length = static_cast<uint16_t>(
            cl > static_cast<size_t>(std::numeric_limits<uint16_t>::max())
                ? std::numeric_limits<uint16_t>::max()
                : cl);
        body_bytes_remaining_ = cl;

        // Only POST carries a body; HEAD and OPTIONS never send one.
        if (request_.method == Method::POST && body_bytes_remaining_ > 0) {
            state_ = State::Body;
        } else {
            state_ = State::Done;
        }
        return true;
    }

    auto [name, value] = parse_header_line(line, true);
    if (name.empty()) {
        state_ = State::Error;
        error_ = ParseError::InvalidHeader;
        error_message_ = "Invalid header line";
        return false;
    }
    request_.headers[name] = value;
    return true;
}

bool RequestParser::process_body() {
    if (body_bytes_remaining_ == 0) {
        state_ = State::Done;
        return true;
    }

    if (buffer_.size() < body_bytes_remaining_) {
        return false;
    }

    request_.buffer = buffer_.substr(0, body_bytes_remaining_);
    buffer_.erase(0, body_bytes_remaining_);
    body_bytes_remaining_ = 0;
    state_ = State::Done;
    return true;
}

bool RequestParser::has_request() const { return state_ == State::Done; }
bool RequestParser::has_error() const { return state_ == State::Error; }
ParseError RequestParser::get_error() const { return error_; }
const std::string& RequestParser::get_error_message() const { return error_message_; }

HttpRequest RequestParser::get_request() {
    HttpRequest r = std::move(request_);
    request_ = HttpRequest();
    state_ = State::RequestLine;
    error_ = ParseError::Ok;
    error_message_.clear();
    body_bytes_remaining_ = 0;
    advance();  // parse any pipelined data already in buffer_
    return r;
}

void RequestParser::reset() {
    buffer_.clear();
    state_ = State::RequestLine;
    request_ = HttpRequest();
    error_ = ParseError::Ok;
    error_message_.clear();
    body_bytes_remaining_ = 0;
}

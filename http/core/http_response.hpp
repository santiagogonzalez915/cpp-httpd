#ifndef HTTP_RESPONSE_HPP
#define HTTP_RESPONSE_HPP

#include <string>
#include <sys/types.h>
#include <unordered_map>

enum class StatusCode {
    Ok = 200,
    NoContent = 204,
    PartialContent = 206,
    NotModified = 304,
    BadRequest = 400,
    InvalidCredentials = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    PayloadTooLarge = 413,
    RangeNotSatisfiable = 416,
    InternalServerError = 500,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504
};

inline const char* to_string(StatusCode s) {
    switch (s) {
        case StatusCode::Ok:                 return "OK";
        case StatusCode::NoContent:          return "No Content";
        case StatusCode::PartialContent:     return "Partial Content";
        case StatusCode::NotModified:        return "Not Modified";
        case StatusCode::BadRequest:         return "Bad Request";
        case StatusCode::InvalidCredentials: return "Unauthorized";
        case StatusCode::Forbidden:          return "Forbidden";
        case StatusCode::NotFound:           return "Not Found";
        case StatusCode::MethodNotAllowed:   return "Method Not Allowed";
        case StatusCode::NotAcceptable:      return "Not Acceptable";
        case StatusCode::PayloadTooLarge:    return "Payload Too Large";
        case StatusCode::RangeNotSatisfiable:return "Range Not Satisfiable";
        case StatusCode::InternalServerError:return "Internal Server Error";
        case StatusCode::BadGateway:         return "Bad Gateway";
        case StatusCode::ServiceUnavailable: return "Service Unavailable";
        case StatusCode::GatewayTimeout:     return "Gateway Timeout";
    }
    return "Unknown Status Code";
}

struct HttpResponse {
    HttpResponse();
    HttpResponse(StatusCode status_code);
    StatusCode status_code = StatusCode::Ok;
    std::string status_message = to_string(status_code);
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // For zero-copy sendfile delivery. -1 means body is in the body string.
    // The caller that sets body_fd is responsible for closing it.
    int body_fd = -1;
    off_t body_fd_offset = 0;
    off_t body_fd_size = 0;

    /** Status line + headers + blank line only (used when body_fd is set). */
    std::string headers_to_bytes() const;
    /** Serialize to HTTP response bytes (status line + headers + body string). */
    std::string to_bytes() const;
};

#endif
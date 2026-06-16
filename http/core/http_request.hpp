#ifndef HTTP_REQUEST_HPP
#define HTTP_REQUEST_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

enum class Method {
    GET,
    POST,
    HEAD,
    OPTIONS
};

inline const char* to_string(Method m) {
    switch (m) {
        case Method::GET:     return "GET";
        case Method::POST:    return "POST";
        case Method::HEAD:    return "HEAD";
        case Method::OPTIONS: return "OPTIONS";
    }
    return "UNKNOWN";
}

struct HttpRequest {
    HttpRequest();
    HttpRequest(Method method, std::string& uri, std::unordered_map<std::string, std::string>& headers, uint16_t cd, std::string& buffer);

    Method method = Method::GET;
    std::string method_str = "GET";
    std::string uri;
    std::unordered_map<std::string, std::string> headers;
    uint16_t content_length = 0;
    std::string buffer;

    const std::string& get_method() const;
    const std::string& get_uri() const;
    const std::unordered_map<std::string, std::string>& get_headers() const;
    const uint16_t& get_content_length() const;
    const std::string& get_body() const;
    std::string to_bytes() const;

};

#endif
#include "core/http_request.hpp"

HttpRequest::HttpRequest() : method(Method::GET), method_str("GET") {}

HttpRequest::HttpRequest(Method method, std::string& uri, std::unordered_map<std::string, std::string>& headers, uint16_t cd, std::string& buffer)
    : method(method), method_str(to_string(method)), uri(uri), headers(headers), content_length(cd), buffer(buffer) {}

const std::string& HttpRequest::get_method() const {
    return this->method_str;
}

const std::string& HttpRequest::get_uri() const {
    return this->uri;
}

const std::unordered_map<std::string, std::string>& HttpRequest::get_headers() const {
    return this->headers;
}

const uint16_t& HttpRequest::get_content_length() const {
    return this->content_length;
}

const std::string& HttpRequest::get_body() const {
    return this->buffer;
}

std::string HttpRequest::to_bytes() const {
    std::string request_line = this->get_method() + " " + this->get_uri() + " " + "HTTP/1.1\r\n";
    for (const auto& [key, value] : this->get_headers()) {
        request_line += key + ": " + value + "\r\n";
    }
    request_line += this->get_body();
    return request_line;
}
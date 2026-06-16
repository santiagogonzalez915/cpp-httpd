#include "core/http_response.hpp"
#include <cstdio>

HttpResponse::HttpResponse() = default;

HttpResponse::HttpResponse(StatusCode code)
    : status_code(code),
      status_message(to_string(code)) {}

static bool is_chunked(const std::unordered_map<std::string, std::string>& headers) {
    auto it = headers.find("Transfer-Encoding");
    if (it == headers.end()) return false;
    return it->second == "chunked";
}

std::string HttpResponse::headers_to_bytes() const {
    std::string out = "HTTP/1.1 " + std::to_string(static_cast<int>(status_code)) + " " + status_message + "\r\n";
    for (const auto& [name, value] : headers) {
        out += name + ": " + value + "\r\n";
    }
    out += "\r\n";
    return out;
}

std::string HttpResponse::to_bytes() const {
    std::string out = headers_to_bytes();
    if (is_chunked(headers)) {
        if (!body.empty()) {
            char buf[32];
            int n = std::snprintf(buf, sizeof(buf), "%zx\r\n", body.size());
            out.append(buf, static_cast<size_t>(n));
            out += body;
            out += "\r\n";
        }
        out += "0\r\n\r\n";
    } else {
        out += body;
    }
    return out;
}

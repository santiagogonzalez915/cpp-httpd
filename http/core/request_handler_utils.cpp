#include "core/request_handler_utils.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>
#include <sys/stat.h>

namespace {

int base64_char_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string normalize_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string segment;
    bool absolute = (!path.empty() && path[0] == '/');
    for (size_t i = absolute ? 1u : 0u; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (segment == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!segment.empty() && segment != ".") {
                parts.push_back(std::move(segment));
            }
            segment.clear();
        } else {
            segment += path[i];
        }
    }
    std::string result;
    if (absolute) result += '/';
    for (size_t j = 0; j < parts.size(); ++j) {
        if (j) result += '/';
        result += parts[j];
    }
    return result.empty() ? "/" : result;
}

std::string trim(const std::string& s, const char* whitespace) {
    const size_t first = s.find_first_not_of(whitespace);
    if (first == std::string::npos) return "";
    const size_t last = s.find_last_not_of(whitespace);
    return (last == std::string::npos)
        ? s.substr(first)
        : s.substr(first, last - first + 1);
}

std::pair<std::string, std::string> parse_header_line(const std::string& line, bool lowercase_name) {
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
        return {"", ""};
    }
    std::string name = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (name.empty()) {
        return {"", ""};
    }
    if (lowercase_name) {
        name = to_lower(name);
    }
    return {name, value};
}

std::string get_header(const std::unordered_map<std::string, std::string>& headers,
                      const std::string& name) {
    auto it = headers.find(name);
    return (it != headers.end()) ? it->second : "";
}

std::pair<std::string, std::string> split_uri(const std::string& uri) {
    size_t q = uri.find('?');
    if (q == std::string::npos)
        return {uri, ""};
    return {uri.substr(0, q), uri.substr(q + 1)};
}

bool path_under_root(const std::string& canonical_path, const std::string& document_root) {
    std::string norm_path = normalize_path(canonical_path);
    std::string norm_root = normalize_path(document_root);
    if (!norm_root.empty() && norm_root.back() == '/')
        norm_root.pop_back();
    if (norm_path == norm_root)
        return true;
    if (norm_path.size() > norm_root.size() && norm_path[norm_root.size()] == '/' &&
        norm_path.compare(0, norm_root.size(), norm_root) == 0)
        return true;
    return false;
}

std::string content_type_from_suffix(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot == path.size() - 1)
        return "application/octet-stream";
    std::string ext = to_lower(path.substr(dot + 1));
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "txt") return "text/plain";
    if (ext == "css") return "text/css";
    if (ext == "js") return "application/javascript";
    if (ext == "json") return "application/json";
    if (ext == "xml") return "application/xml";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "ico") return "image/x-icon";
    if (ext == "pdf") return "application/pdf";
    if (ext == "zip") return "application/zip";
    return "application/octet-stream";
}

std::pair<bool, time_t> parse_http_date(const std::string& s) {
    if (s.empty()) return {false, 0};
    struct tm tm = {};
    const char* rest = strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (!rest) return {false, 0};
    return {true, timegm(&tm)};
}

std::string format_http_date(time_t t) {
    struct tm* gm = gmtime(&t);
    if (!gm) return "";
    char buf[64];
    size_t n = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gm);
    return std::string(buf, n);
}

std::string decode_base64(const std::string& s) {
    std::string out;
    out.reserve((s.size() * 3) / 4);
    int bits = 0;
    int value = 0;
    for (unsigned char c : s) {
        if (std::isspace(c) || c == '=') continue;
        int v = base64_char_value(c);
        if (v < 0) return "";
        value = (value << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((value >> bits) & 0xff);
        }
    }
    return out;
}

std::unique_ptr<HtaccessAuth> parse_htaccess(const std::string& file_path) {
    std::ifstream f(file_path);
    if (!f) return nullptr;
    HtaccessAuth auth;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] == '#') continue;
        if (line.compare(i, 9, "AuthType ") == 0) {
            size_t j = i + 9;
            while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
            if (line.compare(j, 5, "Basic") == 0)
                auth.auth_type_basic = true;
            continue;
        }
        if (line.compare(i, 5, "User ") == 0) {
            auth.user_b64 = trim(line.substr(i + 5), " \t");
            continue;
        }
        if (line.compare(i, 9, "Password ") == 0) {
            auth.password_b64 = trim(line.substr(i + 9), " \t");
            continue;
        }
    }
    if (!auth.auth_type_basic || auth.user_b64.empty()) return nullptr;
    return std::make_unique<HtaccessAuth>(auth);
}

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string trim_trailing_slash(const std::string& s) {
    if (s.empty()) return s;
    if (s.back() == '/') return s.substr(0, s.size() - 1);
    return s;
}

std::string trim_leading_slash(const std::string& s) {
    if (s.empty()) return s;
    if (s.front() == '/') return s.substr(1);
    return s;
}

bool read_file_to_string(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.fail();
}

bool starts_with(const std::string& s, const std::string& prefix) {
    std::string s_lower = to_lower(s);
    std::string prefix_lower = to_lower(prefix);
    return s_lower.compare(0, prefix_lower.size(), prefix_lower) == 0;
}


bool is_executable(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
}
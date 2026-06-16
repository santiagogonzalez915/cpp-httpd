#ifndef REQUEST_HANDLER_UTILS_HPP
#define REQUEST_HANDLER_UTILS_HPP

#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>

std::string normalize_path(const std::string& path);

std::string trim(const std::string& s, const char* whitespace = " \t\r\n");

std::pair<std::string, std::string> parse_header_line(const std::string& line, bool lowercase_name = false);

std::string get_header(const std::unordered_map<std::string, std::string>& headers,
                      const std::string& name);

std::pair<std::string, std::string> split_uri(const std::string& uri);

bool path_under_root(const std::string& canonical_path, const std::string& document_root);

std::string content_type_from_suffix(const std::string& path);

std::pair<bool, time_t> parse_http_date(const std::string& s);

std::string format_http_date(time_t t);

std::string decode_base64(const std::string& s);

struct HtaccessAuth {
    std::string user_b64;
    std::string password_b64;
    bool auth_type_basic = false;
};

std::unique_ptr<HtaccessAuth> parse_htaccess(const std::string& file_path);

std::string to_lower(const std::string& s);

std::string trim_trailing_slash(const std::string& s);

std::string trim_leading_slash(const std::string& s);

bool read_file_to_string(const std::string& path, std::string& out);

bool starts_with(const std::string& s, const std::string& prefix);

bool is_executable(const std::string& path);

#endif

#include "handlers/auth_handler.hpp"

#include "core/http_request.hpp"
#include "core/request_handler_utils.hpp"

AuthResult AuthHandler::check_htaccess_and_auth(const std::string& dir_path, const HttpRequest& request) const {
    std::string htaccess_path = trim_trailing_slash(dir_path) + "/.htaccess";
    auto htaccess = parse_htaccess(htaccess_path);
    if (!htaccess) {
        return AuthResult::Ok;
    }

    std::string auth_header = get_header(request.headers, "authorization");
    if (auth_header.empty()) {
        return AuthResult::MissingAuth;
    }

    if (!starts_with(auth_header, "basic ")) {
        return AuthResult::MissingAuth;
    }

    std::string base64_credentials = auth_header.substr(6);
    std::string credentials = decode_base64(base64_credentials);
    if (credentials.empty()) {
        return AuthResult::MissingAuth;
    }

    size_t colon_pos = credentials.find(':');
    if (colon_pos == std::string::npos) {
        return AuthResult::InvalidAuth;
    }

    std::string client_user = credentials.substr(0, colon_pos);
    std::string client_password = credentials.substr(colon_pos + 1);
    std::string expected_user = decode_base64(htaccess->user_b64);
    std::string expected_password = decode_base64(htaccess->password_b64);

    if (client_user != expected_user || client_password != expected_password) {
        return AuthResult::InvalidAuth;
    }
    return AuthResult::Ok;
}


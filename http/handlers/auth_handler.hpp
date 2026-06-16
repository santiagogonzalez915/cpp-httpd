#ifndef AUTH_HANDLER_HPP
#define AUTH_HANDLER_HPP

#include "core/auth_result.hpp"
#include <string>

struct HttpRequest;

class AuthHandler {
public:
    AuthResult check_htaccess_and_auth(const std::string& dir_path, const HttpRequest& request) const;
};

#endif


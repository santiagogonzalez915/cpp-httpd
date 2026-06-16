#ifndef REQUEST_HANDLER_HPP
#define REQUEST_HANDLER_HPP

#include "config/config.hpp"
#include "core/auth_result.hpp"
#include "core/http_request.hpp"
#include "core/http_response.hpp"
#include "core/logger.hpp"
#include "core/phase_context.hpp"

#include <memory>

class AuthHandler;
class CgiHandler;
class DirectoryHandler;
class StaticFileHandler;
class VHostResolver;

class RequestHandler {
    const Config& config;
    std::unique_ptr<Logger> logger_;
    std::unique_ptr<VHostResolver> vhost_resolver;
    std::unique_ptr<AuthHandler> auth_handler;
    std::unique_ptr<StaticFileHandler> static_files;
    std::unique_ptr<DirectoryHandler> directory_handler;
    std::unique_ptr<CgiHandler> cgi_handler;

    public: 
        explicit RequestHandler(const Config& config);
        ~RequestHandler();
        HttpResponse handle(const HttpRequest& request, bool accepting_new_connections = true,
                            const std::string* client_remote_addr = nullptr) const;

    private: 
        // Phase pipeline
        void run_phases(PhaseContext& ctx) const;
        void phase_find_config(PhaseContext& ctx) const;
        void phase_access(PhaseContext& ctx) const;
        void phase_content(PhaseContext& ctx) const;
        void phase_log(PhaseContext& ctx) const;

};

#endif
#include "handlers/request_handler.hpp"
#include "handlers/auth_handler.hpp"
#include "handlers/cgi_handler.hpp"
#include "handlers/directory_handler.hpp"
#include "core/request_handler_utils.hpp"
#include "handlers/static_file_handler.hpp"
#include "routing/vhost_resolver.hpp"

#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <mutex>

RequestHandler::RequestHandler(const Config& config)
    : config(config),
      logger_(config.log_file.empty()
              ? std::make_unique<Logger>()
              : std::make_unique<Logger>(config.log_file)),
      vhost_resolver(std::make_unique<VHostResolver>(config)),
      auth_handler(std::make_unique<AuthHandler>()),
      static_files(std::make_unique<StaticFileHandler>()),
      directory_handler(std::make_unique<DirectoryHandler>()),
      cgi_handler(std::make_unique<CgiHandler>(config)) {}

RequestHandler::~RequestHandler() = default;

HttpResponse RequestHandler::handle(const HttpRequest& request, bool accepting_new_connections,
                                    const std::string* client_remote_addr) const {
    PhaseContext ctx;
    ctx.request = &request;
    ctx.accepting_new_connections = accepting_new_connections;
    ctx.client_remote_addr = client_remote_addr;
    run_phases(ctx);
    return ctx.response;
}

void RequestHandler::run_phases(PhaseContext& ctx) const {
    ctx.current_phase = Phase::FindConfig;
    phase_find_config(ctx);
    if (ctx.response_ready) { phase_log(ctx); return; }

    ctx.current_phase = Phase::Access;
    phase_access(ctx);
    if (ctx.response_ready) { phase_log(ctx); return; }

    ctx.current_phase = Phase::Content;
    phase_content(ctx);

    // Content must always produce a response; if somehow it didn't, set 500.
    if (!ctx.response_ready) {
        ctx.response = HttpResponse(StatusCode::InternalServerError);
        ctx.response_ready = true;
    }

    ctx.current_phase = Phase::Log;
    phase_log(ctx);
}

void RequestHandler::phase_find_config(PhaseContext& ctx) const {
    ctx.vhost = vhost_resolver->resolve_vhost(*ctx.request);
    if (!ctx.vhost) {
        ctx.response = HttpResponse(StatusCode::ServiceUnavailable);
        ctx.response_ready = true;
        return;
    }

    std::tie(ctx.path, ctx.query_string) = split_uri(ctx.request->uri);
    // Normalize to origin-form
    if (!ctx.path.empty() && ctx.path[0] != '/') {
        ctx.path = "/" + ctx.path;
    }
}

void RequestHandler::phase_access(PhaseContext& ctx) const {
    // GET /load
    if (ctx.request->get_method() == "GET" && ctx.path == "/load") {
        ctx.response = ctx.accepting_new_connections
            ? HttpResponse(StatusCode::Ok)
            : HttpResponse(StatusCode::ServiceUnavailable);
        ctx.response_ready = true;
        return;
    }

    // Path resolution
    auto [resolved, under] = vhost_resolver->resolve_path(*ctx.vhost, ctx.path);
    ctx.resolved_path = resolved;
    ctx.under_root = under;

    if (!ctx.under_root) {
        ctx.response = HttpResponse(StatusCode::Forbidden);
        ctx.response_ready = true;
        return;
    }

    // stat the resolved path
    if (stat(ctx.resolved_path.c_str(), &ctx.st) != 0) {
        ctx.response = HttpResponse(StatusCode::NotFound);
        ctx.response_ready = true;
        return;
    }
    ctx.stat_valid = true;

    // Compute dir_of_resource for .htaccess lookup
    if (S_ISDIR(ctx.st.st_mode)) {
        ctx.dir_of_resource = ctx.resolved_path;
    } else {
        size_t last_slash = ctx.resolved_path.rfind('/');
        ctx.dir_of_resource = (last_slash != std::string::npos)
            ? ctx.resolved_path.substr(0, last_slash) : "";
    }

    // Authentication
    AuthResult auth = auth_handler->check_htaccess_and_auth(ctx.dir_of_resource, *ctx.request);
    if (auth == AuthResult::InvalidAuth || auth == AuthResult::MissingAuth) {
        ctx.response = HttpResponse(StatusCode::InvalidCredentials);
        ctx.response.headers["WWW-Authenticate"] = "Basic realm=\"Restricted Files\"";
        ctx.response.headers["Content-Length"] = "0";
        ctx.response_ready = true;
        return;
    }
}

void RequestHandler::phase_content(PhaseContext& ctx) const {
    // If access phase already produced a response (e.g. /load), nothing to do.
    if (ctx.response_ready) return;

    // OPTIONS is method-level; respond before checking the resource.
    if (ctx.request->method == Method::OPTIONS) {
        ctx.response = HttpResponse(StatusCode::NoContent);
        ctx.response.headers["Allow"] = "GET, HEAD, POST, OPTIONS";
        ctx.response.headers["Date"] = format_http_date(time(nullptr));
        ctx.response_ready = true;
        return;
    }

    if (ctx.stat_valid && S_ISREG(ctx.st.st_mode) && is_executable(ctx.resolved_path)) {
        ctx.response = cgi_handler->run_cgi(ctx.resolved_path, *ctx.request, ctx.vhost, ctx.client_remote_addr);
        ctx.response_ready = true;
        return;
    }

    // POST is only valid for CGI; reject it for static resources.
    if (ctx.request->method == Method::POST) {
        ctx.response = HttpResponse(StatusCode::MethodNotAllowed);
        ctx.response_ready = true;
        return;
    }

    if (ctx.stat_valid && S_ISREG(ctx.st.st_mode)) {
        ctx.response = static_files->serve_static_file(ctx.resolved_path, *ctx.request);
        // HEAD: same headers as GET but no body — close the sendfile fd if one was set.
        if (ctx.request->method == Method::HEAD) {
            if (ctx.response.body_fd >= 0) {
                close(ctx.response.body_fd);
                ctx.response.body_fd = -1;
            }
            ctx.response.body.clear();
        }
        ctx.response_ready = true;
        return;
    }

    if (ctx.stat_valid && S_ISDIR(ctx.st.st_mode)) {
        ctx.response = directory_handler->serve_directory_index(ctx.resolved_path, *ctx.request,
                                                               ctx.vhost->document_root, *static_files);
        if (ctx.request->method == Method::HEAD) {
            if (ctx.response.body_fd >= 0) {
                close(ctx.response.body_fd);
                ctx.response.body_fd = -1;
            }
            ctx.response.body.clear();
        }
        ctx.response_ready = true;
        return;
    }

    ctx.response = HttpResponse(StatusCode::NotFound);
    ctx.response_ready = true;
}


void RequestHandler::phase_log(PhaseContext& ctx) const {
    // Apache Combined Log Format: %h %l %u %t "%r" %>s %b
    static const char* months[] = {
        "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
    };
    time_t now = time(nullptr);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "[%02d/%s/%04d:%02d:%02d:%02d +0000]",
             tm_buf.tm_mday, months[tm_buf.tm_mon], tm_buf.tm_year + 1900,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    std::string client_ip = (ctx.client_remote_addr && !ctx.client_remote_addr->empty())
        ? *ctx.client_remote_addr : "-";

    std::string request_line = std::string(ctx.request->get_method()) + " "
        + ctx.request->get_uri() + " HTTP/1.1";

    std::string bytes_str;
    auto cl_it = ctx.response.headers.find("Content-Length");
    if (cl_it != ctx.response.headers.end()) {
        bytes_str = cl_it->second;
    } else if (ctx.response.body_fd >= 0) {
        bytes_str = std::to_string(ctx.response.body_fd_size);
    } else {
        bytes_str = std::to_string(ctx.response.body.size());
    }

    char entry[1024];
    snprintf(entry, sizeof(entry), "%s - - %s \"%s\" %d %s",
             client_ip.c_str(), timestamp, request_line.c_str(),
             static_cast<int>(ctx.response.status_code), bytes_str.c_str());

    logger_->write(entry);
}
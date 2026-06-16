#ifndef PHASE_CONTEXT_HPP
#define PHASE_CONTEXT_HPP

#include "core/http_request.hpp"
#include "core/http_response.hpp"
#include "config/config.hpp"
#include <string>
#include <sys/stat.h>


enum class Phase {
    FindConfig,
    Access,
    Content,
    Log
};

struct PhaseContext {
    const HttpRequest* request = nullptr;
    bool accepting_new_connections = true;
    const std::string* client_remote_addr = nullptr;

    const VirtualHost* vhost = nullptr;

    std::string path;
    std::string query_string;

    std::string resolved_path;
    bool under_root = false;
    struct stat st{};
    bool stat_valid = false;
    std::string dir_of_resource;

    HttpResponse response;
    bool response_ready = false;

    Phase current_phase = Phase::FindConfig;
};

#endif

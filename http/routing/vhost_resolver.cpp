#include "routing/vhost_resolver.hpp"

#include "core/http_request.hpp"
#include "core/request_handler_utils.hpp"

VHostResolver::VHostResolver(const Config& config) : config(config) {}

const VirtualHost* VHostResolver::resolve_vhost(const HttpRequest& request) const {
    std::string host = get_header(request.headers, "host");
    // strip optional ":port" from host
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        host = host.substr(0, colon);
    }

    if (!host.empty()) {
        for (const VirtualHost& vhost : config.virtual_hosts) {
            if (to_lower(vhost.server_name) == to_lower(host)) {
                return &vhost;
            }
        }
    }

    if (config.virtual_hosts.empty()) {
        return nullptr;
    }
    return &config.virtual_hosts[0];
}

std::pair<std::string, bool> VHostResolver::resolve_path(const VirtualHost& vhost, const std::string& uri) const {
    std::string root = vhost.document_root;
    std::string combined;

    if (uri.empty() || uri == "/") {
        combined = root;
    } else {
        combined = trim_trailing_slash(root) + "/" + trim_leading_slash(uri);
    }

    std::string canonical_path = normalize_path(combined);
    bool under_root = path_under_root(canonical_path, root);

    if (!under_root) {
        return std::make_pair("", false);
    }
    return std::make_pair(canonical_path, true);
}


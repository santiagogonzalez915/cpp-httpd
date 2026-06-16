#ifndef VHOST_RESOLVER_HPP
#define VHOST_RESOLVER_HPP

#include "config/config.hpp"
#include <string>
#include <utility>

struct HttpRequest;

class VHostResolver {
    const Config& config;

public:
    explicit VHostResolver(const Config& config);

    const VirtualHost* resolve_vhost(const HttpRequest& request) const;

    std::pair<std::string, bool> resolve_path(const VirtualHost& vhost, const std::string& uri) const;
};

#endif


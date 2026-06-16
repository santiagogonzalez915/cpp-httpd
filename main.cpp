#include "config/config.hpp"
#include "server/server.hpp"
#include <iostream>
#include <limits.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>

int main(int argc, char* argv[]) {
    std::string config_path = (argc > 1) ? argv[1] : "httpd.conf";
    Config config;
    if (!load_config(config_path, config)) {
        std::cerr << "Failed to load config from " << config_path << "\n";
        return 1;
    }
    // Resolve relative DocumentRoot (e.g. ".") to absolute path using current working directory
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        std::string base(cwd);
        for (auto& vhost : config.virtual_hosts) {
            if (!vhost.document_root.empty() && vhost.document_root[0] != '/') {
                vhost.document_root = base + "/" + vhost.document_root;
                char resolved[PATH_MAX];
                if (realpath(vhost.document_root.c_str(), resolved))
                    vhost.document_root = resolved;
            }
        }
    }
    HttpServer server(config);
    return server.run();
}

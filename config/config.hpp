#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>

struct VirtualHost {
    std::string document_root;
    std::string server_name;
};

struct Config {
    int listen_port = 8080;
    std::vector<VirtualHost> virtual_hosts;
    enum class Mode {Select, Threads};
    Mode mode = Mode::Threads;
    int n_select_loops = 0;
    int n_threads = 4;
    int timeout_seconds = 10;
    std::string log_file;       // empty → log to stderr

    // TLS — all three must be set to enable HTTPS on tls_port.
    std::string tls_cert_file;
    std::string tls_key_file;
    int         tls_port = 4443;
};

bool load_config(const std::string& path, Config& out);

#endif
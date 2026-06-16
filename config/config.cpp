#include "config.hpp"
#include "http/core/request_handler_utils.hpp"
#include <fstream>
#include <cctype>
#include <algorithm>

namespace {

std::string strip_comment(const std::string& s) {
    auto hash = s.find('#');
    std::string before = (hash == std::string::npos) ? s : s.substr(0, hash);
    return trim(before);
}

bool split_first_word(const std::string& line, std::string& key, std::string& value) {
    auto space = line.find_first_of(" \t");
    if (space == std::string::npos) {
        key = trim(line);
        value.clear();
        return !key.empty();
    }
    key = trim(line.substr(0, space));
    value = trim(line.substr(space + 1));
    return true;
}

bool parse_positive_int(const std::string& value, int& out_val) {
    if (value.empty()) return false;
    try {
        size_t pos = 0;
        int n = std::stoi(value, &pos);
        if (n <= 0 || pos != value.size()) return false;
        out_val = n;
        return true;
    } catch (...) {
        return false;
    }
}

}

bool load_config(const std::string& path, Config& out) {
    std::ifstream f(path);
    if (!f.is_open())
        return false;

    bool in_virtual_host = false;
    std::string line;

    while (std::getline(f, line)) {
        std::string normalized = strip_comment(line);
        if (normalized.empty())
            continue;

        if (in_virtual_host) {
            if (normalized.find("</VirtualHost>") == 0) {
                in_virtual_host = false;
                continue;
            }
            std::string key, value;
            if (split_first_word(normalized, key, value)) {
                if (key == "DocumentRoot")
                    out.virtual_hosts.back().document_root = value;
                else if (key == "ServerName")
                    out.virtual_hosts.back().server_name = value;
            }
            continue;
        }

        if (normalized.find("<VirtualHost") == 0) {
            out.virtual_hosts.push_back(VirtualHost{});
            in_virtual_host = true;
            continue;
        }

        std::string key, value;
        if (!split_first_word(normalized, key, value))
            continue;

        if (key == "Listen") {
            int port = 0;
            if (!parse_positive_int(value, port)) return false;
            out.listen_port = port;
        } else if (key == "nThreads") {
            int n = 0;
            if (!parse_positive_int(value, n)) return false;
            out.mode = Config::Mode::Threads;
            out.n_threads = n;
        } else if (key == "nSelectLoops") {
            int n = 0;
            if (!parse_positive_int(value, n)) return false;
            out.mode = Config::Mode::Select;
            out.n_select_loops = n;
        } else if (key == "Timeout") {
            int n = 0;
            if (!parse_positive_int(value, n)) return false;
            out.timeout_seconds = n;
        } else if (key == "LogFile") {
            out.log_file = value;
        } else if (key == "TLSCertFile") {
            out.tls_cert_file = value;
        } else if (key == "TLSKeyFile") {
            out.tls_key_file = value;
        } else if (key == "TLSPort") {
            int n = 0;
            if (!parse_positive_int(value, n)) return false;
            out.tls_port = n;
        }
    }

    if (in_virtual_host)
        return false;
    return true;
}
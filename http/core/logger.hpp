#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

// Thread-safe Apache Combined Log Format writer.
// Falls back to stderr when no log file is configured.
class Logger {
    std::ofstream file_;
    std::mutex    mutex_;
    bool          to_file_;

public:
    Logger() : to_file_(false) {}

    explicit Logger(const std::string& path)
        : file_(path, std::ios::app | std::ios::out),
          to_file_(file_.is_open()) {}

    void write(const std::string& entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (to_file_) {
            file_ << entry << "\n";
            file_.flush();
        } else {
            std::cerr << entry << "\n";
        }
    }
};

#endif

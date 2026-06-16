#ifndef STATIC_FILE_HANDLER_HPP
#define STATIC_FILE_HANDLER_HPP

#include <string>
#include <sys/stat.h>

struct HttpRequest;
struct HttpResponse;

class StaticFileHandler {
public:
    HttpResponse serve_static_file(const std::string& resolved_path, const HttpRequest& request) const;

private:
    bool check_accept(const HttpRequest& request, const std::string& content_type) const;
    static std::string generate_etag(const struct stat& st);
};

#endif


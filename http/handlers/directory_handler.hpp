#ifndef DIRECTORY_HANDLER_HPP
#define DIRECTORY_HANDLER_HPP

#include <string>

class StaticFileHandler;
struct HttpRequest;
struct HttpResponse;

class DirectoryHandler {
public:
    HttpResponse serve_directory_index(const std::string& dir_path,
                                       const HttpRequest& request,
                                       const std::string& document_root,
                                       const StaticFileHandler& static_files) const;
};

#endif


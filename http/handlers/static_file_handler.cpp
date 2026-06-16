#include "handlers/static_file_handler.hpp"

#include "core/http_request.hpp"
#include "core/http_response.hpp"
#include "core/request_handler_utils.hpp"

#include <cstdio>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Parse "bytes=start-end" or "bytes=start-" from a Range header value.
// Returns {valid, start, end}; end == SIZE_MAX signals "open-ended" (to EOF).
struct RangeSpec { bool valid; size_t start; size_t end; };

RangeSpec parse_range(const std::string& header, size_t file_size) {
    const std::string prefix = "bytes=";
    if (header.compare(0, prefix.size(), prefix) != 0)
        return {false, 0, 0};

    std::string spec = header.substr(prefix.size());
    size_t dash = spec.find('-');
    if (dash == std::string::npos) return {false, 0, 0};

    std::string start_str = spec.substr(0, dash);
    std::string end_str   = spec.substr(dash + 1);

    size_t start = 0;
    size_t end   = file_size > 0 ? file_size - 1 : 0;

    if (start_str.empty() && end_str.empty()) return {false, 0, 0};

    if (start_str.empty()) {
        // suffix range: bytes=-N → last N bytes
        size_t suffix = std::stoull(end_str);
        start = (suffix >= file_size) ? 0 : file_size - suffix;
        end   = file_size > 0 ? file_size - 1 : 0;
    } else {
        start = std::stoull(start_str);
        if (!end_str.empty()) end = std::stoull(end_str);
    }

    if (start > end || end >= file_size) return {false, 0, 0};
    return {true, start, end};
}

}  // namespace

// Format: "<inode>-<size>-<mtime>" — same scheme Apache uses.
std::string StaticFileHandler::generate_etag(const struct stat& st) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"%lx-%llx-%llx\"",
                  static_cast<unsigned long>(st.st_ino),
                  static_cast<unsigned long long>(st.st_size),
                  static_cast<unsigned long long>(st.st_mtime));
    return buf;
}

HttpResponse StaticFileHandler::serve_static_file(const std::string& resolved_path, const HttpRequest& request) const {
    struct stat st;
    if (stat(resolved_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return HttpResponse(StatusCode::NotFound);
    }

    time_t mtime = st.st_mtime;
    std::string content_type = content_type_from_suffix(resolved_path);
    std::string etag = generate_etag(st);

    // If-None-Match check (ETag-based conditional).
    std::string if_none_match = get_header(request.headers, "if-none-match");
    if (!if_none_match.empty() && if_none_match == etag) {
        HttpResponse not_modified(StatusCode::NotModified);
        not_modified.headers["Date"]          = format_http_date(time(nullptr));
        not_modified.headers["Server"]        = "Server/1.0";
        not_modified.headers["ETag"]          = etag;
        return not_modified;
    }

    // If-Modified-Since check (date-based conditional).
    std::string if_modified = get_header(request.headers, "if-modified-since");
    if (!if_modified.empty()) {
        auto [parsed_ok, if_modified_time] = parse_http_date(if_modified);
        if (parsed_ok) {
            time_t current_time = time(nullptr);
            if (if_modified_time <= current_time && mtime <= if_modified_time) {
                HttpResponse not_modified(StatusCode::NotModified);
                not_modified.headers["Date"]   = format_http_date(current_time);
                not_modified.headers["Server"] = "Server/1.0";
                not_modified.headers["ETag"]   = etag;
                return not_modified;
            }
        }
    }

    if (!check_accept(request, content_type)) {
        return HttpResponse(StatusCode::NotAcceptable);
    }

    // Open the file once — used for both sendfile (200) and range sendfile (206).
    int file_fd = open(resolved_path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        return HttpResponse(StatusCode::InternalServerError);
    }

    // Range request — sendfile with offset and length.
    std::string range_header = get_header(request.headers, "range");
    if (!range_header.empty()) {
        RangeSpec range = parse_range(range_header, static_cast<size_t>(st.st_size));
        if (!range.valid) {
            close(file_fd);
            HttpResponse bad_range(StatusCode::RangeNotSatisfiable);
            bad_range.headers["Content-Range"] = "bytes */" + std::to_string(st.st_size);
            return bad_range;
        }

        size_t length = range.end - range.start + 1;
        HttpResponse resp(StatusCode::PartialContent);
        resp.headers["Date"]           = format_http_date(time(nullptr));
        resp.headers["Server"]         = "Server/1.0";
        resp.headers["Content-Type"]   = content_type;
        resp.headers["Last-Modified"]  = format_http_date(mtime);
        resp.headers["ETag"]           = etag;
        resp.headers["Cache-Control"]  = "max-age=3600";
        resp.headers["Content-Range"]  = "bytes " + std::to_string(range.start) +
                                         "-" + std::to_string(range.end) +
                                         "/" + std::to_string(st.st_size);
        resp.headers["Content-Length"] = std::to_string(length);
        resp.body_fd        = file_fd;
        resp.body_fd_offset = static_cast<off_t>(range.start);
        resp.body_fd_size   = static_cast<off_t>(length);
        return resp;
    }

    // Normal 200 response — sendfile the whole file.
    HttpResponse resp(StatusCode::Ok);
    resp.headers["Date"]           = format_http_date(time(nullptr));
    resp.headers["Server"]         = "Server/1.0";
    resp.headers["Content-Type"]   = content_type;
    resp.headers["Last-Modified"]  = format_http_date(mtime);
    resp.headers["ETag"]           = etag;
    resp.headers["Cache-Control"]  = "max-age=3600";
    resp.headers["Content-Length"] = std::to_string(st.st_size);
    resp.body_fd        = file_fd;
    resp.body_fd_offset = 0;
    resp.body_fd_size   = st.st_size;
    return resp;
}

bool StaticFileHandler::check_accept(const HttpRequest& request, const std::string& content_type) const {
    std::string accept = get_header(request.headers, "accept");
    if (accept.empty()) return true;
    std::istringstream iss(accept);
    std::string token;
    while (std::getline(iss, token, ',')) {
        size_t semicolon = token.find(';');
        std::string base = (semicolon != std::string::npos) ? token.substr(0, semicolon) : token;
        std::string type = trim(base, " \t");
        if (type.empty()) continue;

        if (type == "*/*") return true;
        if (type == content_type) return true;

        if (type.size() >= 2 && type.back() == '*' && type[type.size() - 2] == '/' &&
            content_type.size() >= type.size() - 1 &&
            content_type.compare(0, type.size() - 1, type, 0, type.size() - 1) == 0) {
            return true;  // e.g. text/* matches text/plain
        }
    }
    return false;
}

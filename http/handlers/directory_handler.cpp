#include "handlers/directory_handler.hpp"

#include "core/http_request.hpp"
#include "core/http_response.hpp"
#include "core/request_handler_utils.hpp"
#include "handlers/static_file_handler.hpp"

#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct DirEntry {
    std::string name;
    bool        is_dir = false;
    off_t       size   = 0;
    time_t      mtime  = 0;
};

std::string format_size(off_t bytes) {
    if (bytes < 1024) return std::to_string(bytes);
    double kb = bytes / 1024.0;
    if (kb < 1024.0) {
        char buf[16]; snprintf(buf, sizeof(buf), "%.1fK", kb); return buf;
    }
    double mb = kb / 1024.0;
    char buf[16]; snprintf(buf, sizeof(buf), "%.1fM", mb); return buf;
}

std::string format_mtime(time_t t) {
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return buf;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else                out += c;
    }
    return out;
}

std::string build_listing(const std::string& uri_path, const std::string& dir_path) {
    DIR* d = opendir(dir_path.c_str());
    if (!d) return "";

    std::vector<DirEntry> entries;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const char* nm = ent->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;

        DirEntry e;
        e.name = nm;
        std::string full = dir_path + "/" + nm;
        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            e.is_dir = S_ISDIR(st.st_mode);
            e.size   = st.st_size;
            e.mtime  = st.st_mtime;
        }
        entries.push_back(std::move(e));
    }
    closedir(d);

    // Directories first, then alphabetical within each group.
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });

    std::string base = uri_path;
    if (base.empty() || base.back() != '/') base += '/';

    std::string title = "Index of " + html_escape(base);
    std::string body = "<!DOCTYPE html>\n<html>\n<head>"
                       "<meta charset=\"utf-8\">"
                       "<title>" + title + "</title></head>\n"
                       "<body>\n<h1>" + title + "</h1><hr>\n<pre>\n";

    if (base != "/") body += "<a href=\"../\">../</a>\n";

    for (const auto& e : entries) {
        std::string display = html_escape(e.name) + (e.is_dir ? "/" : "");
        std::string href    = html_escape(e.name) + (e.is_dir ? "/" : "");
        // Fixed-width name column: 40 chars.
        std::string pad = display.size() < 40 ? std::string(40 - display.size(), ' ') : " ";
        body += "<a href=\"" + href + "\">" + display + "</a>" + pad;
        body += format_mtime(e.mtime) + "  ";
        body += (e.is_dir ? "-" : format_size(e.size));
        body += "\n";
    }

    body += "</pre><hr>\n</body>\n</html>\n";
    return body;
}

}  // namespace

HttpResponse DirectoryHandler::serve_directory_index(const std::string& dir_path,
                                                    const HttpRequest& request,
                                                    const std::string& document_root,
                                                    const StaticFileHandler& static_files) const {
    std::string base = dir_path;
    if (base.empty() || base.back() != '/') base += '/';

    bool is_doc_root =
        (normalize_path(trim_trailing_slash(dir_path)) == normalize_path(trim_trailing_slash(document_root)));
    std::string user_agent = get_header(request.headers, "user-agent");
    bool mobile = (user_agent.find("iPhone") != std::string::npos);

    std::string candidates[3];
    size_t n = 0;
    if (is_doc_root && mobile) {
        candidates[n++] = base + "index_m.html";
        candidates[n++] = base + "index.html";
    } else {
        candidates[n++] = base + "index.html";
    }

    for (size_t i = 0; i < n; ++i) {
        struct stat st;
        if (stat(candidates[i].c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            return static_files.serve_static_file(candidates[i], request);
        }
    }

    // No index file — generate auto-listing.
    std::string uri_path = request.uri;
    {
        auto q = uri_path.find('?');
        if (q != std::string::npos) uri_path = uri_path.substr(0, q);
    }
    std::string body = build_listing(uri_path, dir_path);
    if (body.empty()) return HttpResponse(StatusCode::Forbidden);

    HttpResponse resp(StatusCode::Ok);
    resp.headers["Content-Type"]   = "text/html; charset=utf-8";
    resp.headers["Content-Length"] = std::to_string(body.size());
    resp.body = std::move(body);
    return resp;
}

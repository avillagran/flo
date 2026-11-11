#include "core/clipboard.h"
#include "core/fsutil.h"
#include "core/uri.h"

#include <sstream>

namespace flo::clipboard {

static std::vector<std::string> lines_of(const std::string &data) {
    std::vector<std::string> out;
    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0' || line.back() == ' '))
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::string gnome_copied_files(const std::vector<std::string> &paths, bool cut) {
    std::string out = cut ? "cut" : "copy";
    for (const auto &p : paths)
        out += "\n" + path_to_uri(absolute_path(p));
    return out;
}

std::string uri_list(const std::vector<std::string> &paths) {
    std::string out;
    for (const auto &p : paths)
        out += path_to_uri(absolute_path(p)) + "\r\n";
    return out;
}

std::string plain_text(const std::vector<std::string> &paths) {
    std::string out;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i)
            out += '\n';
        out += absolute_path(paths[i]);
    }
    return out;
}

std::vector<std::string> offered_types(bool cut) {
    std::vector<std::string> t{"x-special/gnome-copied-files", "text/uri-list"};
    if (cut)
        t.push_back("application/x-kde-cutselection");
    t.push_back("text/plain;charset=utf-8");
    t.push_back("text/plain");
    return t;
}

std::vector<std::string> parse_gnome_copied_files(const std::string &data, bool &cut) {
    cut = false;
    std::vector<std::string> paths;
    const auto lines = lines_of(data);
    if (lines.empty())
        return paths;
    cut = lines[0] == "cut";
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string p = uri_to_path(lines[i]);
        if (!p.empty())
            paths.push_back(p);
    }
    return paths;
}

std::vector<std::string> parse_uri_list(const std::string &data) {
    std::vector<std::string> paths;
    for (const auto &line : lines_of(data)) {
        if (line.empty() || line[0] == '#')
            continue;
        const std::string p = uri_to_path(line);
        if (!p.empty())
            paths.push_back(p);
    }
    return paths;
}

std::vector<std::string> parse_any(const std::string &mime, const std::string &data, bool &cut) {
    cut = false;
    if (mime == "x-special/gnome-copied-files")
        return parse_gnome_copied_files(data, cut);
    if (mime == "text/uri-list")
        return parse_uri_list(data);
    std::vector<std::string> paths;
    for (const auto &line : lines_of(data)) {
        if (line.compare(0, 7, "file://") == 0) {
            const std::string p = uri_to_path(line);
            if (!p.empty())
                paths.push_back(p);
        } else if (!line.empty() && line[0] == '/') {
            paths.push_back(clean_path(line));
        } else if (!line.empty()) {
            return {}; // ordinary text, not a list of files
        }
    }
    return paths;
}

} // namespace flo::clipboard

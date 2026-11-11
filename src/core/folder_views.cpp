#include "core/folder_views.h"

#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace flo {

namespace {
const char kHeader[] = "# Flo folder views, format 1";

bool parse_zoom(const std::string &s, int &out) {
    if (s.empty() || s.size() > 3)
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    int v = std::atoi(s.c_str());
    if (v < 65 || v > 180)
        return false;
    out = v;
    return true;
}
} // namespace

std::string FolderViews::path() { return xdg::config_home() + "/flo/folder-views"; }

std::string FolderViews::serialize(const std::vector<FolderView> &views) {
    std::string out = std::string(kHeader) + "\n";
    for (const auto &v : views)
        out += std::string(view_mode_name(v.view)) + " " + sort_mode_name(v.sort) + " " +
               std::to_string(v.zoom_percent) + " " + path_to_uri(v.path) + "\n";
    return out;
}

bool FolderViews::parse(const std::string &text, std::vector<FolderView> &out, std::string &why) {
    out.clear();
    if (text.size() > kMaxBytes) {
        why = "The folder views file is too big.";
        return false;
    }
    std::istringstream in(text);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (!header) {
            if (line != kHeader) {
                why = "The folder views file is not Flo's format 1.";
                return false;
            }
            header = true;
            continue;
        }
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream f(line);
        std::string view, sort, zoom, uri, extra;
        f >> view >> sort >> zoom >> uri;
        FolderView v;
        if (uri.empty() || (f >> extra) || !parse_view_mode(view, v.view) || !parse_sort_mode(sort, v.sort) ||
            !parse_zoom(zoom, v.zoom_percent)) {
            why = "A line of the folder views file can't be read.";
            return false;
        }
        v.path = uri_to_path(uri);
        if (v.path.empty() || v.path[0] != '/' || v.path.size() > kMaxPath) {
            why = "A folder of the folder views file is not a local path.";
            return false;
        }
        out.push_back(std::move(v));
        if (out.size() > kMaxFolders) {
            why = "The folder views file has more than 256 folders.";
            return false;
        }
    }
    if (!header && !text.empty()) {
        why = "The folder views file is not Flo's format 1.";
        return false;
    }
    std::sort(out.begin(), out.end(), [](const FolderView &a, const FolderView &b) { return a.path < b.path; });
    out.erase(std::unique(out.begin(), out.end(), [](const FolderView &a, const FolderView &b) { return a.path == b.path; }),
              out.end());
    return true;
}

bool FolderViews::load(const std::string &file) {
    views_.clear();
    error_.clear();
    writable_ = true;
    std::string text;
    if (!exists_nofollow(file))
        return true;
    if (!read_file_capped(file, kMaxBytes + 1, text, false)) {
        writable_ = false;
        error_ = "The folder views file can't be read; it is kept unchanged.";
        return false;
    }
    if (!parse(text, views_, error_)) {
        views_.clear();
        writable_ = false;
        error_ += " It is kept unchanged.";
        return false;
    }
    return true;
}

bool FolderViews::save(const std::string &file) const {
    if (!writable_)
        return false;
    if (!make_dirs(path_dirname(file), 0700))
        return false;
    return atomic_write_file(file, serialize(views_), 0600);
}

const FolderView *FolderViews::get(const std::string &folder) const {
    auto it = std::lower_bound(views_.begin(), views_.end(), folder,
                               [](const FolderView &v, const std::string &p) { return v.path < p; });
    return it != views_.end() && it->path == folder ? &*it : nullptr;
}

bool FolderViews::set(const FolderView &v) {
    if (!writable_) {
        error_ = "The folder views file can't be changed.";
        return false;
    }
    if (v.path.empty() || v.path[0] != '/' || v.path.size() > kMaxPath) {
        error_ = "Only local folders keep a view.";
        return false;
    }
    FolderView c = v;
    c.zoom_percent = std::clamp(c.zoom_percent, 65, 180);
    auto it = std::lower_bound(views_.begin(), views_.end(), c.path,
                               [](const FolderView &a, const std::string &p) { return a.path < p; });
    if (it != views_.end() && it->path == c.path) {
        *it = c;
        return true;
    }
    if (views_.size() >= kMaxFolders) {
        error_ = "Up to 256 folders keep a view; forget one first.";
        return false;
    }
    views_.insert(it, c);
    return true;
}

bool FolderViews::forget(const std::string &folder) {
    if (!writable_)
        return false;
    auto it = std::lower_bound(views_.begin(), views_.end(), folder,
                               [](const FolderView &a, const std::string &p) { return a.path < p; });
    if (it == views_.end() || it->path != folder)
        return false;
    views_.erase(it);
    return true;
}

} // namespace flo

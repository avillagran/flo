#include "core/starred.h"

#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <glib.h>

#include <algorithm>
#include <set>
#include <sstream>

namespace flo {

namespace {
const char kHeader[] = "# Flo starred items, format 1";
}

std::string Starred::path() { return xdg::data_home() + "/flo/starred"; }

std::string Starred::serialize() const {
    std::string out = std::string(kHeader) + "\n";
    for (const auto &p : items_)
        out += path_to_uri(p) + "\n";
    return out;
}

bool Starred::parse(const std::string &text, std::vector<std::string> &out) {
    out.clear();
    if (text.size() > kMaxBytes || !g_utf8_validate(text.data(), gssize(text.size()), nullptr))
        return false;
    std::istringstream in(text);
    std::string line;
    std::set<std::string> seen;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::string p = uri_to_path(line);
        if (p.empty() || p[0] != '/' || !seen.insert(p).second)
            continue;
        out.push_back(p);
        if (out.size() >= kMaxItems)
            break;
    }
    return true;
}

bool Starred::load(const std::string &file) {
    items_.clear();
    index_.clear();
    error_.clear();
    writable_ = true;
    if (!exists_nofollow(file))
        return true;
    std::string text;
    if (!read_file_capped(file, kMaxBytes, text, false) || !parse(text, items_)) {
        items_.clear();
        writable_ = false;
        error_ = "The starred list can't be read; it is kept unchanged.";
        reindex();
        return false;
    }
    reindex();
    return true;
}

bool Starred::save(const std::string &file) const {
    if (!writable_ || !make_dirs(path_dirname(file), 0700))
        return false;
    return atomic_write_file(file, serialize(), 0600);
}

void Starred::reindex() { index_ = std::unordered_set<std::string>(items_.begin(), items_.end()); }

bool Starred::contains(const std::string &p) const { return index_.count(p) > 0; }

bool Starred::add(const std::vector<std::string> &paths) {
    std::vector<std::string> fresh;
    for (const auto &p : paths)
        if (!p.empty() && p[0] == '/' && !contains(p) && std::find(fresh.begin(), fresh.end(), p) == fresh.end())
            fresh.push_back(p);
    if (items_.size() + fresh.size() > kMaxItems) {
        error_ = "Up to 10000 starred items.";
        return false;
    }
    items_.insert(items_.end(), fresh.begin(), fresh.end());
    index_.insert(fresh.begin(), fresh.end());
    return true;
}

int Starred::remove(const std::vector<std::string> &paths) {
    size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const std::string &p) {
                                    return std::find(paths.begin(), paths.end(), p) != paths.end();
                                }),
                 items_.end());
    if (items_.size() != before)
        reindex();
    return int(before - items_.size());
}

int Starred::toggle(const std::vector<std::string> &paths) {
    if (paths.empty() || !writable_)
        return 0;
    bool all = std::all_of(paths.begin(), paths.end(), [this](const std::string &p) { return contains(p); });
    if (all)
        return -remove(paths);
    size_t before = items_.size();
    if (!add(paths))
        return 0;
    return int(items_.size() - before);
}

} // namespace flo

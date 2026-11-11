#include "core/session.h"

#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace flo {

namespace {
const char kHeader[] = "# Flo session, format 1";

std::string enc(const std::string &s) { return percent_encode_path(s); }

bool dec(const std::string &s, std::string &out) {
    if (!percent_decode(s, out))
        return false;
    for (char c : out)
        if (c == '\n' || c == '\r')
            return false;
    return true;
}

bool abs_path(const std::string &uri, std::string &out) {
    out = uri_to_path(uri);
    return !out.empty() && out[0] == '/' && out.size() <= 4096;
}

bool small_int(const std::string &s, int lo, int hi, int &out) {
    if (s.empty() || s.size() > 4)
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    int v = std::atoi(s.c_str());
    if (v < lo || v > hi)
        return false;
    out = v;
    return true;
}
} // namespace

std::string Session::path() { return xdg::state_home() + "/flo/session"; }

std::string Session::serialize() const {
    std::string o = std::string(kHeader) + "\n";
    o += "active " + std::to_string(active) + "\n";
    for (const auto &t : tabs) {
        o += "tab " + path_to_uri(t.folder) + "\n";
        o += std::string("view ") + view_mode_name(t.view) + "\n";
        o += std::string("sort ") + sort_mode_name(t.sort) + "\n";
        o += "zoom " + std::to_string(t.zoom_percent) + "\n";
        for (const auto &b : t.back)
            o += "back " + path_to_uri(b) + "\n";
        for (const auto &f : t.forward)
            o += "forward " + path_to_uri(f) + "\n";
        if (!t.filter.empty())
            o += "filter " + enc(t.filter) + "\n";
        for (const auto &s : t.selected)
            o += "select " + enc(s) + "\n";
        if (!t.current.empty())
            o += "current " + enc(t.current) + "\n";
    }
    for (const auto &c : closed)
        o += "closed " + path_to_uri(c) + "\n";
    return o;
}

bool Session::parse(const std::string &text, Session &out, std::string &why) {
    out = Session();
    why.clear();
    auto bad = [&](const char *w) {
        why = w;
        out = Session();
        return false;
    };
    if (text.size() > kMaxBytes)
        return bad("The session file is too big.");
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kHeader)
        return bad("The session file is not Flo's format 1.");
    bool have_active = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos)
            return bad("A line of the session file can't be read.");
        std::string key = line.substr(0, sp), val = line.substr(sp + 1);
        TabState *t = out.tabs.empty() ? nullptr : &out.tabs.back();
        std::string p;
        if (key == "active") {
            if (!small_int(val, 0, int(kMaxTabs) - 1, out.active))
                return bad("The session's active tab can't be read.");
            have_active = true;
        } else if (key == "tab") {
            if (out.tabs.size() >= kMaxTabs || !abs_path(val, p))
                return bad("A tab of the session file can't be read.");
            out.tabs.push_back(TabState());
            out.tabs.back().folder = p;
        } else if (key == "closed") {
            if (out.closed.size() >= kMaxClosed || !abs_path(val, p))
                return bad("A closed tab of the session file can't be read.");
            out.closed.push_back(p);
        } else if (!t) {
            return bad("A tab setting comes before any tab.");
        } else if (key == "view") {
            if (!parse_view_mode(val, t->view))
                return bad("A tab's view can't be read.");
        } else if (key == "sort") {
            if (!parse_sort_mode(val, t->sort))
                return bad("A tab's sort can't be read.");
        } else if (key == "zoom") {
            if (!small_int(val, 65, 180, t->zoom_percent))
                return bad("A tab's zoom can't be read.");
        } else if (key == "back" || key == "forward") {
            auto &h = key == "back" ? t->back : t->forward;
            if (t->back.size() + t->forward.size() >= kMaxHistory || !abs_path(val, p))
                return bad("A tab's history can't be read.");
            h.push_back(p);
        } else if (key == "filter") {
            if (!t->filter.empty() || !dec(val, t->filter) || t->filter.size() > 256)
                return bad("A tab's filter can't be read.");
        } else if (key == "select") {
            std::string n;
            if (t->selected.size() >= kMaxSelected || !dec(val, n) || n.empty() || n.find('/') != std::string::npos)
                return bad("A tab's selection can't be read.");
            t->selected.push_back(n);
        } else if (key == "current") {
            if (!dec(val, t->current) || t->current.find('/') != std::string::npos)
                return bad("A tab's focused item can't be read.");
        } else {
            return bad("The session file has an unknown line.");
        }
    }
    if (out.tabs.empty())
        return bad("The session file has no tabs.");
    if (!have_active || out.active >= int(out.tabs.size()))
        return bad("The session's active tab is missing.");
    return true;
}

bool Session::load(Session &out, const std::string &file) {
    out = Session();
    std::string text, why;
    if (!read_file_capped(file, kMaxBytes, text, false))
        return false;
    return parse(text, out, why);
}

bool Session::save(const std::string &file) const {
    if (tabs.empty() || tabs.size() > kMaxTabs)
        return false;
    if (!make_dirs(path_dirname(file), 0700))
        return false;
    return atomic_write_file(file, serialize(), 0600);
}

} // namespace flo

#include "core/settings.h"
#include "core/fsutil.h"
#include "core/rail_order.h"
#include "core/xdg.h"

#include <cstdlib>
#include <sstream>

namespace flo {

static std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

bool Ini::parse(const std::string &text) {
    lines_.clear();
    std::istringstream in(text);
    std::string raw, section;
    while (std::getline(in, raw)) {
        Line l;
        l.raw = raw;
        const std::string t = trim(raw);
        if (!t.empty() && t[0] == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            l.section = section;
        } else if (!t.empty() && t[0] != '#' && t[0] != ';' && t.find('=') != std::string::npos) {
            l.section = section;
            l.key = trim(t.substr(0, t.find('=')));
            l.value = trim(t.substr(t.find('=') + 1));
        } else {
            l.section = section;
        }
        lines_.push_back(std::move(l));
    }
    return true;
}

std::string Ini::serialize() const {
    std::string out;
    for (const auto &l : lines_)
        out += (l.key.empty() ? l.raw : l.key + "=" + l.value) + "\n";
    return out;
}

std::string Ini::get(const std::string &section, const std::string &key, const std::string &fallback) const {
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it)
        if (it->section == section && it->key == key)
            return it->value;
    return fallback;
}

bool Ini::has(const std::string &section, const std::string &key) const {
    for (const auto &l : lines_)
        if (l.section == section && l.key == key)
            return true;
    return false;
}

void Ini::set(const std::string &section, const std::string &key, const std::string &value) {
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it)
        if (it->section == section && it->key == key) {
            it->value = value;
            return;
        }
    // Append after the section's last line, creating the section if needed.
    std::size_t header = lines_.size(), insert_at = lines_.size();
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].section != section)
            continue;
        if (header == lines_.size())
            header = i;
        insert_at = i + 1;
    }
    if (header == lines_.size() || (lines_[header].key.empty() && trim(lines_[header].raw) != "[" + section + "]")) {
        Line h;
        h.section = section;
        h.raw = "[" + section + "]";
        lines_.push_back(h);
        header = lines_.size() - 1;
        insert_at = lines_.size();
    }
    // Keep blank lines after the last key, not before it.
    while (insert_at > header + 1 && lines_[insert_at - 1].key.empty() && trim(lines_[insert_at - 1].raw).empty())
        --insert_at;
    Line l;
    l.section = section;
    l.key = key;
    l.value = value;
    lines_.insert(lines_.begin() + static_cast<long>(insert_at), l);
}

const char *view_mode_name(ViewMode m) {
    switch (m) {
    case ViewMode::Grid: return "grid";
    case ViewMode::List: return "list";
    case ViewMode::Columns: return "columns";
    case ViewMode::Groups: return "groups";
    case ViewMode::Space: return "space";
    }
    return "groups";
}

bool parse_view_mode(const std::string &s, ViewMode &out) {
    if (s == "grid") out = ViewMode::Grid;
    else if (s == "list") out = ViewMode::List;
    else if (s == "columns") out = ViewMode::Columns;
    else if (s == "groups") out = ViewMode::Groups;
    else if (s == "space") out = ViewMode::Space;
    else return false;
    return true;
}

std::string Settings::path() { return xdg::config_home() + "/flo/flo.conf"; }

static bool parse_bool(const std::string &s, bool &out) {
    if (s == "true" || s == "1" || s == "yes") out = true;
    else if (s == "false" || s == "0" || s == "no") out = false;
    else return false;
    return true;
}

static bool parse_int(const std::string &s, int lo, int hi, int &out) {
    if (s.empty())
        return false;
    char *end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end || v < lo || v > hi)
        return false;
    out = static_cast<int>(v);
    return true;
}

void Settings::read_from(const Ini &ini) {
    parse_view_mode(ini.get("view", "mode"), view);
    parse_sort_mode(ini.get("view", "sort"), sort);
    parse_bool(ini.get("view", "hidden"), show_hidden);
    parse_bool(ini.get("view", "details"), details_pane);
    parse_bool(ini.get("view", "rail-pinned"), rail_pinned);
    parse_bool(ini.get("view", "auto-sizes"), auto_sizes);
    parse_bool(ini.get("view", "operation-bar"), operation_bar);
    shortcuts = profile_from_key(ini.get("keys", "profile", profile_key(shortcuts)));
    parse_bool(ini.get("view", "thumbnails"), thumbnails);
    parse_int(ini.get("view", "thumbnail-size"), 64, 1024, thumbnail_size);
    parse_int(ini.get("window", "width"), 200, 20000, window_width);
    parse_int(ini.get("window", "height"), 150, 20000, window_height);
    parse_bool(ini.get("window", "maximized"), window_maximized);
    rail_order = rail_order::parse(ini.get("rail", "order"));
    const std::string c = ini.get("view", "corners", corners);
    if (c == "omarchy" || c == "square" || c == "rounded")
        corners = c;
    local_theme = ini.get("view", "theme") == "tokyo-night";
}

void Settings::write_to(Ini &ini) const {
    ini.set("view", "mode", view_mode_name(view));
    ini.set("view", "sort", sort_mode_name(sort));
    ini.set("view", "hidden", show_hidden ? "true" : "false");
    ini.set("view", "details", details_pane ? "true" : "false");
    ini.set("view", "rail-pinned", rail_pinned ? "true" : "false");
    ini.set("view", "auto-sizes", auto_sizes ? "true" : "false");
    ini.set("view", "operation-bar", operation_bar ? "true" : "false");
    ini.set("keys", "profile", profile_key(shortcuts));
    ini.set("view", "thumbnails", thumbnails ? "true" : "false");
    ini.set("view", "thumbnail-size", std::to_string(thumbnail_size));
    ini.set("window", "width", std::to_string(window_width));
    ini.set("window", "height", std::to_string(window_height));
    ini.set("window", "maximized", window_maximized ? "true" : "false");
    ini.set("view", "corners", corners);
    ini.set("view", "theme", local_theme ? "tokyo-night" : "system");
    if (!rail_order.empty() || ini.has("rail", "order"))
        ini.set("rail", "order", rail_order::serialize(rail_order));
}

Settings Settings::load(const std::string &file) {
    Settings s;
    std::string text;
    if (read_file_capped(file, 256 * 1024, text)) {
        Ini ini;
        ini.parse(text);
        s.read_from(ini);
    }
    return s;
}

bool Settings::save(const std::string &file) const {
    Ini ini;
    std::string text;
    if (read_file_capped(file, 256 * 1024, text))
        ini.parse(text);
    write_to(ini);
    if (!make_dirs(path_dirname(file), 0700))
        return false;
    return atomic_write_file(file, ini.serialize(), 0644);
}

} // namespace flo

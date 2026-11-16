#include "core/default_apps.h"

#include "core/fsutil.h"
#include "core/xdg.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace flo::default_apps {

namespace {

constexpr std::size_t kMaxConfig = 1 << 20; // config files are small

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        lines.push_back(cur);
    return lines;
}

std::string join_lines(const std::vector<std::string> &lines) {
    std::string out;
    for (const auto &l : lines)
        out += l + "\n";
    return out;
}

bool is_group(const std::string &line, std::string *name = nullptr) {
    std::string t = trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']')
        return false;
    if (name)
        *name = t;
    return true;
}

bool key_of(const std::string &line, std::string &key, std::string &value) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#')
        return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos)
        return false;
    key = trim(t.substr(0, eq));
    value = trim(t.substr(eq + 1));
    return true;
}

bool read_text(const std::string &file, std::string &out, bool &exists) {
    out.clear();
    struct stat st;
    exists = ::stat(file.c_str(), &st) == 0;
    if (!exists)
        return true;
    return read_file_capped(file, kMaxConfig, out, true);
}

// Writes a config file atomically. A symbolic link (dotfiles kept in a
// repository) keeps pointing where it did: the file it points to is
// replaced instead of the link.
std::string write_text(const std::string &file, const std::string &text) {
    std::string target = file;
    struct stat st;
    if (::lstat(file.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        char buf[PATH_MAX];
        if (!::realpath(file.c_str(), buf))
            return "The file links to something that does not exist.";
        target = buf;
    }
    if (!make_dirs(path_dirname(target), 0755))
        return "The folder can't be created.";
    mode_t mode = 0644;
    if (::stat(target.c_str(), &st) == 0)
        mode = st.st_mode & 07777;
    if (!atomic_write_file(target, text, mode))
        return "The file can't be written.";
    return {};
}

bool safe_value(const std::string &s) {
    return !s.empty() && s.size() <= 255 && s.find_first_of("\n\r=;[]") == std::string::npos;
}

std::string escape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '%' || c == ';' || c < 0x20) {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        } else {
            out += char(c);
        }
    }
    return out;
}

std::string unescape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string env_or(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    return v && *v ? std::string(v) : fallback;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

bool file_exists(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string user_portal_dir() { return path_join(xdg::config_home(), "xdg-desktop-portal"); }

// The .portal files installed (user data first), by name ("gtk").
std::string find_portal(const std::string &name) {
    std::vector<std::string> dirs = {xdg::data_home()};
    for (const auto &d : xdg::data_dirs())
        dirs.push_back(d);
    for (const auto &d : dirs) {
        std::string p = path_join(path_join(d, "xdg-desktop-portal/portals"), name + ".portal");
        if (file_exists(p))
            return p;
    }
    return {};
}

bool portal_implements(const std::string &name, const std::string &iface) {
    std::string p = find_portal(name), text;
    bool exists = false;
    if (p.empty() || !read_text(p, text, exists))
        return false;
    for (const auto &i : split(ini_get(text, "portal", "Interfaces"), ';'))
        if (trim(i) == iface)
            return true;
    return false;
}

} // namespace

// --- INI text -------------------------------------------------------------------

std::string ini_get(const std::string &text, const std::string &group, const std::string &key, bool *found) {
    if (found)
        *found = false;
    const std::string want = "[" + group + "]";
    bool in = false;
    for (const auto &line : split_lines(text)) {
        std::string g;
        if (is_group(line, &g)) {
            in = g == want;
            continue;
        }
        std::string k, v;
        if (in && key_of(line, k, v) && k == key) {
            if (found)
                *found = true;
            return v;
        }
    }
    return {};
}

std::string ini_set(const std::string &text, const std::string &group, const std::string &key,
                    const std::string &value) {
    const std::string want = "[" + group + "]";
    std::vector<std::string> lines = split_lines(text), out;
    bool in = false, done = false, seen = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        std::string g;
        if (is_group(line, &g)) {
            if (in && !done) {
                // The end of the group: the key goes after its last entry.
                size_t at = out.size();
                while (at > 0 && trim(out[at - 1]).empty())
                    --at;
                out.insert(out.begin() + long(at), key + "=" + value);
                done = true;
            }
            in = g == want;
            seen = seen || in;
            out.push_back(line);
            continue;
        }
        std::string k, v;
        if (in && !done && key_of(line, k, v) && k == key) {
            out.push_back(key + "=" + value);
            done = true;
            continue;
        }
        out.push_back(line);
    }
    if (in && !done) {
        size_t at = out.size();
        while (at > 0 && trim(out[at - 1]).empty())
            --at;
        out.insert(out.begin() + long(at), key + "=" + value);
        done = true;
    }
    if (!seen) {
        if (!out.empty() && !trim(out.back()).empty())
            out.push_back("");
        out.push_back(want);
        out.push_back(key + "=" + value);
    }
    return join_lines(out);
}

std::string ini_remove(const std::string &text, const std::string &group, const std::string &key) {
    const std::string want = "[" + group + "]";
    std::vector<std::string> out;
    bool in = false, done = false;
    for (const auto &line : split_lines(text)) {
        std::string g;
        if (is_group(line, &g)) {
            in = g == want;
            out.push_back(line);
            continue;
        }
        std::string k, v;
        if (in && !done && key_of(line, k, v) && k == key) {
            done = true;
            continue;
        }
        out.push_back(line);
    }
    return join_lines(out);
}

// --- Previous -------------------------------------------------------------------

std::string Previous::encode() const {
    if (!valid)
        return {};
    return std::string(had ? "1" : "0") + ";" + escape(value) + ";" + (had2 ? "1" : "0") + ";" + escape(value2);
}

Previous Previous::decode(const std::string &text) {
    Previous p;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : text) {
        if (c == ';') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    if (parts.size() != 4 || (parts[0] != "0" && parts[0] != "1") || (parts[2] != "0" && parts[2] != "1"))
        return p;
    p.had = parts[0] == "1";
    p.value = unescape(parts[1]);
    p.had2 = parts[2] == "1";
    p.value2 = unescape(parts[3]);
    p.valid = true;
    return p;
}

// --- File manager -----------------------------------------------------------------

std::string mimeapps_path() { return path_join(xdg::config_home(), "mimeapps.list"); }

std::string user_default(const std::string &file, const std::string &mime) {
    std::string text;
    bool exists = false;
    if (!read_text(file, text, exists))
        return {};
    std::vector<std::string> ids = split(ini_get(text, "Default Applications", mime), ';');
    return ids.empty() ? std::string() : trim(ids[0]);
}

std::string set_default(const std::string &file, const std::string &mime, const std::string &id, Previous *before) {
    if (!safe_value(mime) || !safe_value(id) || id.size() < 9 || id.compare(id.size() - 8, 8, ".desktop") != 0)
        return "Not a valid type or application.";
    std::string text;
    bool exists = false;
    if (!read_text(file, text, exists))
        return "The file can't be read.";
    Previous p;
    p.value = ini_get(text, "Default Applications", mime, &p.had);
    p.value2 = ini_get(text, "Added Associations", mime, &p.had2);
    p.valid = true;
    // Files' set_default_in(): the default, and the id first among the
    // added associations.
    std::string out = ini_set(text, "Default Applications", mime, id);
    std::string ids = id + ";";
    for (const auto &other : split(p.value2, ';'))
        if (trim(other) != id && !trim(other).empty())
            ids += trim(other) + ";";
    out = ini_set(out, "Added Associations", mime, ids);
    std::string err = write_text(file, out);
    if (err.empty() && before)
        *before = p;
    return err;
}

std::string restore_default(const std::string &file, const std::string &mime, const Previous &before) {
    if (!before.valid)
        return "Nothing to restore.";
    std::string text;
    bool exists = false;
    if (!read_text(file, text, exists))
        return "The file can't be read.";
    std::string out = before.had ? ini_set(text, "Default Applications", mime, before.value)
                                 : ini_remove(text, "Default Applications", mime);
    out = before.had2 ? ini_set(out, "Added Associations", mime, before.value2)
                      : ini_remove(out, "Added Associations", mime);
    return write_text(file, out);
}

// --- File chooser -------------------------------------------------------------------

std::vector<std::string> current_desktops() {
    std::vector<std::string> out;
    for (auto d : split(env_or("XDG_CURRENT_DESKTOP", ""), ':')) {
        std::transform(d.begin(), d.end(), d.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        out.push_back(d);
    }
    return out;
}

std::vector<std::string> portal_config_dirs() {
    std::vector<std::string> dirs = {user_portal_dir()};
    for (const auto &d : split(env_or("XDG_CONFIG_DIRS", "/etc/xdg"), ':'))
        dirs.push_back(path_join(d, "xdg-desktop-portal"));
    dirs.push_back("/etc/xdg-desktop-portal");
    dirs.push_back(path_join(xdg::data_home(), "xdg-desktop-portal"));
    for (const auto &d : xdg::data_dirs())
        dirs.push_back(path_join(d, "xdg-desktop-portal"));
    return dirs;
}

std::string effective_portal_config(const std::vector<std::string> &desktops) {
    const auto dirs = portal_config_dirs();
    for (const auto &d : dirs)
        for (const auto &desk : desktops)
            if (file_exists(path_join(d, desk + "-portals.conf")))
                return path_join(d, desk + "-portals.conf");
    for (const auto &d : dirs)
        if (file_exists(path_join(d, "portals.conf")))
            return path_join(d, "portals.conf");
    return {};
}

std::string portal_config_path(const std::vector<std::string> &desktops, std::string *seed_from) {
    if (seed_from)
        seed_from->clear();
    const std::string user = user_portal_dir();
    for (const auto &desk : desktops) {
        std::string p = path_join(user, desk + "-portals.conf");
        struct stat st;
        if (::lstat(p.c_str(), &st) == 0)
            return p;
    }
    std::string p = path_join(user, "portals.conf");
    struct stat st;
    if (::lstat(p.c_str(), &st) != 0 && seed_from)
        *seed_from = effective_portal_config(desktops);
    return p;
}

std::string file_chooser_backend(const std::vector<std::string> &desktops) {
    // An interface line: the user's desktop file, the user's portals.conf,
    // then the system's files in their order.
    std::vector<std::string> files;
    const std::string user = user_portal_dir();
    bool user_desktop = false;
    for (const auto &desk : desktops)
        if (file_exists(path_join(user, desk + "-portals.conf"))) {
            files.push_back(path_join(user, desk + "-portals.conf"));
            user_desktop = true;
            break;
        }
    if (!user_desktop && file_exists(path_join(user, "portals.conf")))
        files.push_back(path_join(user, "portals.conf"));
    std::string eff = effective_portal_config(desktops);
    if (!eff.empty())
        files.push_back(eff);
    for (const auto &f : files) {
        std::string text;
        bool exists = false, found = false;
        if (!read_text(f, text, exists))
            continue;
        std::string v = ini_get(text, "preferred", kFileChooserKey, &found);
        if (found && !v.empty())
            return trim(split(v, ';').empty() ? v : split(v, ';')[0]);
    }
    // The default list of the configuration in use.
    std::string text;
    bool exists = false;
    if (!files.empty() && read_text(files.front(), text, exists))
        for (const auto &name : split(ini_get(text, "preferred", "default"), ';'))
            if (portal_implements(trim(name), kFileChooserKey))
                return trim(name);
    return {};
}

std::string set_file_chooser(const std::vector<std::string> &desktops, const std::string &backend, Previous *before) {
    if (!safe_value(backend))
        return "Not a valid portal backend.";
    std::string seed;
    const std::string file = portal_config_path(desktops, &seed);
    std::string text;
    bool exists = false;
    if (!read_text(file, text, exists))
        return "The file can't be read.";
    if (!exists && !seed.empty()) {
        bool seed_exists = false;
        if (!read_text(seed, text, seed_exists))
            text.clear();
    }
    Previous p;
    p.value = ini_get(text, "preferred", kFileChooserKey, &p.had);
    p.valid = true;
    std::string err = write_text(file, ini_set(text, "preferred", kFileChooserKey, backend));
    if (err.empty() && before)
        *before = p;
    return err;
}

std::string restore_file_chooser(const std::vector<std::string> &desktops, const Previous &before) {
    if (!before.valid)
        return "Nothing to restore.";
    const std::string file = portal_config_path(desktops);
    std::string text;
    bool exists = false;
    if (!read_text(file, text, exists) || !exists)
        return "The file can't be read.";
    std::string out = before.had ? ini_set(text, "preferred", kFileChooserKey, before.value)
                                 : ini_remove(text, "preferred", kFileChooserKey);
    return write_text(file, out);
}

std::string portal_file_text() {
    return std::string("[portal]\nDBusName=") + kPortalBusName +
           "\nInterfaces=org.freedesktop.impl.portal.FileChooser;\n";
}

std::string dbus_service_text(const std::string &exe) {
    return std::string("[D-BUS Service]\nName=") + kPortalBusName + "\nExec=" + exe + " --portal\n";
}

std::string installed_portal_file() { return find_portal(kPortalName); }

std::string installed_dbus_service() {
    std::vector<std::string> dirs = {xdg::data_home()};
    for (const auto &d : xdg::data_dirs())
        dirs.push_back(d);
    for (const auto &d : dirs) {
        std::string p = path_join(path_join(d, "dbus-1/services"), std::string(kPortalBusName) + ".service");
        if (file_exists(p))
            return p;
    }
    return {};
}

std::string install_portal_backend(const std::string &exe) {
    if (exe.empty() || exe[0] != '/' || exe.find('\n') != std::string::npos)
        return "Flo's program path is unknown.";
    const std::string portal = path_join(xdg::data_home(), "xdg-desktop-portal/portals/flo.portal");
    const std::string svc =
        path_join(xdg::data_home(), "dbus-1/services/" + std::string(kPortalBusName) + ".service");
    std::string have = installed_portal_file();
    if (have.empty() || have == portal) {
        std::string err = write_text(portal, portal_file_text());
        if (!err.empty())
            return err;
    }
    have = installed_dbus_service();
    if (have.empty() || have == svc) {
        std::string err = write_text(svc, dbus_service_text(exe));
        if (!err.empty())
            return err;
    }
    return {};
}

} // namespace flo::default_apps

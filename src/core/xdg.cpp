#include "core/xdg.h"
#include "core/fsutil.h"

#include <cstdlib>
#include <pwd.h>
#include <sstream>
#include <unistd.h>

namespace flo::xdg {

static std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// The spec ignores relative values.
static std::string env_abs(const char *name) {
    std::string v = env(name);
    return !v.empty() && v[0] == '/' ? v : std::string();
}

std::string home() {
    std::string h = env_abs("HOME");
    if (!h.empty())
        return h;
    if (const passwd *pw = ::getpwuid(::getuid()))
        if (pw->pw_dir)
            return pw->pw_dir;
    return "/";
}

std::string config_home() {
    std::string v = env_abs("XDG_CONFIG_HOME");
    return v.empty() ? home() + "/.config" : v;
}
std::string data_home() {
    std::string v = env_abs("XDG_DATA_HOME");
    return v.empty() ? home() + "/.local/share" : v;
}
std::string cache_home() {
    std::string v = env_abs("XDG_CACHE_HOME");
    return v.empty() ? home() + "/.cache" : v;
}
std::string state_home() {
    std::string v = env_abs("XDG_STATE_HOME");
    return v.empty() ? home() + "/.local/state" : v;
}
std::string runtime_dir() { return env_abs("XDG_RUNTIME_DIR"); }

std::vector<std::string> data_dirs() {
    std::string v = env("XDG_DATA_DIRS");
    if (v.empty())
        v = "/usr/local/share:/usr/share";
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string part;
    while (std::getline(ss, part, ':'))
        if (!part.empty() && part[0] == '/')
            out.push_back(part);
    return out;
}

std::vector<std::pair<std::string, std::string>> parse_user_dirs(const std::string &text,
                                                                  const std::string &home_dir) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // XDG_DOWNLOAD_DIR="$HOME/Downloads"
        std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#')
            continue;
        line = line.substr(start);
        if (line.compare(0, 4, "XDG_") != 0)
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq < 8 || line.compare(eq - 4, 4, "_DIR") != 0)
            continue;
        const std::string key = line.substr(4, eq - 8);
        std::string value = line.substr(eq + 1);
        if (value.size() < 2 || value.front() != '"')
            continue;
        const auto close = value.find('"', 1);
        if (close == std::string::npos)
            continue;
        value = value.substr(1, close - 1);
        std::string path;
        if (value.compare(0, 5, "$HOME") == 0)
            path = home_dir + value.substr(5);
        else if (!value.empty() && value[0] == '/')
            path = value;
        else
            continue;
        // Unescape \" and \\ as the user-dirs format allows.
        std::string unescaped;
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '\\' && i + 1 < path.size())
                ++i;
            unescaped += path[i];
        }
        out.emplace_back(key, clean_path(unescaped));
    }
    return out;
}

std::string user_dir(const std::string &key) {
    static const std::pair<const char *, const char *> fallback[] = {
        {"DESKTOP", "Desktop"}, {"DOWNLOAD", "Downloads"}, {"TEMPLATES", "Templates"},
        {"PUBLICSHARE", "Public"}, {"DOCUMENTS", "Documents"}, {"MUSIC", "Music"},
        {"PICTURES", "Pictures"}, {"VIDEOS", "Videos"}};
    const std::string h = home();
    std::string text;
    if (read_file_capped(config_home() + "/user-dirs.dirs", 64 * 1024, text)) {
        for (const auto &kv : parse_user_dirs(text, h))
            if (kv.first == key)
                return clean_path(kv.second) == clean_path(h) ? std::string() : kv.second;
    }
    for (const auto &f : fallback)
        if (key == f.first)
            return h + "/" + f.second;
    return {};
}

std::string find_program(const std::string &name) {
    if (name.empty())
        return {};
    if (name.find('/') != std::string::npos)
        return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    std::stringstream ss(env("PATH").empty() ? "/usr/local/bin:/usr/bin:/bin" : env("PATH"));
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty())
            continue;
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0 && !is_dir(candidate))
            return candidate;
    }
    return {};
}

std::string test_hook(const char *name) {
    const char *run = std::getenv("FLO_TEST_RUN");
    if (!run || !*run)
        return {};
    const char *v = std::getenv(name);
    return v ? v : "";
}

} // namespace flo::xdg

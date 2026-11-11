#include "ui/util.h"

#include "core/fsutil.h"
#include "core/process.h"
#include "core/trash.h"
#include "core/uri.h"
#include "core/xdg.h"

#include "fltk-omarchy/i18n.h"

#include <FL/Fl.H>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <map>

namespace flo::ui {

void defer(std::function<void()> fn) {
    auto *heap = new std::function<void()>(std::move(fn));
    Fl::add_timeout(0, [](void *d) {
        auto *f = static_cast<std::function<void()> *>(d);
        (*f)();
        delete f;
    }, heap);
}

std::string FolderModel::path(int i) const {
    if (i < 0 || i >= int(entries.size()))
        return {};
    const std::string &n = entries[size_t(i)].name;
    return !n.empty() && n[0] == '/' ? n : path_join(base, n);
}

int FolderModel::index_of_name(const std::string &name) const {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].name == name || display_name(entries[i]) == name)
            return int(i);
    return -1;
}

int FolderModel::index_of_path(const std::string &p) const {
    if (p.empty())
        return -1;
    for (int i = 0; i < count(); ++i)
        if (path(i) == p)
            return i;
    return -1;
}

std::string display_name(const Entry &e) {
    auto slash = e.name.rfind('/');
    return slash == std::string::npos ? e.name : e.name.substr(slash + 1);
}

std::string kind_icon(const Entry &e) {
    if (e.is_dir)
        return "folder";
    switch (e.kind) {
    case Kind::Image: return "file-image";
    case Kind::Video: return "file-video";
    case Kind::Audio: return "file-music";
    case Kind::Pdf: return "file-text";
    case Kind::Text: return "file-code";
    case Kind::Archive: return "file-archive";
    default: return "file";
    }
}

fo::Rgb kind_color(const Entry &e) {
    const fo::Theme &t = fo::theme();
    if (e.is_dir)
        return t.accent();
    switch (e.kind) {
    case Kind::Pdf: return t.red();
    case Kind::Text: return t.blue();
    case Kind::Audio: return t.magenta();
    case Kind::Video: return t.orange();
    case Kind::Archive: return t.yellow();
    case Kind::Image: return t.cyan();
    default: return t.secondary_foreground();
    }
}

std::string size_text(const Entry &e) {
    if (e.is_dir)
        return e.item_count < 0 ? std::string() : fo::trn("%n items", e.item_count);
    return fo::i18n::format_size(uint64_t(std::max<int64_t>(0, e.size)));
}

std::string when_text(int64_t mtime_ns) {
    if (mtime_ns <= 0)
        return {};
    return fo::i18n::format_when(std::time_t(mtime_ns / 1000000000));
}

std::string place_icon(const std::string &icon) {
    static const std::map<std::string, std::string> m = {
        {"home", "home"},          {"documents", "file-text"}, {"downloads", "download"},
        {"pictures", "image"},     {"music", "music"},         {"videos", "video"},
        {"desktop", "monitor"},    {"templates", "file-plus"}, {"public", "share-2"},
        {"dropbox", "cloud"},      {"recent", "clock"},        {"trash", "trash-2"},
        {"drive", "hard-drive"},   {"removable", "usb"},       {"network", "globe"},
        {"omarchy", "settings"},
    };
    auto it = m.find(icon);
    return it == m.end() ? "folder" : it->second;
}

std::string tilde(const std::string &path) {
    const std::string home = xdg::home();
    if (path == home)
        return "~";
    if (path_inside(path, home))
        return "~" + path.substr(home.size());
    return path;
}

std::string expand_typed(const std::string &text, const std::string &cwd) {
    std::string t = text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n'))
        t.pop_back();
    while (!t.empty() && t.front() == ' ')
        t.erase(0, 1);
    if (t.empty())
        return {};
    std::string p = uri_to_path(t);
    if (!p.empty())
        return clean_path(p);
    if (t == "~")
        return xdg::home();
    if (t.compare(0, 2, "~/") == 0)
        return clean_path(xdg::home() + t.substr(1));
    if (t[0] == '/')
        return clean_path(t);
    return clean_path(path_join(cwd, t));
}

std::string type_description(const std::string &mime) {
    static std::map<std::string, std::string> cache;
    if (mime.empty())
        return {};
    auto it = cache.find(mime);
    if (it != cache.end())
        return it->second;
    std::string out;
    if (gchar *d = g_content_type_get_description(mime.c_str())) {
        out = d;
        g_free(d);
    }
    if (!out.empty())
        out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    cache[mime] = out;
    return out;
}

bool in_trash_folder(const std::string &path) { return trash::in_trash(path); }

bool spawn_detached(const std::vector<std::string> &argv, const std::string &cwd) {
    if (argv.empty())
        return false;
    std::vector<char *> args;
    for (const auto &a : argv)
        args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        // The grandchild runs the program in its own session; the child exits
        // at once so nothing is left for Flo to reap.
        if (fork() != 0)
            _exit(0);
        setsid();
        signal(SIGPIPE, SIG_DFL);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
        }
        for (int fd = 3; fd < 1024; ++fd)
            close(fd);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0)
            _exit(127);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void open_default(const std::string &path) {
    if (!xdg::find_program("xdg-open").empty())
        spawn_detached({"xdg-open", path}, path_dirname(path));
    else
        spawn_detached({"gio", "open", path}, path_dirname(path));
}

bool open_terminal(const std::string &folder) {
    std::vector<std::vector<std::string>> tries = {{"xdg-terminal-exec"}};
    if (const char *t = std::getenv("TERMINAL"); t && *t)
        tries.push_back({t});
    for (const char *t : {"alacritty", "kitty", "foot", "ghostty", "wezterm", "gnome-terminal", "konsole", "xterm"})
        tries.push_back({t});
    for (const auto &argv : tries)
        if (!xdg::find_program(argv[0]).empty())
            return spawn_detached(argv, folder);
    return false;
}

std::vector<AppChoice> apps_for(const std::string &mime) {
    std::vector<AppChoice> out;
    if (mime.empty())
        return out;
    GAppInfo *def = g_app_info_get_default_for_type(mime.c_str(), FALSE);
    std::string def_id = def && g_app_info_get_id(def) ? g_app_info_get_id(def) : "";
    GList *all = g_app_info_get_all_for_type(mime.c_str());
    for (GList *l = all; l; l = l->next) {
        auto *info = G_APP_INFO(l->data);
        if (!g_app_info_should_show(info))
            continue;
        AppChoice c;
        c.id = g_app_info_get_id(info) ? g_app_info_get_id(info) : "";
        c.name = g_app_info_get_display_name(info) ? g_app_info_get_display_name(info) : c.id;
        c.is_default = !def_id.empty() && c.id == def_id;
        if (c.id.empty())
            continue;
        if (c.is_default)
            out.insert(out.begin(), c);
        else
            out.push_back(c);
    }
    g_list_free_full(all, g_object_unref);
    if (def)
        g_object_unref(def);
    return out;
}

bool launch_app(const std::string &id, const std::vector<std::string> &paths) {
    GDesktopAppInfo *info = g_desktop_app_info_new(id.c_str());
    if (!info)
        return false;
    GList *files = nullptr;
    for (const auto &p : paths)
        files = g_list_append(files, g_file_new_for_path(p.c_str()));
    GError *err = nullptr;
    bool ok = g_app_info_launch(G_APP_INFO(info), files, nullptr, &err);
    if (err)
        g_error_free(err);
    g_list_free_full(files, g_object_unref);
    g_object_unref(info);
    return ok;
}

std::string set_default_app(const std::string &mime, const std::string &id) {
    if (mime.empty() || id.empty())
        return "No type to set a default for.";
    if (!xdg::find_program("xdg-mime").empty()) {
        RunOptions o;
        o.timeout_ms = 10000;
        ProcessResult r = run_process({"xdg-mime", "default", id, mime}, o);
        if (r.ok())
            return {};
        std::string why = r.err;
        while (!why.empty() && (why.back() == '\n' || why.back() == ' '))
            why.pop_back();
        return why.empty() ? "xdg-mime failed." : why;
    }
    GDesktopAppInfo *info = g_desktop_app_info_new(id.c_str());
    if (!info)
        return "That application is no longer installed.";
    GError *err = nullptr;
    bool ok = g_app_info_set_as_default_for_type(G_APP_INFO(info), mime.c_str(), &err);
    std::string why = err && err->message ? err->message : "";
    if (err)
        g_error_free(err);
    g_object_unref(info);
    return ok ? std::string() : (why.empty() ? std::string("The default couldn't be changed.") : why);
}

std::string uri_list_for_drag(const std::vector<std::string> &paths) {
    std::string s;
    for (const auto &p : paths)
        s += path_to_uri(p) + "\r\n";
    return s;
}

} // namespace flo::ui

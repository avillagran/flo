#include "core/places.h"
#include "core/fsutil.h"
#include "core/gmain_thread.h"
#include "core/natural_sort.h"
#include "core/trash.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <dirent.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo {
namespace places {

std::string display_name(const std::string &folder_name) {
    return folder_name == "omarchy" ? std::string("Omarchy") : folder_name;
}

std::vector<Place> home_places(const std::string &home, const std::string &dropbox_status) {
    // The XDG folders keep their own glyphs, wherever user-dirs points.
    std::map<std::string, std::string> icons;
    static const std::pair<const char *, const char *> roles[] = {
        {"DOCUMENTS", "documents"}, {"DOWNLOAD", "downloads"}, {"PICTURES", "pictures"}, {"MUSIC", "music"},
        {"VIDEOS", "videos"},       {"DESKTOP", "desktop"},     {"TEMPLATES", "templates"}, {"PUBLICSHARE", "public"}};
    for (const auto &r : roles) {
        const std::string dir = xdg::user_dir(r.first);
        if (!dir.empty())
            icons[clean_path(dir)] = r.second;
    }
    icons[clean_path(home + "/Dropbox")] = "dropbox";

    std::vector<std::string> names;
    if (DIR *d = ::opendir(home.c_str())) {
        while (dirent *e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.')
                continue;
            if (is_dir(path_join(home, n)))
                names.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end(), natural_less);
    std::vector<Place> out;
    for (const auto &n : names) {
        Place p;
        p.path = clean_path(path_join(home, n));
        p.name = display_name(n);
        auto it = icons.find(p.path);
        p.icon = it == icons.end() ? "folder" : it->second;
        if (p.icon == "dropbox")
            p.status = dropbox_status;
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Place> views() {
    Place recent;
    recent.name = "Recents";
    recent.path = "recent://";
    recent.icon = "recent";
    Place t;
    t.name = "Trash";
    t.path = trash::home_trash() + "/files";
    t.icon = "trash";
    return {recent, t};
}

std::string bookmarks_path() { return xdg::config_home() + "/gtk-3.0/bookmarks"; }

std::vector<Bookmark> parse_bookmarks(const std::string &text) {
    std::vector<Bookmark> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty())
            continue;
        Bookmark b;
        const auto space = line.find(' ');
        b.uri = line.substr(0, space);
        if (space != std::string::npos)
            b.label = line.substr(space + 1);
        b.path = uri_to_path(b.uri);
        if (!b.path.empty())
            b.path = clean_path(b.path);
        if (uri_scheme(b.uri).empty())
            continue;
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<Bookmark> read_bookmarks(const std::string &file) {
    std::string text;
    if (!read_file_capped(file, 1u << 20, text))
        return {};
    return parse_bookmarks(text);
}

// Home's own visible folders (and home itself) are already under Home.
static bool listed_under_home(const std::string &path, const std::string &home) {
    const std::string h = clean_path(home);
    const std::string base = path_basename(path);
    return path == h || (path_dirname(path) == h && !base.empty() && base[0] != '.');
}

std::vector<Place> pinned(const std::vector<Bookmark> &bookmarks, const std::string &home) {
    std::vector<Place> out;
    std::set<std::string> seen;
    for (const auto &b : bookmarks) {
        if (b.path.empty() || listed_under_home(b.path, home) || !seen.insert(b.path).second || !is_dir(b.path))
            continue;
        Place p;
        p.path = b.path;
        p.name = b.label.empty() ? display_name(path_basename(b.path)) : b.label;
        p.icon = "folder";
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Place> remote_bookmarks(const std::vector<Bookmark> &bookmarks) {
    std::vector<Place> out;
    for (const auto &b : bookmarks) {
        if (!b.path.empty())
            continue;
        Place p;
        p.uri = b.uri;
        p.name = b.label.empty() ? b.uri : b.label;
        p.icon = "network";
        p.mounted = false;
        out.push_back(std::move(p));
    }
    return out;
}

bool is_pinned(const std::string &path, const std::string &file) {
    const std::string clean = clean_path(absolute_path(path));
    for (const auto &b : read_bookmarks(file))
        if (b.path == clean)
            return true;
    return false;
}

static bool write_lines(const std::string &file, const std::vector<std::string> &lines) {
    std::string text;
    for (const auto &l : lines)
        text += l + "\n";
    return make_dirs(path_dirname(file), 0700) && atomic_write_file(file, text, 0644);
}

static std::vector<std::string> raw_lines(const std::string &file) {
    std::vector<std::string> lines;
    std::string text;
    if (read_file_capped(file, 1u << 20, text)) {
        std::istringstream in(text);
        std::string l;
        while (std::getline(in, l))
            if (!l.empty())
                lines.push_back(l);
    }
    return lines;
}

std::string pin(const std::string &path, const std::string &home, const std::string &file) {
    const std::string clean = clean_path(absolute_path(path));
    if (!is_dir(clean))
        return "Only folders can be pinned.";
    if (listed_under_home(clean, home))
        return display_name(path_basename(clean)) + " is already under Home.";
    if (is_pinned(clean, file))
        return {};
    auto lines = raw_lines(file);
    lines.push_back(path_to_uri(clean));
    if (!write_lines(file, lines))
        return "Couldn't save the pin to " + file + ".";
    return {};
}

bool unpin(const std::string &path, const std::string &file) {
    const std::string clean = clean_path(absolute_path(path));
    auto lines = raw_lines(file);
    const auto before = lines.size();
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [&](const std::string &l) {
                                   const std::string p = uri_to_path(l.substr(0, l.find(' ')));
                                   return !p.empty() && clean_path(p) == clean;
                               }),
                lines.end());
    return lines.size() != before && write_lines(file, lines);
}

std::vector<Place> physical_drives(const std::vector<MountInfo> &mounts_in, const std::string &system_device,
                                   const std::vector<std::pair<std::string, std::string>> &labels) {
    // Where the system itself lives; a drive mounted here is the computer.
    static const char *const system_roots[] = {"/",     "/boot", "/efi",  "/home", "/var", "/usr", "/opt",
                                               "/srv",  "/root", "/tmp",  "/nix",  "/snap", "/swap"};
    auto mounts = mounts_in;
    // Shortest path first, so a drive mounted twice is listed at its top.
    std::stable_sort(mounts.begin(), mounts.end(),
                     [](const MountInfo &a, const MountInfo &b) { return a.root_path.size() < b.root_path.size(); });
    std::vector<Place> out;
    std::set<std::string> seen;
    for (const auto &m : mounts) {
        bool system = m.root_path.compare(0, 2, "/.") == 0;
        for (const char *r : system_roots) {
            const std::string root = r;
            if (m.root_path == root || (root != "/" && path_inside(m.root_path, root)))
                system = true;
        }
        const bool block = m.source.compare(0, 5, "/dev/") == 0 && m.source.compare(0, 9, "/dev/loop") != 0 &&
                           m.source.compare(0, 9, "/dev/zram") != 0;
        if (system || !block || is_pseudo_fs(m.fs_type) || m.source == system_device || !seen.insert(m.source).second)
            continue;
        Place p;
        p.path = m.root_path;
        for (const auto &l : labels)
            if (l.first == m.source)
                p.name = l.second;
        if (p.name.empty())
            p.name = path_basename(m.root_path);
        if (p.name.empty())
            p.name = m.root_path;
        p.icon = "drive";
        p.id = m.source;
        // /run/media and /media are where udisks puts removable media.
        p.removable = path_inside(m.root_path, "/run/media") || path_inside(m.root_path, "/media");
        if (p.removable)
            p.icon = "removable";
        p.can_unmount = true;
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Place> drives() {
    const auto mounts = read_mountinfo();
    std::string system_device;
    for (const auto &m : mounts)
        if (m.root_path == "/")
            system_device = m.source;
    std::vector<std::pair<std::string, std::string>> labels;
    if (DIR *d = ::opendir("/dev/disk/by-label")) {
        while (dirent *e = ::readdir(d)) {
            if (e->d_name[0] == '.')
                continue;
            const std::string target = real_path(std::string("/dev/disk/by-label/") + e->d_name);
            // udev escapes spaces as \x20.
            std::string label = e->d_name, plain;
            for (std::size_t i = 0; i < label.size(); ++i) {
                if (label.compare(i, 4, "\\x20") == 0) {
                    plain += ' ';
                    i += 3;
                } else {
                    plain += label[i];
                }
            }
            if (!target.empty())
                labels.emplace_back(target, plain);
        }
        ::closedir(d);
    }
    return physical_drives(mounts, system_device, labels);
}

} // namespace places

// ---------------------------------------------------------------------------

struct Devices::Impl {
    EventQueue::Poster poster;
    GVolumeMonitor *monitor = nullptr; // GLib thread only
    std::function<void(std::vector<Place>)> on_changed;
    mutable std::mutex mutex;
    std::vector<Place> last;
    std::vector<gulong> handlers;
    std::weak_ptr<Impl> self;

    // GLib thread: physical drives from the mount table plus GIO volumes
    // that aren't mounted yet (a plugged USB stick).
    void refresh() {
        std::vector<Place> list = places::drives();
        if (monitor) {
            GList *vols = g_volume_monitor_get_volumes(monitor);
            for (GList *l = vols; l; l = l->next) {
                GVolume *v = G_VOLUME(l->data);
                GMount *m = g_volume_get_mount(v);
                if (m) {
                    // Mounted: already listed from the mount table; mark ejectable.
                    GFile *root = g_mount_get_root(m);
                    if (char *path = g_file_get_path(root)) {
                        for (auto &p : list)
                            if (p.path == path) {
                                p.can_eject = g_mount_can_eject(m);
                                p.can_unmount = g_mount_can_unmount(m);
                            }
                        g_free(path);
                    }
                    g_object_unref(root);
                    g_object_unref(m);
                    continue;
                }
                GDrive *drive = g_volume_get_drive(v);
                const bool removable = drive && (g_drive_is_removable(drive) || g_drive_is_media_removable(drive));
                if (g_volume_can_mount(v) && (removable || !drive)) {
                    Place p;
                    char *name = g_volume_get_name(v);
                    char *id = g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UUID);
                    if (!id)
                        id = g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
                    p.name = name ? name : "Volume";
                    p.id = id ? id : p.name;
                    p.icon = "removable";
                    p.removable = true;
                    p.mounted = false;
                    p.can_eject = g_volume_can_eject(v);
                    g_free(name);
                    g_free(id);
                    list.push_back(std::move(p));
                }
                if (drive)
                    g_object_unref(drive);
            }
            g_list_free_full(vols, g_object_unref);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            last = list;
        }
        auto cb = on_changed;
        auto weak = self;
        if (cb)
            poster.post([weak, cb, list] {
                if (!weak.expired())
                    cb(list);
            });
    }

    static void changed(GVolumeMonitor *, gpointer, gpointer data) { static_cast<Impl *>(data)->refresh(); }

    GVolume *find_volume(const std::string &id) {
        GVolume *found = nullptr;
        GList *vols = g_volume_monitor_get_volumes(monitor);
        for (GList *l = vols; l && !found; l = l->next) {
            GVolume *v = G_VOLUME(l->data);
            for (const char *kind : {G_VOLUME_IDENTIFIER_KIND_UUID, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE}) {
                char *vid = g_volume_get_identifier(v, kind);
                char *name = g_volume_get_name(v);
                if ((vid && id == vid) || (name && id == name))
                    found = G_VOLUME(g_object_ref(v));
                g_free(vid);
                g_free(name);
                if (found)
                    break;
            }
        }
        g_list_free_full(vols, g_object_unref);
        return found;
    }
};

Devices::Devices(EventQueue &queue) : impl_(std::make_shared<Impl>()) {
    impl_->poster = queue.poster();
    impl_->self = impl_;
}

Devices::~Devices() {
    auto impl = impl_;
    GMainThread::instance().invoke_sync([impl] {
        if (impl->monitor) {
            for (gulong h : impl->handlers)
                g_signal_handler_disconnect(impl->monitor, h);
            g_object_unref(impl->monitor);
            impl->monitor = nullptr;
        }
        impl->on_changed = nullptr;
    });
}

void Devices::watch(std::function<void(std::vector<Place>)> on_changed) {
    auto impl = impl_;
    GMainThread::instance().invoke([impl, on_changed] {
        impl->on_changed = on_changed;
        if (!impl->monitor) {
            impl->monitor = g_volume_monitor_get();
            for (const char *sig : {"volume-added", "volume-removed", "mount-added", "mount-removed", "mount-changed",
                                    "drive-connected", "drive-disconnected"})
                impl->handlers.push_back(
                    g_signal_connect(impl->monitor, sig, G_CALLBACK(Impl::changed), impl.get()));
        }
        impl->refresh();
    });
}

std::vector<Place> Devices::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last;
}

namespace {
struct OpData {
    std::weak_ptr<Devices::Impl> impl;
    EventQueue::Poster poster;
    Devices::Done done;
    std::string path;
};
void finish(OpData *d, bool ok, const std::string &path, GError *error) {
    const std::string msg = error ? error->message : "";
    auto done = d->done;
    auto weak = d->impl;
    if (done)
        d->poster.post([weak, done, ok, path, msg] {
            if (!weak.expired())
                done(ok, path, msg);
        });
    if (error)
        g_error_free(error);
    delete d;
}
} // namespace

void Devices::mount(const std::string &volume_id, Done done) {
    auto impl = impl_;
    GMainThread::instance().invoke([impl, volume_id, done] {
        if (!impl->monitor)
            impl->monitor = g_volume_monitor_get();
        GVolume *v = impl->find_volume(volume_id);
        auto *d = new OpData{impl, impl->poster, done, {}};
        if (!v) {
            finish(d, false, {}, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such volume"));
            return;
        }
        g_volume_mount(v, G_MOUNT_MOUNT_NONE, nullptr, nullptr,
                       [](GObject *src, GAsyncResult *res, gpointer data) {
                           GError *err = nullptr;
                           const bool ok = g_volume_mount_finish(G_VOLUME(src), res, &err);
                           std::string path;
                           if (ok) {
                               if (GMount *m = g_volume_get_mount(G_VOLUME(src))) {
                                   GFile *root = g_mount_get_root(m);
                                   if (char *p = g_file_get_path(root)) {
                                       path = p;
                                       g_free(p);
                                   }
                                   g_object_unref(root);
                                   g_object_unref(m);
                               }
                           }
                           finish(static_cast<OpData *>(data), ok, path, err);
                       },
                       d);
        g_object_unref(v);
    });
}

// Test hook (FLO_TEST_MOUNTINFO, inside a private test run only): a drive
// from the mountinfo fixture is "unmounted" by taking its line out of the
// fixture, and the run's ejects.log records it. Nothing on the system is
// touched.
static bool fixture_unmount(const std::string &path, bool eject) {
    const std::string fixture = xdg::test_hook("FLO_TEST_MOUNTINFO");
    std::string text;
    if (fixture.empty() || !read_file_capped(fixture, 1u << 20, text))
        return false;
    std::string kept;
    bool found = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto rows = parse_mountinfo(line);
        if (!found && rows.size() == 1 && rows[0].root_path == path) {
            found = true;
            continue;
        }
        kept += line + "\n";
    }
    if (!found || !atomic_write_file(fixture, kept, 0600))
        return false;
    const std::string run = xdg::test_hook("FLO_TEST_RUN");
    std::string log;
    read_file_capped(run + "/ejects.log", 1u << 20, log);
    atomic_write_file(run + "/ejects.log", log + (eject ? "eject " : "unmount ") + path + "\n", 0600);
    return true;
}

static void unmount_or_eject(const std::shared_ptr<Devices::Impl> &impl, const std::string &path, Devices::Done done,
                             bool eject) {
    GMainThread::instance().invoke([impl, path, done, eject] {
        auto *d = new OpData{impl, impl->poster, done, path};
        if (fixture_unmount(path, eject)) {
            finish(d, true, path, nullptr);
            impl->refresh();
            return;
        }
        GFile *file = g_file_new_for_path(path.c_str());
        GMount *m = g_file_find_enclosing_mount(file, nullptr, nullptr);
        g_object_unref(file);
        if (!m) {
            finish(d, false, path, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED, "Not a mounted drive"));
            return;
        }
        GMountOperation *op = g_mount_operation_new();
        auto cb = [](GObject *src, GAsyncResult *res, gpointer data) {
            GError *err = nullptr;
            auto *od = static_cast<OpData *>(data);
            const bool ok = g_mount_eject_with_operation_finish(G_MOUNT(src), res, &err);
            finish(od, ok, od->path, err);
        };
        if (eject && g_mount_can_eject(m)) {
            g_mount_eject_with_operation(m, G_MOUNT_UNMOUNT_NONE, op, nullptr, cb, d);
        } else {
            g_mount_unmount_with_operation(
                m, G_MOUNT_UNMOUNT_NONE, op, nullptr,
                [](GObject *src, GAsyncResult *res, gpointer data) {
                    GError *err = nullptr;
                    auto *od = static_cast<OpData *>(data);
                    const bool ok = g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err);
                    finish(od, ok, od->path, err);
                },
                d);
        }
        g_object_unref(op);
        g_object_unref(m);
    });
}

void Devices::unmount(const std::string &path, Done done) { unmount_or_eject(impl_, path, std::move(done), false); }
void Devices::eject(const std::string &path, Done done) { unmount_or_eject(impl_, path, std::move(done), true); }

} // namespace flo

#include "core/places.h"
#include "core/network.h"
#include "core/fsutil.h"
#include "core/gmain_thread.h"
#include "core/trash.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>

#include <dirent.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo {
namespace places {

std::vector<Place> xdg_places(const std::string &home, const std::string &user_dirs) {
    const std::string h = clean_path(home);
    std::vector<Place> out;
    Place home_place;
    home_place.name = "Home";
    home_place.path = h;
    home_place.icon = "home";
    out.push_back(home_place);
    const auto named = xdg::parse_user_dirs(user_dirs, h);
    static const struct {
        const char *key, *name, *fallback, *icon;
    } roles[] = {{"DOWNLOAD", "Downloads", "Downloads", "downloads"}, {"DOCUMENTS", "Documents", "Documents", "documents"},
                 {"PICTURES", "Pictures", "Pictures", "pictures"},    {"MUSIC", "Music", "Music", "music"},
                 {"VIDEOS", "Videos", "Videos", "videos"},            {"DESKTOP", "Desktop", "Desktop", "desktop"}};
    for (const auto &r : roles) {
        std::string path = path_join(h, r.fallback);
        for (const auto &kv : named)
            if (kv.first == r.key)
                path = kv.second;
        path = clean_path(path);
        if (path == h || !is_dir(path))
            continue;
        Place p;
        p.name = r.name;
        p.path = path;
        p.icon = r.icon;
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Place> xdg_places() {
    std::string text;
    read_file_capped(xdg::config_home() + "/user-dirs.dirs", 64 * 1024, text);
    return xdg_places(xdg::home(), text);
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

std::vector<Place> pinned(const std::vector<Bookmark> &bookmarks) {
    std::vector<Place> out;
    std::set<std::string> seen;
    for (const auto &b : bookmarks) {
        if (b.path.empty() || !seen.insert(b.path).second || !is_dir(b.path))
            continue;
        Place p;
        p.path = b.path;
        p.name = b.label.empty() ? path_basename(b.path) : b.label;
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

std::string pin(const std::string &path, const std::string &file) {
    const std::string clean = clean_path(absolute_path(path));
    if (!is_dir(clean))
        return "Only folders can be pinned.";
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

std::string move_bookmark(const std::string &path, std::size_t index, const std::string &file) {
    const std::string clean = clean_path(absolute_path(path));
    auto lines = raw_lines(file);
    // The Bookmarks section's rows, as pinned() lists them, with their lines.
    std::vector<std::pair<std::string, std::size_t>> rows;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto b = parse_bookmarks(lines[i]);
        if (b.empty() || b[0].path.empty() || !seen.insert(b[0].path).second || !is_dir(b[0].path))
            continue;
        rows.push_back({b[0].path, i});
    }
    std::size_t from = rows.size();
    for (std::size_t k = 0; k < rows.size(); ++k)
        if (rows[k].first == clean)
            from = k;
    if (from == rows.size())
        return "The bookmark no longer exists.";
    const std::size_t line = rows[from].second;
    rows.erase(rows.begin() + std::ptrdiff_t(from));
    index = std::min(index, rows.size());
    if (index == from)
        return {};
    std::string text = lines[line];
    lines.erase(lines.begin() + std::ptrdiff_t(line));
    auto shifted = [&](std::size_t l) { return l > line ? l - 1 : l; };
    const std::size_t at = index < rows.size() ? shifted(rows[index].second) : shifted(rows.back().second) + 1;
    lines.insert(lines.begin() + std::ptrdiff_t(at), text);
    if (!write_lines(file, lines))
        return "Couldn't save the bookmarks to " + file + ".";
    return {};
}

std::string relabel(const std::string &path, const std::string &label, const std::string &file) {
    if (label.size() > 96)
        return "A label holds 1 to 96 bytes.";
    for (unsigned char c : label)
        if (c < 0x20 || c == 0x7f)
            return "Type a single line without control characters.";
    const std::string clean = clean_path(absolute_path(path));
    auto lines = raw_lines(file);
    bool found = false;
    for (auto &l : lines) {
        const std::string uri = l.substr(0, l.find(' '));
        const std::string p = uri_to_path(uri);
        if (p.empty() || clean_path(p) != clean)
            continue;
        std::string t = label;
        while (!t.empty() && t.back() == ' ')
            t.pop_back();
        while (!t.empty() && t.front() == ' ')
            t.erase(t.begin());
        l = t.empty() ? uri : uri + " " + t;
        found = true;
    }
    if (!found)
        return "This folder is not a favorite.";
    if (!write_lines(file, lines))
        return "Couldn't save the favorites to " + file + ".";
    return {};
}

std::string default_label(const std::string &path) {
    std::string name = path_basename(clean_path(path));
    for (auto &c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            c = '?';
    if (name.empty())
        name = "Folder";
    if (name.size() > 96) {
        size_t cut = 93;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
            --cut;
        name = name.substr(0, cut) + "\xE2\x80\xA6";
    }
    return name;
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
                        bool listed = false;
                        for (auto &p : list)
                            if (p.path == path) {
                                p.can_eject = g_mount_can_eject(m);
                                p.can_unmount = g_mount_can_unmount(m);
                                listed = true;
                            }
                        // A phone or camera (MTP, gphoto2) mounted by GVfs
                        // isn't in the mount table: keep it listed, at its
                        // GVfs path, so it can be opened and ejected.
                        GDrive *drive = g_volume_get_drive(v);
                        const bool removable =
                            drive && (g_drive_is_removable(drive) || g_drive_is_media_removable(drive));
                        if (drive)
                            g_object_unref(drive);
                        if (!listed && (removable || !drive) && !gvfs_root().empty() &&
                            path_inside(path, gvfs_root())) {
                            Place p;
                            char *name = g_volume_get_name(v);
                            p.name = name ? name : path_basename(path);
                            g_free(name);
                            p.path = path;
                            p.id = path;
                            p.icon = "removable";
                            p.removable = true;
                            p.can_eject = g_mount_can_eject(m);
                            p.can_unmount = g_mount_can_unmount(m);
                            list.push_back(std::move(p));
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

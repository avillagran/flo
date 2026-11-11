// The sidebar's places, as Files lists them (places.rs, places_sidebar.rs):
// Places (Home and the XDG user folders that exist), Bookmarks (GTK
// bookmarks), Views (Recents, Trash) and Drives (removable and extra
// internal disks, not the system's own), plus mount / unmount / eject
// through GIO.
#pragma once

#include "core/event_queue.h"
#include "core/fsutil.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo {

struct Place {
    std::string name;
    std::string path;   // local path ("recent://" for Recents); empty when not mounted
    std::string uri;    // remote bookmarks and unmounted volumes
    std::string icon;   // home documents downloads pictures music videos desktop
                        // folder recent trash drive removable network
    std::string id;     // volumes: identifier for Devices::mount()
    bool removable = false;
    bool mounted = true;
    bool can_unmount = false, can_eject = false;
};

namespace places {

// Files' places (places.rs xdg_places): Home, then the XDG user folders in
// Files' order (Downloads, Documents, Pictures, Music, Videos, Desktop),
// each only when it is an existing folder other than home. `user_dirs` is
// the text of user-dirs.dirs; a folder it doesn't name falls back to
// home/<English name>. Names are the English source strings ("Home",
// "Downloads", ...), translated where they are shown.
std::vector<Place> xdg_places(const std::string &home, const std::string &user_dirs);
// The same for xdg::home() and $XDG_CONFIG_HOME/user-dirs.dirs.
std::vector<Place> xdg_places();
// Recents and Trash.
std::vector<Place> views();

// GTK bookmarks: "URI[ label]" lines in $XDG_CONFIG_HOME/gtk-3.0/bookmarks.
struct Bookmark {
    std::string uri, label;
    std::string path; // for file:// bookmarks
};
std::string bookmarks_path();
std::vector<Bookmark> parse_bookmarks(const std::string &text);
std::vector<Bookmark> read_bookmarks(const std::string &file = bookmarks_path());
// Files' Bookmarks section: local, existing folders, no duplicates, in the
// file's order (a place may be bookmarked too, as in Files).
std::vector<Place> pinned(const std::vector<Bookmark> &bookmarks);
// Remote bookmarks (sftp://, smb://, ...), kept for GTK and listed.
std::vector<Place> remote_bookmarks(const std::vector<Bookmark> &bookmarks);
bool is_pinned(const std::string &path, const std::string &file = bookmarks_path());
// Adds a folder (written atomically; other lines, remote ones included, are
// kept as they were). Returns "" or a sentence saying why not.
std::string pin(const std::string &path, const std::string &file = bookmarks_path());
bool unpin(const std::string &path, const std::string &file = bookmarks_path());
// Files' favorite labels: sets the GTK label of a local bookmark ("" goes
// back to the folder's name). The folder itself is never renamed. Returns
// "" or a sentence saying why not (not a favorite, too long, a line break).
std::string relabel(const std::string &path, const std::string &label, const std::string &file = bookmarks_path());
// Files' default label for a new favorite: the folder's name, control
// characters replaced, at most 96 bytes.
std::string default_label(const std::string &path);

// Drives from a mount table (Folio's physicalDrives): block devices that
// aren't the system's own, the shortest mount point of each device.
// `labels` maps a device path to its filesystem label.
std::vector<Place> physical_drives(const std::vector<MountInfo> &mounts, const std::string &system_device,
                                   const std::vector<std::pair<std::string, std::string>> &labels = {});
// The same from /proc/self/mountinfo, /dev/disk/by-label and /sys.
std::vector<Place> drives();

} // namespace places

// Removable and mountable volumes through GIO's volume monitor, on the GLib
// thread. Everything reports on the UI thread.
class Devices {
public:
    explicit Devices(EventQueue &queue);
    ~Devices();

    // Starts watching; `on_changed` gets the drive list now and on every
    // plug, unplug, mount or unmount.
    void watch(std::function<void(std::vector<Place>)> on_changed);
    std::vector<Place> snapshot() const; // the last list delivered

    using Done = std::function<void(bool ok, const std::string &path, const std::string &error)>;
    void mount(const std::string &volume_id, Done done);    // Place::id of an unmounted volume
    void unmount(const std::string &path, Done done);       // a mounted drive's path
    void eject(const std::string &path, Done done);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace flo

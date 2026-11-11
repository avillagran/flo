// The sidebar's places, as Folio lists them: Home (every visible folder in
// home, XDG folders and Dropbox with their own glyphs, `omarchy` shown as
// "Omarchy"), Pinned (GTK bookmarks), Views (Recents, Trash) and Drives
// (removable and extra internal disks, not the system's own), plus
// mount / unmount / eject through GIO.
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
                        // templates public dropbox folder recent trash drive
                        // removable network
    std::string status; // Dropbox: synced / syncing / stopped
    std::string id;     // volumes: identifier for Devices::mount()
    bool removable = false;
    bool mounted = true;
    bool can_unmount = false, can_eject = false;
};

namespace places {

// Home's visible folders, natural order. `dropbox_status` labels ~/Dropbox.
std::vector<Place> home_places(const std::string &home, const std::string &dropbox_status = {});
// Recents and Trash.
std::vector<Place> views();
// Folio's display name for a folder ("omarchy" -> "Omarchy").
std::string display_name(const std::string &folder_name);

// GTK bookmarks: "URI[ label]" lines in $XDG_CONFIG_HOME/gtk-3.0/bookmarks.
struct Bookmark {
    std::string uri, label;
    std::string path; // for file:// bookmarks
};
std::string bookmarks_path();
std::vector<Bookmark> parse_bookmarks(const std::string &text);
std::vector<Bookmark> read_bookmarks(const std::string &file = bookmarks_path());
// Pinned folders: local, existing, not already under Home, no duplicates.
std::vector<Place> pinned(const std::vector<Bookmark> &bookmarks, const std::string &home);
// Remote bookmarks (sftp://, smb://, ...), kept for GTK and listed.
std::vector<Place> remote_bookmarks(const std::vector<Bookmark> &bookmarks);
bool is_pinned(const std::string &path, const std::string &file = bookmarks_path());
// Adds a folder (written atomically; other lines, remote ones included, are
// kept as they were). Returns "" or a sentence saying why not.
std::string pin(const std::string &path, const std::string &home, const std::string &file = bookmarks_path());
bool unpin(const std::string &path, const std::string &file = bookmarks_path());

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

// Flo as the desktop's file manager and file chooser, changed only when the
// user asks from Preferences, and always reversibly.
//
// File manager: the inode/directory entry of $XDG_CONFIG_HOME/mimeapps.list,
// written as Files' apps.rs set_default_in() writes it (the
// [Default Applications] line, and the id moved to the front of the
// [Added Associations] line); nothing else in the file changes.
//
// File chooser: the org.freedesktop.impl.portal.FileChooser line in the
// [preferred] group of the user's xdg-desktop-portal configuration, the
// file xdg-desktop-portal actually reads (see portal_config_path()). Other
// lines stay as they were. xdg-desktop-portal finds Flo's backend through
// flo.portal and starts it through a D-Bus service file; when neither is
// installed system-wide, user-level copies go to $XDG_DATA_HOME
// (install_portal_backend()).
//
// Every change returns what it replaced (a Previous), which restore puts
// back exactly: the old value, or no line at all when there was none.
#pragma once

#include <string>
#include <vector>

namespace flo::default_apps {

constexpr const char *kFloDesktopId = "flo.desktop";
constexpr const char *kDirectoryMime = "inode/directory";
constexpr const char *kPortalName = "flo"; // flo.portal
constexpr const char *kPortalBusName = "org.freedesktop.impl.portal.desktop.flo";
constexpr const char *kFileChooserKey = "org.freedesktop.impl.portal.FileChooser";

// INI-like text as freedesktop config files use it (lines kept verbatim).
// The value of `key` in `[group]`; `found` tells a missing key from "".
std::string ini_get(const std::string &text, const std::string &group, const std::string &key, bool *found = nullptr);
// Sets `key` in `[group]` (the group is added at the end when missing),
// changing only that line.
std::string ini_set(const std::string &text, const std::string &group, const std::string &key,
                    const std::string &value);
// Removes `key` from `[group]` (the first such line only).
std::string ini_remove(const std::string &text, const std::string &group, const std::string &key);

// What a change replaced, to put it back.
struct Previous {
    bool had = false;   // the line existed
    std::string value;  // its value then
    bool had2 = false;  // a second line (mimeapps: [Added Associations])
    std::string value2;
    bool valid = false; // something was recorded
    // "1;value;1;value2" with the values percent-escaped (for the settings).
    std::string encode() const;
    static Previous decode(const std::string &text);
};

// --- File manager --------------------------------------------------------

std::string mimeapps_path(); // $XDG_CONFIG_HOME/mimeapps.list
// The first id of the [Default Applications] line for `mime` in `file`
// ("" when the user file sets none; the system may still have one).
std::string user_default(const std::string &file, const std::string &mime);
// Files' set_default_in(). `before` gets what was there. Returns "" or the
// reason (English source string).
std::string set_default(const std::string &file, const std::string &mime, const std::string &id,
                        Previous *before = nullptr);
// Puts back what set_default() replaced.
std::string restore_default(const std::string &file, const std::string &mime, const Previous &before);

// --- File chooser ----------------------------------------------------------

// The desktops of $XDG_CURRENT_DESKTOP, lowercased ("hyprland").
std::vector<std::string> current_desktops();
// Where xdg-desktop-portal 1.22 looks (checked with `xdg-desktop-portal -v`):
// "<desktop>-portals.conf" for each current desktop, then "portals.conf",
// in $XDG_CONFIG_HOME/xdg-desktop-portal, $XDG_CONFIG_DIRS, /etc, then
// $XDG_DATA_HOME and $XDG_DATA_DIRS (…/xdg-desktop-portal). An interface
// line in the user's portals.conf wins over a system desktop file, but a
// user desktop file, when there is one, hides the user's portals.conf.
std::vector<std::string> portal_config_dirs();
// The first existing "<desktop>-portals.conf" across the directories,
// else the first "portals.conf"; "" when none.
std::string effective_portal_config(const std::vector<std::string> &desktops);
// The user file Flo edits: the user's "<desktop>-portals.conf" when there
// is one, else the user's portals.conf (new ones start as a copy of the
// effective system file, `seed_from`, so nothing else changes).
std::string portal_config_path(const std::vector<std::string> &desktops, std::string *seed_from = nullptr);
// The FileChooser backend in use: the explicit line (user desktop file,
// user portals.conf, then the system files), else the first backend of
// the [preferred] default list that implements FileChooser ("gtk").
std::string file_chooser_backend(const std::vector<std::string> &desktops);
// Writes FileChooser=flo; `before` gets what was there.
std::string set_file_chooser(const std::vector<std::string> &desktops, const std::string &backend,
                             Previous *before = nullptr);
std::string restore_file_chooser(const std::vector<std::string> &desktops, const Previous &before);

// The text of flo.portal and of the D-Bus service file that starts `exe`.
std::string portal_file_text();
std::string dbus_service_text(const std::string &exe);
// Whether a flo.portal / Flo backend service file is installed in any data
// directory (user first), and where.
std::string installed_portal_file();
std::string installed_dbus_service();
// Writes the user-level flo.portal and D-Bus service (for `exe`) under
// $XDG_DATA_HOME when no system-wide copy exists. Returns "" or the reason.
std::string install_portal_backend(const std::string &exe);

} // namespace flo::default_apps

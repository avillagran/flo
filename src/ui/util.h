// Small helpers shared by the window and the preview: how entries are named,
// drawn and described, typed paths, and starting other programs.
#pragma once

#include "core/entry.h"

#include "fltk-omarchy/theme.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flo::ui {

// Runs `fn` on the FLTK loop right after the current event (for actions
// that delete the widget whose callback asked for them).
void defer(std::function<void()> fn);

// The folder a pane shows, as names relative to `base` (search results) or
// absolute paths (Recents).
struct FolderModel {
    std::string base; // folder, or "recent://"
    std::vector<Entry> entries;
    bool loading = false;
    std::string error;
    // Folder tiles' picture peeks, by the folder's absolute path (filled
    // with the item counts; names inside are relative to that folder).
    std::unordered_map<std::string, std::vector<Entry>> peeks;

    std::string path(int i) const;
    int index_of_name(const std::string &name) const; // the entry's own name
    int index_of_path(const std::string &path) const;
    int count() const { return int(entries.size()); }
};

constexpr const char *kRecents = "recent://";
// Files' Starred list (Ctrl+Alt+E): the starred items, wherever they are.
constexpr const char *kStarred = "starred://";
// Recents and Starred are lists, not folders.
inline bool is_virtual_folder(const std::string &f) { return f == kRecents || f == kStarred; }

// The last path component of an entry's name (search results and Recents
// carry longer names).
std::string display_name(const Entry &e);
// Icon (fltk-omarchy names) and color for an entry's kind.
std::string kind_icon(const Entry &e);
fo::Rgb kind_color(const Entry &e);
// "28.0 KB" for files, "3 items" for folders (nothing while uncounted).
std::string size_text(const Entry &e);
// "2:14 PM" today, "11/9/26" before.
std::string when_text(int64_t mtime_ns);
// The sidebar icon for a Place::icon.
std::string place_icon(const std::string &icon);
// "/home/me/Pictures" -> "~/Pictures".
std::string tilde(const std::string &path);
// Folio's typed paths: "~", "~/x", absolute, or relative to `cwd`.
std::string expand_typed(const std::string &text, const std::string &cwd);
// The system's name for a MIME type ("JPEG image").
std::string type_description(const std::string &mime);
// Whether a path is inside a trash's files/ folder (or is one).
bool in_trash_folder(const std::string &path);

// Starts a program detached from Flo (its own session, reaped at once).
bool spawn_detached(const std::vector<std::string> &argv, const std::string &cwd = {});
// Opens a file or folder in its default application.
void open_default(const std::string &path);
// A terminal in `folder` (xdg-terminal-exec, $TERMINAL, then common ones).
bool open_terminal(const std::string &folder);

struct AppChoice {
    std::string id, name;
    bool is_default = false;
};
// Applications that can open a type, the default first.
std::vector<AppChoice> apps_for(const std::string &mime);
bool launch_app(const std::string &id, const std::vector<std::string> &paths);
// Makes `id` the default application for `mime`, as the user asked from
// Open With: `xdg-mime default` (GIO's own call when xdg-utils is
// missing). Blocking (a child process): call it off the UI thread.
// Returns an error sentence, or "" on success.
std::string set_default_app(const std::string &mime, const std::string &id);

// "file:///a\r\nfile:///b\r\n" for drags.
std::string uri_list_for_drag(const std::vector<std::string> &paths);

} // namespace flo::ui

// Saved per-folder views, as Files keeps them (folder_views.rs): a folder's
// view, sort and Grid zoom are saved only when asked ("Save this folder's
// view") and applied every time the folder is opened again; "Forget this
// folder's view" drops them. Folders without a saved view keep whatever the
// pane shows. Hidden files stay a global setting.
//
// $XDG_CONFIG_HOME/flo/folder-views, a text file written atomically:
//
//   # Flo folder views, format 1
//   grid name 115 file:///home/me/Pictures
//
// one line per folder (view, sort, zoom percent, file:// URI), at most 256
// folders; an unreadable or foreign file is never overwritten.
#pragma once

#include "core/entry.h"
#include "core/settings.h"

#include <string>
#include <vector>

namespace flo {

struct FolderView {
    std::string path; // absolute, as typed (no canonicalisation)
    ViewMode view = ViewMode::Groups;
    SortMode sort = SortMode::Name;
    int zoom_percent = 100; // Files: 65..180
};

class FolderViews {
public:
    static constexpr std::size_t kMaxFolders = 256, kMaxPath = 4096, kMaxBytes = 256 * 1024;

    static std::string path(); // $XDG_CONFIG_HOME/flo/folder-views

    // Reads `file`. A missing file is an empty, writable catalog; a file
    // that can't be parsed is kept read-only (save() refuses) and error()
    // says why.
    bool load(const std::string &file = path());
    bool save(const std::string &file = path()) const;

    const FolderView *get(const std::string &folder) const;
    // Adds or replaces; false (with error()) when the catalog is full, the
    // path is not absolute or too long, or the catalog is read-only.
    bool set(const FolderView &v);
    bool forget(const std::string &folder);

    const std::vector<FolderView> &all() const { return views_; }
    bool writable() const { return writable_; }
    const std::string &error() const { return error_; }

    static std::string serialize(const std::vector<FolderView> &views);
    // False for a file that is not Flo's format 1 (or breaks its limits).
    static bool parse(const std::string &text, std::vector<FolderView> &out, std::string &why);

private:
    std::vector<FolderView> views_; // sorted by path
    bool writable_ = true;
    std::string error_;
};

} // namespace flo

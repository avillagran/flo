// Folder entries, their kinds and Folio's sort orders.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

namespace flo {

// Folio's coarse kinds (FolderModel::kindForMime), used for icons and the
// "kind" sort.
enum class Kind : uint8_t { Folder, Pdf, Image, Video, Audio, Text, Archive, Other };
const char *kind_name(Kind k); // "folder", "pdf", "image", ...
Kind kind_for_mime(const char *mime);

// Folio's sort modes. "modified" is Folio's saved name for newest first.
// Files' sort criteria and directions: clicking the active column flips it
// (Name A→Z / Z→A, Kind, Newest / Oldest, Size biggest / smallest first).
enum class SortMode : uint8_t { Name, Kind, Newest, Oldest, Size, NameDesc, KindDesc, SizeAsc };
// "name", "kind", "modified", "oldest", "size", "name-desc", "kind-desc", "size-asc"
const char *sort_mode_name(SortMode m);
bool parse_sort_mode(const std::string &s, SortMode &out); // also accepts "newest"

struct Entry {
    std::string name;        // file name; for search results the path relative to the root
    std::string link_target; // readlink() of a symlink, else empty
    const char *mime = "";   // interned (g_intern_string), never freed
    int64_t size = 0;        // bytes (0 for folders)
    int64_t mtime_ns = 0;    // modification time, ns since the epoch
    uint32_t mode = 0;       // st_mode of the entry itself (lstat)
    int32_t item_count = -1; // folders: entries inside, -1 = not counted yet
    Kind kind = Kind::Other;
    bool is_dir = false;      // a folder, following symlinks
    bool is_link = false;
    bool broken_link = false; // a symlink whose target is missing
    bool hidden = false;      // name starts with '.'

    // The extension Folio sorts "kind" by, upper case ("TAR.GZ", "PNG").
    std::string extension() const;
};

struct EntryOptions {
    // Sniff the first bytes when the name doesn't tell the type (files
    // without a known extension). Off for remote filesystems.
    bool sniff = true;
};

// Fills `e` for `name` inside the directory open as `dirfd` (lstat, then stat
// for symlinks). Returns false when the entry vanished.
bool stat_entry(int dirfd, const std::string &name, Entry &e, const EntryOptions &opt);
// The same for an absolute path.
bool stat_path(const std::string &path, Entry &e, const EntryOptions &opt);

// Content type by name, and by content when uncertain and allowed. Interned.
const char *mime_for_name(const std::string &name, bool *uncertain = nullptr);
const char *mime_for_file(int dirfd, const std::string &name, const struct stat &st, bool sniff);

// Folders first, then by `mode`, ties by natural name order (stable).
void sort_entries(std::vector<Entry> &entries, SortMode mode);
bool entry_less(const Entry &a, const Entry &b, SortMode mode);

} // namespace flo

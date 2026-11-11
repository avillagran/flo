// The freedesktop Trash (Trash Specification 1.0): the home trash at
// $XDG_DATA_HOME/Trash and each volume's $topdir/.Trash/$uid or
// $topdir/.Trash-$uid. Synchronous; FileOps runs these on its worker.
//
// Safety rules: trashing never copies across filesystems (an item that has
// no trash on its own volume is refused and left alone); renames never
// replace anything; permanent deletion only acts on items directly inside a
// trash's files/ folder, only with an explicit confirmation flag, never
// follows symlinks and never leaves the item's filesystem.
#pragma once

#include "core/fsutil.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flo::trash {

// Where one trashed object went; enough to restore it exactly.
struct Receipt {
    std::string trash_dir;     // ".../Trash" or "$topdir/.Trash-1000"
    std::string name;          // its name inside files/
    std::string original_path; // absolute
    FileId id;                 // identity of the object (lstat)
    std::string files_path() const { return trash_dir + "/files/" + name; }
    std::string info_path() const { return trash_dir + "/info/" + name + ".trashinfo"; }
};

struct Item {
    std::string trash_dir;
    std::string name;          // inside files/
    std::string original_path; // absolute ("" when the note is unreadable)
    std::string deletion_date; // as written, local time "YYYY-MM-DDThh:mm:ss"
    int64_t deleted_at = 0;    // seconds since the epoch (local time interpreted), 0 unknown
    bool is_dir = false;
    bool is_link = false;
    int64_t size = 0;
    std::string files_path() const { return trash_dir + "/files/" + name; }
    std::string info_path() const { return trash_dir + "/info/" + name + ".trashinfo"; }
};

std::string home_trash();              // $XDG_DATA_HOME/Trash
std::vector<std::string> trash_dirs(); // home + existing volume trashes, deduplicated
bool in_trash(const std::string &path); // inside some trash's files/ (or is it)

// Moves `path` (not following a final symlink) into the trash of its volume.
// With `expected`, refuses unless the object at `path` is that identity.
bool put(const std::string &path, Receipt &receipt, std::string &error, const FileId *expected = nullptr);

// Every item of every trash, newest first.
std::vector<Item> list();
// Items of one trash directory (fixtures and tests).
std::vector<Item> list_dir(const std::string &trash_dir);

// Parses .trashinfo text. `topdir` resolves a relative Path= (volume trash).
bool parse_trashinfo(const std::string &text, const std::string &topdir, std::string &original_path,
                     std::string &deletion_date);
std::string format_trashinfo(const std::string &path_value, const std::string &deletion_date);
// The volume top directory a trash dir belongs to ("" for the home trash).
std::string topdir_of(const std::string &trash_dir);

enum class OnConflict { KeepBoth, Fail };
// Restores a trashed item to its original path (recreating missing parent
// folders). KeepBoth picks "name (copy).ext" when the name is taken (as
// Folio does); Fail leaves everything as it is with errno-like EEXIST in
// `error`. With `expected`, refuses unless the trashed object is that one.
bool restore(const std::string &trash_dir, const std::string &name, OnConflict policy, std::string &restored_to,
             std::string &error, const FileId *expected = nullptr);
bool restore(const Item &item, OnConflict policy, std::string &restored_to, std::string &error);
// Finds the trash item for a path in a trash's files/ folder.
bool item_for_trashed_path(const std::string &files_path, Item &item);

// Permanently deletes one item directly inside a trash's files/ folder,
// and its note. Refuses anything else, and anything without confirmed.
bool delete_permanently(const std::string &files_path, bool confirmed, std::string &error);
// Deletes every item of every trash. Returns how many went.
int empty_all(bool confirmed, std::string &error);

} // namespace flo::trash

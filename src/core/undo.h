// Undo and redo of file operations, with the Rust predecessor's rule:
// nothing is ever deleted by inference. Undo never unlinks or removes a
// tree; every object it touches was recorded with its identity (device,
// inode) by the operation that created, moved or trashed it, and is checked
// again right before it is acted on. Undoing a creation (copy, new folder,
// compress, extract) moves exactly that object to the trash; undoing a move
// renames it back without replacing anything; undoing a trash restores it.
//
// Synchronous; FileOps keeps one history on its worker thread.
#pragma once

#include "core/fsutil.h"
#include "core/trash.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace flo {

// An object an operation created, with what it looked like when it was done.
struct CreatedRecord {
    std::string path;
    FileId id;
    int64_t size = 0;     // files: size and mtime must still match
    int64_t mtime_ns = 0;
    bool is_dir = false;
    bool must_be_empty = false; // new folders: anything put in them keeps them
};

// An object an operation renamed or moved within one filesystem.
struct MovedRecord {
    std::string from, to;
    FileId id;
};

// An object put back from the trash by an undo, so redo can trash it again.
struct RestoredRecord {
    std::string path;
    FileId id;
};

struct UndoEntry {
    std::string label;                  // "Copy 3 items", "Rename “a” to “b”"
    std::vector<MovedRecord> moved;     // undo renames each back (to -> from)
    std::vector<CreatedRecord> created; // undo trashes each (after verification)
    std::vector<trash::Receipt> trashed; // undo restores each (exact name)
    int64_t fence_ns = 0; // changes after this time mean "modified since"

    // Filled while undone, used by redo.
    std::vector<trash::Receipt> created_in_trash; // created objects undo trashed
    std::vector<RestoredRecord> restored;         // trashed objects undo restored
    int64_t undo_fence_ns = 0;

    bool empty() const { return moved.empty() && created.empty() && trashed.empty(); }
};

// Reads the fine wall clock, then waits until the coarse clock that stamps
// inode changes has passed it, so any later change gets a newer ctime.
int64_t take_fence();

// Why a created object must be kept: "" when it is still exactly what the
// operation made (same identity at the same path; files unchanged; folders
// with nothing changed inside since `fence_ns`, within a 20,000-entry budget,
// never following links or leaving the filesystem).
std::string verify_created(const CreatedRecord &c, int64_t fence_ns);
// Fills a record for an object just created at `path` (lstat).
bool record_created(const std::string &path, CreatedRecord &out, bool must_be_empty = false);

struct UndoReport {
    std::size_t done = 0;
    std::vector<std::string> kept; // one sentence per object left alone
    std::string label;
};

class UndoHistory {
public:
    explicit UndoHistory(std::size_t max_entries = 50) : max_(max_entries) {}

    // Records a finished operation; clears the redo stack. Entries with no
    // objects, or more than 1000, are not recorded (the latter returns false).
    bool push(UndoEntry entry);

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::string undo_label() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string redo_label() const { return redo_.empty() ? std::string() : redo_.back().label; }

    // Undoes / redoes the newest entry. What could be done moves to the other
    // stack; objects that changed meanwhile are kept and listed.
    UndoReport undo();
    UndoReport redo();

    void clear() {
        undo_.clear();
        redo_.clear();
    }

private:
    std::size_t max_;
    std::deque<UndoEntry> undo_, redo_;
};

} // namespace flo

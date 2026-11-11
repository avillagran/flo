// File operations with Folio's semantics and the Rust predecessor's safety
// rules, run one at a time on a worker thread:
//
//  - nothing is ever overwritten: names that are taken get "name (copy).ext",
//    "name (copy 2).ext" (Folio's freeName), or the conflict is asked about;
//    "Replace" moves the existing item to the trash first (undoable);
//  - deleting means the trash; permanent deletion only acts inside a trash,
//    only with an explicit confirmation;
//  - a move across filesystems copies, then sends each original to its own
//    volume's trash (never unlinks it);
//  - files are published with rename-without-replace from a hidden partial
//    file, so a half-copied file never sits under its final name;
//  - every operation but restore / delete permanently / empty trash can be
//    undone and redone (see undo.h).
//
// All callbacks run on the UI thread (inside EventQueue::dispatch()).
#pragma once

#include "core/event_queue.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo {

enum class OpKind {
    Copy, Move, Rename, Duplicate, NewFolder, Compress, Extract,
    Trash, Restore, DeletePermanently, EmptyTrash, Undo, Redo
};
const char *op_kind_name(OpKind k);

// What to do when a name is taken in the destination.
enum class ConflictPolicy {
    KeepBoth, // Folio's default: the next free "(copy)" name
    Skip,
    Replace,  // the existing item goes to the trash first
    Ask,      // FileOpsCallbacks::on_conflict, then resolve_conflict()
};
enum class ConflictChoice { KeepBoth, Skip, Replace, Cancel };

struct OpConflict {
    uint64_t op = 0;
    std::string source;      // what is being copied or moved
    std::string destination; // the taken path
    bool source_is_dir = false, destination_is_dir = false;
};

struct OpProgress {
    uint64_t op = 0;
    OpKind kind = OpKind::Copy;
    uint64_t bytes_done = 0, bytes_total = 0; // total 0 = unknown
    std::size_t items_done = 0, items_total = 0;
    std::string current; // name being worked on
};

struct OpResult {
    uint64_t op = 0;
    OpKind kind = OpKind::Copy;
    bool ok = false;
    bool cancelled = false;
    std::string folder;               // where results landed (select them there)
    std::vector<std::string> created; // absolute paths of what was made, moved or restored
    std::vector<std::string> errors;  // one sentence each
    std::string message;              // a short summary for the window
};

struct FileOpsCallbacks {
    std::function<void(const OpProgress &)> on_progress;       // at most ~10 per second
    std::function<void(const OpConflict &)> on_conflict;       // policy Ask only
    std::function<void(const OpResult &)> on_done;
    // After any change to the history: labels like "Copy 3 items" ("" = none).
    std::function<void(const std::string &undo_label, const std::string &redo_label)> on_history;
};

class FileOps {
public:
    FileOps(EventQueue &queue, FileOpsCallbacks callbacks);
    ~FileOps();
    FileOps(const FileOps &) = delete;
    FileOps &operator=(const FileOps &) = delete;

    // Each call queues an operation and returns its id.
    uint64_t copy(std::vector<std::string> paths, std::string folder, ConflictPolicy policy = ConflictPolicy::KeepBoth);
    uint64_t move(std::vector<std::string> paths, std::string folder, ConflictPolicy policy = ConflictPolicy::KeepBoth);
    // Clipboard paste: cut moves, copy copies. Refused into a trash.
    uint64_t paste(std::vector<std::string> paths, std::string folder, bool cut,
                   ConflictPolicy policy = ConflictPolicy::KeepBoth);
    // Drag and drop (Folio): moves on the same drive, copies across drives.
    uint64_t drop(std::vector<std::string> paths, std::string folder, ConflictPolicy policy = ConflictPolicy::KeepBoth);
    uint64_t duplicate(std::vector<std::string> paths);
    uint64_t rename(std::string path, std::string new_name);
    // "New Folder", "New Folder 2", ... unless a name is given.
    uint64_t new_folder(std::string folder, std::string name = {});
    // A zip beside the items (one item: "name.zip", several: "Archive.zip").
    uint64_t compress(std::vector<std::string> paths);
    // Extracts beside the archive: a single top-level item lands as itself,
    // several land in a folder named after the archive.
    uint64_t extract(std::string archive);
    uint64_t trash(std::vector<std::string> paths);
    // Restores items of a trash's files/ folder (name taken: "(copy)").
    uint64_t restore(std::vector<std::string> trashed_paths);
    uint64_t delete_permanently(std::vector<std::string> trashed_paths, bool confirmed);
    uint64_t empty_trash(bool confirmed);
    uint64_t undo();
    uint64_t redo();

    // Cancels a queued or running operation (stops between files and within
    // big files; what was finished stays, recorded for undo).
    void cancel(uint64_t op);
    void cancel_all();
    // Answers an on_conflict question; apply_to_all covers the rest of the op.
    void resolve_conflict(uint64_t op, ConflictChoice choice, bool apply_to_all);

    bool busy() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// Folio's outermost(): drops paths inside another chosen folder (a chosen
// item reached only through a symlinked folder stays, and comes first).
std::vector<std::string> outermost_paths(const std::vector<std::string> &paths);
// The archive name Compress would pick in `folder` for `paths`.
std::string archive_name_for(const std::vector<std::string> &paths, const std::string &folder);
// A rename target that is acceptable: not empty, no '/', not "." or "..".
bool valid_file_name(const std::string &name);

} // namespace flo

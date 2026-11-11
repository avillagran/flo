// Trash, undo/redo and the operation queue, inside private fixtures. The
// home trash is $XDG_DATA_HOME/Trash, which the runner points inside the run.
#include "harness.h"

#include "core/event_queue.h"
#include "core/fileops.h"
#include "core/fsutil.h"
#include "core/trash.h"
#include "core/undo.h"
#include "core/xdg.h"

#include <chrono>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

namespace {

struct Ops {
    EventQueue q;
    std::vector<OpResult> done;
    std::vector<OpConflict> conflicts;
    std::string undo_label, redo_label;
    FileOps ops;
    Ops()
        : ops(q, FileOpsCallbacks{nullptr, [this](const OpConflict &c) { conflicts.push_back(c); },
                                  [this](const OpResult &r) { done.push_back(r); },
                                  [this](const std::string &u, const std::string &r) {
                                      undo_label = u;
                                      redo_label = r;
                                  }}) {}
    OpResult wait(uint64_t id, int ms = 10000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            for (auto &r : done)
                if (r.op == id)
                    return r;
            q.wait(50);
            q.dispatch();
        }
        OpResult none;
        none.errors.push_back("timed out");
        return none;
    }
};

bool exists(const std::string &p) { return exists_nofollow(p); }

} // namespace

TEST(trash_is_private_in_tests) {
    CHECK(path_inside(trash::home_trash(), std::getenv("FLO_TEST_RUN")));
}

TEST(trash_put_list_restore) {
    const std::string d = private_dir("trash");
    write_file(d + "/note.txt", "keep me");
    ::mkdir((d + "/folder").c_str(), 0700);
    write_file(d + "/folder/inside", "x");
    CHECK(::symlink("note.txt", (d + "/link").c_str()) == 0);
    assert_inside(d);
    trash::Receipt r1, r2, r3;
    std::string error;
    CHECK(trash::put(d + "/note.txt", r1, error));
    CHECK(trash::put(d + "/folder", r2, error));
    CHECK(trash::put(d + "/link", r3, error)); // the link, not its target
    CHECK(!exists(d + "/note.txt") && !exists(d + "/folder"));
    CHECK_EQ(r1.trash_dir, trash::home_trash());
    CHECK_EQ(read_file(r1.files_path()), std::string("keep me"));
    struct stat st;
    CHECK(::lstat(r3.files_path().c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    const std::string info = read_file(r1.info_path());
    CHECK(info.find("[Trash Info]\nPath=" + d + "/note.txt\nDeletionDate=") == 0);
    CHECK(::stat(r1.info_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    // Same name again gets a new slot.
    write_file(d + "/note.txt", "second");
    trash::Receipt r4;
    CHECK(trash::put(d + "/note.txt", r4, error));
    CHECK_EQ(r4.name, std::string("note.txt.2"));
    auto items = trash::list_dir(trash::home_trash());
    int mine = 0;
    for (auto &i : items)
        if (path_inside(i.original_path, d)) {
            ++mine;
            CHECK(!i.deletion_date.empty() && i.deleted_at > 0);
        }
    CHECK_EQ(mine, 4);
    CHECK(trash::in_trash(r1.files_path()));
    // Restore: the exact name, then "(copy)" when it's taken.
    std::string to;
    CHECK(trash::restore(r4.trash_dir, r4.name, trash::OnConflict::Fail, to, error));
    CHECK_EQ(to, d + "/note.txt");
    CHECK(!exists(r4.info_path()));
    CHECK(!trash::restore(r1.trash_dir, r1.name, trash::OnConflict::Fail, to, error));
    CHECK(exists(r1.files_path())); // left in the trash, nothing replaced
    CHECK_EQ(read_file(d + "/note.txt"), std::string("second"));
    trash::Item item;
    CHECK(trash::item_for_trashed_path(r1.files_path(), item));
    CHECK(trash::restore(item, trash::OnConflict::KeepBoth, to, error));
    CHECK_EQ(to, d + "/note (copy).txt");
    CHECK_EQ(read_file(to), std::string("keep me"));
    // Restore recreates a missing parent folder.
    CHECK(trash::restore(r2.trash_dir, r2.name, trash::OnConflict::Fail, to, error));
    CHECK_EQ(read_file(d + "/folder/inside"), std::string("x"));
    // Identity checks: a different object at the path is refused.
    FileId wrong{1, 2, 0};
    CHECK(!trash::put(d + "/folder", r2, error, &wrong));
    CHECK(exists(d + "/folder"));
    // Trash folders themselves and their ancestors are refused.
    CHECK(!trash::put(trash::home_trash(), r2, error));
    CHECK(!trash::put(xdg::data_home(), r2, error));
}

TEST(trashinfo_parsing_and_volume_paths) {
    std::string path, date;
    CHECK(trash::parse_trashinfo("[Trash Info]\nPath=/a/b%20c\nDeletionDate=2026-11-11T10:00:00\n", "", path, date));
    CHECK_EQ(path, std::string("/a/b c"));
    CHECK_EQ(date, std::string("2026-11-11T10:00:00"));
    CHECK(trash::parse_trashinfo("[Trash Info]\nPath=photos/x.jpg\n", "/run/media/u/USB", path, date));
    CHECK_EQ(path, std::string("/run/media/u/USB/photos/x.jpg"));
    CHECK(!trash::parse_trashinfo("[Trash Info]\nPath=relative\n", "", path, date)); // home trash: absolute only
    CHECK(!trash::parse_trashinfo("[Other]\nPath=/x\n", "", path, date));
    const std::string uid = std::to_string(getuid());
    CHECK_EQ(trash::topdir_of("/mnt/disk/.Trash-" + uid), std::string("/mnt/disk"));
    CHECK_EQ(trash::topdir_of("/mnt/disk/.Trash/" + uid), std::string("/mnt/disk"));
    CHECK_EQ(trash::topdir_of("/home/u/.local/share/Trash"), std::string());
}

TEST(delete_permanently_only_inside_trash) {
    const std::string d = private_dir("purge");
    write_file(d + "/victim", "x");
    write_file(d + "/outside", "y");
    trash::Receipt r;
    std::string error;
    CHECK(trash::put(d + "/victim", r, error));
    CHECK(!trash::delete_permanently(r.files_path(), false, error)); // needs confirmation
    CHECK(exists(r.files_path()));
    CHECK(!trash::delete_permanently(d + "/outside", true, error)); // not in a trash
    CHECK(exists(d + "/outside"));
    CHECK(!trash::delete_permanently(trash::home_trash() + "/files", true, error));
    CHECK(!trash::delete_permanently(trash::home_trash() + "/info/" + r.name + ".trashinfo", true, error));
    CHECK_EQ(trash::empty_all(false, error), 0);
    assert_inside(r.files_path());
    CHECK(trash::delete_permanently(r.files_path(), true, error));
    CHECK(!exists(r.files_path()) && !exists(r.info_path()));
}

TEST(undo_history_identity_checks) {
    const std::string d = private_dir("undo");
    // A created file, unchanged: undo trashes it; redo restores the same inode.
    write_file(d + "/made.txt", "made");
    UndoHistory h;
    UndoEntry e;
    e.label = "Copy “made.txt”";
    CreatedRecord c;
    CHECK(record_created(d + "/made.txt", c));
    e.created.push_back(c);
    e.fence_ns = take_fence();
    h.push(e);
    CHECK_EQ(h.undo_label(), std::string("Copy “made.txt”"));
    UndoReport r = h.undo();
    CHECK(r.done == 1 && r.kept.empty());
    CHECK(!exists(d + "/made.txt"));
    CHECK(h.can_redo());
    r = h.redo();
    CHECK(r.done == 1 && r.kept.empty());
    FileId now;
    CHECK(lstat_id(d + "/made.txt", now) && now == c.id);
    CHECK(h.can_undo());

    // Modified after the operation: kept.
    UndoHistory h2;
    UndoEntry e2;
    write_file(d + "/edited.txt", "v1");
    CHECK(record_created(d + "/edited.txt", c));
    e2.created.push_back(c);
    e2.fence_ns = take_fence();
    h2.push(e2);
    FILE *f = std::fopen((d + "/edited.txt").c_str(), "a");
    std::fputs(" v2", f);
    std::fclose(f);
    r = h2.undo();
    CHECK(r.done == 0 && r.kept.size() == 1);
    CHECK(exists(d + "/edited.txt"));

    // Replaced by another object with the same name: kept.
    UndoHistory h3;
    UndoEntry e3;
    write_file(d + "/swap.txt", "a");
    CHECK(record_created(d + "/swap.txt", c));
    e3.created.push_back(c);
    e3.fence_ns = take_fence();
    h3.push(e3);
    CHECK(::rename((d + "/swap.txt").c_str(), (d + "/swap-old.txt").c_str()) == 0);
    write_file(d + "/swap.txt", "a");
    r = h3.undo();
    CHECK(r.done == 0 && r.kept.size() == 1);
    CHECK(exists(d + "/swap.txt") && exists(d + "/swap-old.txt"));

    // A new folder with something put in it: kept.
    UndoHistory h4;
    UndoEntry e4;
    ::mkdir((d + "/New Folder").c_str(), 0700);
    CHECK(record_created(d + "/New Folder", c, true));
    e4.created.push_back(c);
    e4.fence_ns = take_fence();
    h4.push(e4);
    write_file(d + "/New Folder/user-file", "mine");
    r = h4.undo();
    CHECK(r.done == 0 && r.kept.size() == 1);
    CHECK_EQ(read_file(d + "/New Folder/user-file"), std::string("mine"));

    // Moves go back without replacing: an occupied original name is kept.
    UndoHistory h5;
    UndoEntry e5;
    write_file(d + "/a", "A");
    FileId id;
    lstat_id(d + "/a", id);
    CHECK(::rename((d + "/a").c_str(), (d + "/b").c_str()) == 0);
    e5.moved.push_back({d + "/a", d + "/b", id});
    h5.push(e5);
    write_file(d + "/a", "occupant");
    r = h5.undo();
    CHECK(r.done == 0 && r.kept.size() == 1);
    CHECK_EQ(read_file(d + "/a"), std::string("occupant"));
    CHECK_EQ(read_file(d + "/b"), std::string("A"));
}

TEST(file_ops_copy_move_rename_undo) {
    const std::string d = private_dir("ops");
    ::mkdir((d + "/src").c_str(), 0700);
    ::mkdir((d + "/dst").c_str(), 0700);
    write_file(d + "/src/a.txt", "alpha");
    ::mkdir((d + "/src/tree").c_str(), 0700);
    ::chmod((d + "/src/tree").c_str(), 0750); // the runner's umask is 077
    write_file(d + "/src/tree/x", "x");
    CHECK(::symlink("x", (d + "/src/tree/lx").c_str()) == 0);
    write_file(d + "/dst/a.txt", "occupant");
    Ops o;
    // Copy never overwrites: "a (copy).txt".
    OpResult r = o.wait(o.ops.copy({d + "/src/a.txt", d + "/src/tree"}, d + "/dst"));
    CHECK(r.ok);
    CHECK_EQ(read_file(d + "/dst/a.txt"), std::string("occupant"));
    CHECK_EQ(read_file(d + "/dst/a (copy).txt"), std::string("alpha"));
    CHECK_EQ(read_file(d + "/dst/tree/x"), std::string("x"));
    struct stat st;
    CHECK(::lstat((d + "/dst/tree/lx").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(::stat((d + "/dst/tree").c_str(), &st) == 0 && (st.st_mode & 0777) == 0750);
    CHECK_EQ(o.undo_label, std::string("Copy 2 items"));
    // Undo of the copy: the copies go to the trash, sources untouched.
    r = o.wait(o.ops.undo());
    CHECK(r.ok);
    CHECK(!exists(d + "/dst/a (copy).txt") && !exists(d + "/dst/tree"));
    CHECK_EQ(read_file(d + "/src/a.txt"), std::string("alpha"));
    CHECK_EQ(read_file(d + "/dst/a.txt"), std::string("occupant"));
    CHECK_EQ(o.redo_label, std::string("Copy 2 items"));
    r = o.wait(o.ops.redo());
    CHECK(r.ok && exists(d + "/dst/tree/x"));

    // Rename and its undo.
    r = o.wait(o.ops.rename(d + "/src/a.txt", "renamed.txt"));
    CHECK(r.ok && exists(d + "/src/renamed.txt") && !exists(d + "/src/a.txt"));
    r = o.wait(o.ops.rename(d + "/src/renamed.txt", "../escape"));
    CHECK(!r.ok && exists(d + "/src/renamed.txt"));
    r = o.wait(o.ops.rename(d + "/src/renamed.txt", "tree"));
    CHECK(!r.ok); // taken: nothing replaced
    r = o.wait(o.ops.undo());
    CHECK(r.ok && exists(d + "/src/a.txt"));

    // Move within a filesystem, then undo.
    r = o.wait(o.ops.move({d + "/src/a.txt"}, d + "/dst"));
    CHECK(r.ok && exists(d + "/dst/a (copy).txt") && !exists(d + "/src/a.txt"));
    r = o.wait(o.ops.undo());
    CHECK(r.ok && exists(d + "/src/a.txt"));
    // Not into itself.
    r = o.wait(o.ops.move({d + "/src"}, d + "/src/tree"));
    CHECK(!r.ok && exists(d + "/src/tree"));
    // Not into a trash.
    r = o.wait(o.ops.copy({d + "/src/a.txt"}, trash::home_trash() + "/files"));
    CHECK(!r.ok);
}

TEST(file_ops_new_folder_duplicate_trash_conflicts) {
    const std::string d = private_dir("ops2");
    Ops o;
    OpResult r = o.wait(o.ops.new_folder(d));
    CHECK(r.ok && r.created.size() == 1 && r.created[0] == d + "/New Folder");
    r = o.wait(o.ops.new_folder(d));
    CHECK(r.ok && r.created[0] == d + "/New Folder 2");
    r = o.wait(o.ops.undo());
    CHECK(r.ok && !exists(d + "/New Folder 2"));
    write_file(d + "/doc.md", "# hi");
    r = o.wait(o.ops.duplicate({d + "/doc.md"}));
    CHECK(r.ok && read_file(d + "/doc (copy).md") == "# hi");
    r = o.wait(o.ops.duplicate({d + "/doc.md"}));
    CHECK(r.ok && exists(d + "/doc (copy 2).md"));

    // Trash and undo restores to the exact place.
    r = o.wait(o.ops.trash({d + "/doc.md"}));
    CHECK(r.ok && !exists(d + "/doc.md"));
    r = o.wait(o.ops.undo());
    CHECK(r.ok && read_file(d + "/doc.md") == "# hi");
    r = o.wait(o.ops.redo());
    CHECK(r.ok && !exists(d + "/doc.md"));
    r = o.wait(o.ops.undo());
    CHECK(r.ok && exists(d + "/doc.md"));

    // Restore through the queue picks "(copy)" when taken.
    trash::Receipt rec;
    std::string error;
    write_file(d + "/t.txt", "old");
    CHECK(trash::put(d + "/t.txt", rec, error));
    write_file(d + "/t.txt", "new");
    r = o.wait(o.ops.restore({rec.files_path()}));
    CHECK(r.ok && r.created.size() == 1 && r.created[0] == d + "/t (copy).txt");

    // Ask: the UI chooses Replace; the occupant goes to the trash (undoable).
    ::mkdir((d + "/in").c_str(), 0700);
    write_file(d + "/in/t.txt", "occupant");
    const uint64_t id = o.ops.copy({d + "/t.txt"}, d + "/in", ConflictPolicy::Ask);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (o.conflicts.empty() && std::chrono::steady_clock::now() < end) {
        o.q.wait(50);
        o.q.dispatch();
    }
    CHECK_EQ(o.conflicts.size(), static_cast<std::size_t>(1));
    if (!o.conflicts.empty()) {
        CHECK_EQ(o.conflicts[0].destination, d + "/in/t.txt");
        o.ops.resolve_conflict(id, ConflictChoice::Replace, false);
    }
    r = o.wait(id);
    CHECK(r.ok && read_file(d + "/in/t.txt") == "new");
    r = o.wait(o.ops.undo());
    CHECK(r.ok && read_file(d + "/in/t.txt") == "occupant");
    // Skip policy leaves both alone.
    r = o.wait(o.ops.copy({d + "/t.txt"}, d + "/in", ConflictPolicy::Skip));
    CHECK(r.ok && read_file(d + "/in/t.txt") == "occupant" && !exists(d + "/in/t (copy).txt"));
}

TEST(file_ops_compress_and_extract) {
    const std::string d = private_dir("archive");
    ::mkdir((d + "/proj").c_str(), 0700);
    write_file(d + "/proj/a.txt", "A");
    write_file(d + "/b.txt", "B");
    Ops o;
    OpResult r = o.wait(o.ops.compress({d + "/proj"}));
    CHECK(r.ok && exists(d + "/proj.zip"));
    r = o.wait(o.ops.compress({d + "/proj", d + "/b.txt"}));
    CHECK(r.ok && exists(d + "/Archive.zip"));
    // A single top-level folder lands as itself (free name since proj exists).
    r = o.wait(o.ops.extract(d + "/proj.zip"));
    CHECK(r.ok && read_file(d + "/proj (copy)/a.txt") == "A");
    // Several top-level items land in a folder named after the archive.
    r = o.wait(o.ops.extract(d + "/Archive.zip"));
    CHECK(r.ok && read_file(d + "/Archive/b.txt") == "B" && read_file(d + "/Archive/proj/a.txt") == "A");
    // Undo of extract trashes what was extracted.
    r = o.wait(o.ops.undo());
    CHECK(r.ok && !exists(d + "/Archive"));
    // No hidden temporary folders are left behind.
    bool leftovers = false;
    for (auto &e : list_folder_names(d))
        if (e.compare(0, 5, ".flo-") == 0)
            leftovers = true;
    CHECK(!leftovers);
    CHECK_EQ(archive_name_for({d + "/b.txt"}, d), std::string("b.txt.zip"));
}

TEST(outermost_paths_drop_nested) {
    const std::string d = private_dir("outer");
    ::mkdir((d + "/a").c_str(), 0700);
    write_file(d + "/a/x", "");
    auto o = outermost_paths({d + "/a/x", d + "/a", d + "/a"});
    CHECK(o.size() == 1 && o[0] == d + "/a");
    CHECK(valid_file_name("ok.txt") && !valid_file_name("a/b") && !valid_file_name("..") && !valid_file_name(""));
}

TEST(file_ops_pause_resume_and_clear_pending) {
    const std::string d = private_dir("pause");
    const std::string src = d + "/src", dst = d + "/dst";
    CHECK(::mkdir(src.c_str(), 0700) == 0 && ::mkdir(dst.c_str(), 0700) == 0);
    write_file(src + "/a", "first");
    write_file(src + "/b", "second");
    write_file(src + "/c", "third");
    write_file(dst + "/a", "already here");
    assert_inside(d);
    Ops o;
    // "a" is taken: the worker waits for the answer, so A is the running op.
    uint64_t a = o.ops.copy({src + "/a", src + "/b"}, dst, ConflictPolicy::Ask);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (o.conflicts.empty() && std::chrono::steady_clock::now() < end) {
        o.q.wait(50);
        o.q.dispatch();
    }
    CHECK_EQ(o.conflicts.size(), size_t(1));
    CHECK(o.ops.pause(a));
    CHECK(o.ops.paused(a));
    uint64_t b = o.ops.copy({src + "/c"}, dst);
    uint64_t c = o.ops.move({src + "/c"}, dst);
    CHECK(!o.ops.pause(b)); // only the running operation pauses
    CHECK_EQ(o.ops.pending(), size_t(2));
    CHECK_EQ(o.ops.clear_pending(), size_t(2));
    CHECK_EQ(o.ops.pending(), size_t(0));
    o.ops.resolve_conflict(a, ConflictChoice::KeepBoth, false);
    // Paused at the next check: "b" does not arrive while paused.
    const auto hold = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < hold) {
        o.q.wait(50);
        o.q.dispatch();
    }
    CHECK(o.done.empty());
    CHECK(!exists(dst + "/b"));
    CHECK(o.ops.resume(a));
    OpResult ra = o.wait(a);
    CHECK(ra.ok);
    CHECK_EQ(read_file(dst + "/b"), std::string("second"));
    CHECK_EQ(read_file(dst + "/a"), std::string("already here"));
    OpResult rb = o.wait(b), rc = o.wait(c);
    CHECK(rb.cancelled && rc.cancelled);
    CHECK(!exists(dst + "/c"));
    CHECK_EQ(read_file(src + "/c"), std::string("third"));
    CHECK(!o.ops.pause(a)); // finished
}

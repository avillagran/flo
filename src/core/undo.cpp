#include "core/undo.h"

#include <cerrno>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo {

static int64_t clock_ns(clockid_t id) {
    timespec ts;
    ::clock_gettime(id, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int64_t take_fence() {
    const int64_t t = clock_ns(CLOCK_REALTIME);
    for (int i = 0; i < 200 && clock_ns(CLOCK_REALTIME_COARSE) <= t; ++i)
        ::usleep(1000);
    return t;
}

bool record_created(const std::string &path, CreatedRecord &out, bool must_be_empty) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0)
        return false;
    out.path = path;
    out.id = FileId{st.st_dev, st.st_ino, st.st_mode};
    out.size = static_cast<int64_t>(st.st_size);
    out.mtime_ns = timespec_ns(st.st_mtim);
    out.is_dir = S_ISDIR(st.st_mode);
    out.must_be_empty = must_be_empty;
    return true;
}

namespace {

constexpr std::size_t kScanBudget = 20000;
constexpr int kMaxDepth = 128;

// Every entry under the folder open as `fd`: unchanged since the fence, on
// the same device, within budget. Returns "" or why not.
std::string scan_tree(int fd, dev_t dev, int64_t fence, std::size_t &budget, int depth) {
    if (depth > kMaxDepth)
        return "too deep to verify";
    DIR *d = ::fdopendir(fd);
    if (!d) {
        ::close(fd);
        return "couldn't be read to verify";
    }
    std::string why;
    while (dirent *e = ::readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        if (budget-- == 0) {
            why = "too big to verify";
            break;
        }
        struct stat st;
        if (::fstatat(::dirfd(d), n, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            why = "changed while being verified";
            break;
        }
        if (st.st_dev != dev) {
            why = "holds another filesystem";
            break;
        }
        if (timespec_ns(st.st_ctim) > fence) {
            why = "modified afterwards";
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            const int child = ::openat(::dirfd(d), n, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                why = "couldn't be read to verify";
                break;
            }
            why = scan_tree(child, dev, fence, budget, depth + 1);
            if (!why.empty())
                break;
        }
    }
    ::closedir(d);
    return why;
}

} // namespace

std::string verify_created(const CreatedRecord &c, int64_t fence_ns) {
    struct stat st;
    if (::lstat(c.path.c_str(), &st) != 0)
        return "is no longer there";
    if (st.st_dev != c.id.dev || st.st_ino != c.id.ino)
        return "was replaced by another item";
    if (timespec_ns(st.st_ctim) > fence_ns)
        return "was modified afterwards";
    if (!S_ISDIR(st.st_mode)) {
        if (static_cast<int64_t>(st.st_size) != c.size || timespec_ns(st.st_mtim) != c.mtime_ns)
            return "was modified afterwards";
        return {};
    }
    const int fd = ::open(c.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return "couldn't be read to verify";
    struct stat opened;
    if (::fstat(fd, &opened) != 0 || opened.st_ino != st.st_ino || opened.st_dev != st.st_dev) {
        ::close(fd);
        return "was replaced by another item";
    }
    if (c.must_be_empty) {
        DIR *d = ::fdopendir(fd);
        if (!d) {
            ::close(fd);
            return "couldn't be read to verify";
        }
        bool empty = true;
        while (dirent *e = ::readdir(d)) {
            const char *n = e->d_name;
            if (!(n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))) {
                empty = false;
                break;
            }
        }
        ::closedir(d);
        return empty ? std::string() : "has new items in it";
    }
    std::size_t budget = kScanBudget;
    return scan_tree(fd, st.st_dev, fence_ns, budget, 0);
}

bool UndoHistory::push(UndoEntry entry) {
    if (entry.empty())
        return true;
    if (entry.moved.size() + entry.created.size() + entry.trashed.size() > 1000)
        return false;
    redo_.clear();
    undo_.push_back(std::move(entry));
    while (undo_.size() > max_)
        undo_.pop_front();
    return true;
}

static std::string name_of(const std::string &path) { return "“" + path_basename(path) + "”"; }

UndoReport UndoHistory::undo() {
    UndoReport report;
    if (undo_.empty())
        return report;
    UndoEntry e = std::move(undo_.back());
    undo_.pop_back();
    report.label = e.label;
    UndoEntry r; // what redo will do
    r.label = e.label;

    // 1. Moves go back, newest first, never replacing anything.
    for (auto it = e.moved.rbegin(); it != e.moved.rend(); ++it) {
        FileId now;
        if (!lstat_id(it->to, now) || now != it->id) {
            report.kept.push_back(name_of(it->to) + " was moved or replaced afterwards; it was left alone.");
            continue;
        }
        if (rename_noreplace(it->to, it->from) != 0) {
            report.kept.push_back(errno == EEXIST ? name_of(it->from) + " is taken again; nothing was replaced."
                                                  : "Couldn't move " + name_of(it->to) + " back: " + std::strerror(errno));
            continue;
        }
        r.moved.push_back(*it);
        ++report.done;
    }
    // 2. Created objects go to the trash, if still exactly what was made.
    for (auto it = e.created.rbegin(); it != e.created.rend(); ++it) {
        const std::string why = verify_created(*it, e.fence_ns);
        if (!why.empty()) {
            report.kept.push_back(name_of(it->path) + " " + why + "; it was kept.");
            continue;
        }
        trash::Receipt receipt;
        std::string error;
        if (!trash::put(it->path, receipt, error, &it->id)) {
            report.kept.push_back(error);
            continue;
        }
        r.created.push_back(*it);
        r.created_in_trash.push_back(receipt);
        ++report.done;
    }
    // 3. Trashed objects come back to their exact place.
    for (auto it = e.trashed.rbegin(); it != e.trashed.rend(); ++it) {
        std::string to, error;
        if (!trash::restore(it->trash_dir, it->name, trash::OnConflict::Fail, to, error, &it->id)) {
            report.kept.push_back(error);
            continue;
        }
        FileId id;
        lstat_id(to, id);
        r.trashed.push_back(*it);
        r.restored.push_back({to, id});
        ++report.done;
    }
    if (!r.empty()) {
        r.fence_ns = e.fence_ns;
        r.undo_fence_ns = take_fence();
        redo_.push_back(std::move(r));
    }
    return report;
}

UndoReport UndoHistory::redo() {
    UndoReport report;
    if (redo_.empty())
        return report;
    UndoEntry e = std::move(redo_.back());
    redo_.pop_back();
    report.label = e.label;
    UndoEntry u; // undoable again
    u.label = e.label;
    bool reoffer = true;

    // 1. What undo restored from the trash goes back there.
    for (std::size_t i = 0; i < e.restored.size(); ++i) {
        trash::Receipt receipt;
        std::string error;
        if (!trash::put(e.restored[i].path, receipt, error, &e.restored[i].id)) {
            report.kept.push_back(error);
            continue;
        }
        u.trashed.push_back(receipt);
        ++report.done;
    }
    // 2. Created objects come back from the trash (the same inodes).
    for (std::size_t i = 0; i < e.created_in_trash.size() && i < e.created.size(); ++i) {
        const trash::Receipt &rc = e.created_in_trash[i];
        CreatedRecord in_trash = e.created[i];
        in_trash.path = rc.files_path();
        // Changed while in the trash: restored anyway (that destroys
        // nothing), but not offered for undo again.
        const bool unchanged = verify_created(in_trash, e.undo_fence_ns).empty();
        std::string to, error;
        if (!trash::restore(rc.trash_dir, rc.name, trash::OnConflict::Fail, to, error, &rc.id)) {
            report.kept.push_back(error);
            continue;
        }
        if (unchanged) {
            CreatedRecord again;
            if (record_created(to, again, e.created[i].must_be_empty))
                u.created.push_back(again);
        } else {
            reoffer = false;
        }
        ++report.done;
    }
    // 3. Moves go forward again.
    for (const auto &m : e.moved) {
        FileId now;
        if (!lstat_id(m.from, now) || now != m.id) {
            report.kept.push_back(name_of(m.from) + " was moved or replaced afterwards; it was left alone.");
            continue;
        }
        if (rename_noreplace(m.from, m.to) != 0) {
            report.kept.push_back(errno == EEXIST ? name_of(m.to) + " is taken; nothing was replaced."
                                                  : "Couldn't move " + name_of(m.from) + ": " + std::strerror(errno));
            continue;
        }
        u.moved.push_back(m);
        ++report.done;
    }
    if (!u.empty() && reoffer) {
        u.fence_ns = take_fence();
        undo_.push_back(std::move(u));
    }
    return report;
}

} // namespace flo

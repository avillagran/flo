#include "core/fileops.h"
#include "core/fsutil.h"
#include "core/process.h"
#include "core/trash.h"
#include "core/undo.h"
#include "core/xdg.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace flo {

const char *op_kind_name(OpKind k) {
    switch (k) {
    case OpKind::Copy: return "copy";
    case OpKind::Move: return "move";
    case OpKind::Rename: return "rename";
    case OpKind::Duplicate: return "duplicate";
    case OpKind::NewFolder: return "new-folder";
    case OpKind::Compress: return "compress";
    case OpKind::Extract: return "extract";
    case OpKind::Trash: return "trash";
    case OpKind::Restore: return "restore";
    case OpKind::DeletePermanently: return "delete-permanently";
    case OpKind::EmptyTrash: return "empty-trash";
    case OpKind::Undo: return "undo";
    case OpKind::Redo: return "redo";
    }
    return "?";
}

bool valid_file_name(const std::string &name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\0') == std::string::npos && name.size() <= 255;
}

std::vector<std::string> outermost_paths(const std::vector<std::string> &paths) {
    std::set<std::string> chosen;
    for (const auto &p : paths)
        chosen.insert(clean_path(absolute_path(p)));
    std::vector<std::string> beyond_links, outer;
    std::set<std::string> seen;
    for (const auto &p : paths) {
        const std::string clean = clean_path(absolute_path(p));
        if (!seen.insert(clean).second)
            continue;
        bool inside = false, beyond_link = false;
        for (std::size_t slash = clean.rfind('/'); slash != std::string::npos && slash > 0 && !inside && !beyond_link;
             slash = clean.rfind('/', slash - 1)) {
            if (!chosen.count(clean.substr(0, slash)))
                continue;
            // A link on the way up is copied as a link, not gone into.
            for (std::size_t step = clean.rfind('/'); step != std::string::npos && step >= slash && !beyond_link;
                 step = step == 0 ? std::string::npos : clean.rfind('/', step - 1)) {
                struct stat st;
                beyond_link = ::lstat(clean.substr(0, step).c_str(), &st) == 0 && S_ISLNK(st.st_mode);
            }
            inside = !beyond_link;
        }
        if (beyond_link)
            beyond_links.push_back(clean);
        else if (!inside)
            outer.push_back(clean);
    }
    beyond_links.insert(beyond_links.end(), outer.begin(), outer.end());
    return beyond_links;
}

std::string archive_name_for(const std::vector<std::string> &paths, const std::string &folder) {
    const std::string base = paths.size() == 1 ? path_basename(paths.front()) : std::string("Archive");
    return free_name(folder, base + ".zip");
}

// ---------------------------------------------------------------------------

namespace {

struct OpState {
    CancelToken cancel;
    std::mutex m;
    std::condition_variable cv;
    // Files' Pause: the worker waits at its next check (between files and
    // between chunks of a big file) until resumed or cancelled.
    std::atomic<bool> paused{false};
    std::mutex pm;
    std::condition_variable pcv;
    bool answered = false;
    ConflictChoice choice = ConflictChoice::KeepBoth;
    bool apply_all = false;
};

void wake(OpState &s) {
    { std::lock_guard<std::mutex> lock(s.pm); }
    s.pcv.notify_all();
}

std::string quoted(const std::string &path) { return "“" + path_basename(path) + "”"; }

std::string items_label(const char *verb, const std::vector<std::string> &paths) {
    if (paths.size() == 1)
        return std::string(verb) + " " + quoted(paths.front());
    return std::string(verb) + " " + std::to_string(paths.size()) + " items";
}

} // namespace

struct FileOps::Impl {
    EventQueue::Poster poster;
    FileOpsCallbacks cb;
    Executor exec;
    std::mutex mutex;
    std::map<uint64_t, std::shared_ptr<OpState>> ops;
    uint64_t next_id = 1;
    std::atomic<int> active{0};
    std::atomic<uint64_t> running{0}; // the operation the worker is on
    UndoHistory history; // only touched on the worker thread
    std::weak_ptr<Impl> self;

    void post_history() {
        const std::string u = history.can_undo() ? history.undo_label() : std::string();
        const std::string r = history.can_redo() ? history.redo_label() : std::string();
        auto cbh = cb.on_history;
        if (cbh)
            poster.post([cbh, u, r] { cbh(u, r); });
    }
};

namespace {

// Everything one running operation needs on the worker thread.
struct Ctx {
    FileOps::Impl *impl;
    uint64_t id;
    OpKind kind;
    std::shared_ptr<OpState> state;
    ConflictPolicy policy;
    OpResult result;
    UndoEntry entry;
    OpProgress progress;
    std::chrono::steady_clock::time_point last_progress{};

    // Waits here while the operation is paused; true once it is cancelled.
    bool cancelled() const {
        if (state->paused.load()) {
            std::unique_lock<std::mutex> lock(state->pm);
            state->pcv.wait(lock, [this] { return !state->paused.load() || state->cancel.cancelled(); });
        }
        return state->cancel.cancelled();
    }

    void report_progress(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_progress < std::chrono::milliseconds(100))
            return;
        last_progress = now;
        auto f = impl->cb.on_progress;
        if (f) {
            OpProgress p = progress;
            impl->poster.post([f, p] { f(p); });
        }
    }

    void error(const std::string &e) { result.errors.push_back(e); }
};

enum class Resolve { Proceed, Skip, Cancel };

// Picks the destination name for `source` in `folder`, asking or trashing
// the occupant as the policy says.
Resolve resolve_name(Ctx &ctx, const std::string &source, const std::string &folder, std::string &name) {
    const std::string dest = path_join(folder, name);
    struct stat dst;
    if (::lstat(dest.c_str(), &dst) != 0)
        return Resolve::Proceed;
    struct stat src{};
    const bool have_src = ::lstat(source.c_str(), &src) == 0;
    const bool same_object = have_src && src.st_dev == dst.st_dev && src.st_ino == dst.st_ino;
    ConflictPolicy policy = ctx.policy;
    if (policy == ConflictPolicy::Ask && !same_object) {
        auto f = ctx.impl->cb.on_conflict;
        if (!f) {
            policy = ConflictPolicy::KeepBoth;
        } else {
            OpConflict c;
            c.op = ctx.id;
            c.source = source;
            c.destination = dest;
            c.source_is_dir = have_src && S_ISDIR(src.st_mode);
            c.destination_is_dir = S_ISDIR(dst.st_mode);
            {
                std::lock_guard<std::mutex> lock(ctx.state->m);
                ctx.state->answered = false;
            }
            ctx.impl->poster.post([f, c] { f(c); });
            std::unique_lock<std::mutex> lock(ctx.state->m);
            while (!ctx.state->answered && !ctx.state->cancel.cancelled())
                ctx.state->cv.wait_for(lock, std::chrono::milliseconds(200));
            if (ctx.state->cancel.cancelled() || ctx.state->choice == ConflictChoice::Cancel) {
                ctx.state->cancel.cancel();
                return Resolve::Cancel;
            }
            switch (ctx.state->choice) {
            case ConflictChoice::Skip: policy = ConflictPolicy::Skip; break;
            case ConflictChoice::Replace: policy = ConflictPolicy::Replace; break;
            default: policy = ConflictPolicy::KeepBoth; break;
            }
            if (ctx.state->apply_all)
                ctx.policy = policy;
        }
    }
    // Replacing an item with itself (a copy into its own folder) keeps both.
    if (policy == ConflictPolicy::Ask || (policy == ConflictPolicy::Replace && same_object))
        policy = ConflictPolicy::KeepBoth;
    switch (policy) {
    case ConflictPolicy::Skip:
        return Resolve::Skip;
    case ConflictPolicy::Replace: {
        trash::Receipt receipt;
        std::string error;
        const FileId id{dst.st_dev, dst.st_ino, dst.st_mode};
        if (!trash::put(dest, receipt, error, &id)) {
            ctx.error("Couldn't replace " + quoted(dest) + ": " + error);
            return Resolve::Skip;
        }
        ctx.entry.trashed.push_back(receipt);
        return Resolve::Proceed;
    }
    default:
        name = free_name(folder, name);
        return Resolve::Proceed;
    }
}

void scan_size(const std::string &path, uint64_t &bytes, std::size_t &items, std::size_t &budget) {
    if (budget == 0)
        return;
    --budget;
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0)
        return;
    ++items;
    if (S_ISREG(st.st_mode))
        bytes += static_cast<uint64_t>(st.st_size);
    if (!S_ISDIR(st.st_mode))
        return;
    DIR *d = ::opendir(path.c_str());
    if (!d)
        return;
    while (dirent *e = ::readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        scan_size(path_join(path, n), bytes, items, budget);
        if (budget == 0)
            break;
    }
    ::closedir(d);
}

std::atomic<unsigned> g_part_counter{0};

// Copies a regular file's data into a hidden partial file in `folder`, then
// publishes it under `name` without replacing anything. Returns the final
// name, or "" (error recorded). On a race for the name it takes the next
// free one.
std::string copy_file(Ctx &ctx, const std::string &src, const struct stat &st, const std::string &folder,
                      const std::string &name) {
    const int in = ::open(src.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
    if (in < 0) {
        ctx.error("Couldn't read " + quoted(src) + ": " + std::strerror(errno));
        return {};
    }
    const std::string part = path_join(folder, ".flo-part-" + std::to_string(::getpid()) + "-" +
                                                   std::to_string(g_part_counter++));
    const int out = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        ctx.error("Couldn't write in " + folder + ": " + std::strerror(errno));
        ::close(in);
        return {};
    }
    bool ok = true;
    bool use_cfr = true;
    std::vector<char> buf;
    for (;;) {
        if (ctx.cancelled()) {
            ok = false;
            break;
        }
        ssize_t n = -1;
        if (use_cfr) {
            n = ::copy_file_range(in, nullptr, out, nullptr, 4u << 20, 0);
            if (n < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP ||
                          errno == EBADF)) {
                use_cfr = false;
                continue;
            }
        } else {
            if (buf.empty())
                buf.resize(1u << 20);
            n = ::read(in, buf.data(), buf.size());
            if (n > 0) {
                ssize_t done = 0;
                while (done < n) {
                    const ssize_t w = ::write(out, buf.data() + done, static_cast<std::size_t>(n - done));
                    if (w < 0 && errno == EINTR)
                        continue;
                    if (w <= 0) {
                        n = -1;
                        break;
                    }
                    done += w;
                }
            }
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ctx.error("Couldn't copy " + quoted(src) + ": " + std::strerror(errno));
            ok = false;
            break;
        }
        if (n == 0)
            break;
        ctx.progress.bytes_done += static_cast<uint64_t>(n);
        ctx.report_progress();
    }
    if (ok) {
        // Folio keeps permissions; the modification time is kept too.
        ::fchmod(out, st.st_mode & 07777);
        const timespec times[2] = {st.st_atim, st.st_mtim};
        ::futimens(out, times);
    }
    if (::close(out) != 0 && ok) {
        ctx.error("Couldn't finish writing " + quoted(src) + ".");
        ok = false;
    }
    ::close(in);
    std::string final_name = name;
    if (ok) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (rename_noreplace(part, path_join(folder, final_name)) == 0)
                return final_name;
            if (errno != EEXIST) {
                ctx.error("Couldn't place " + quoted(src) + ": " + std::strerror(errno));
                break;
            }
            final_name = free_name(folder, name);
        }
    }
    ::unlink(part.c_str()); // only the partial file this call created
    return {};
}

// Copies `src` (files, folders, links as links) to folder/name. Returns the
// created name ("" when nothing was created). `ok` turns false on any error
// or cancel inside.
std::string copy_entry(Ctx &ctx, const std::string &src, const std::string &folder, const std::string &name,
                       bool &ok, int depth = 0) {
    struct stat st;
    if (::lstat(src.c_str(), &st) != 0) {
        ctx.error(quoted(src) + " is no longer there.");
        ok = false;
        return {};
    }
    ctx.progress.current = path_basename(src);
    if (depth > 200) {
        ctx.error(quoted(src) + " is nested too deeply.");
        ok = false;
        return {};
    }
    if (S_ISLNK(st.st_mode)) {
        // Links are copied as links, as written.
        char target[4096];
        const ssize_t n = ::readlink(src.c_str(), target, sizeof target - 1);
        if (n < 0) {
            ctx.error("Couldn't read the link " + quoted(src) + ".");
            ok = false;
            return {};
        }
        target[n] = '\0';
        std::string final_name = name;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::symlink(target, path_join(folder, final_name).c_str()) == 0) {
                ++ctx.progress.items_done;
                return final_name;
            }
            if (errno != EEXIST)
                break;
            final_name = free_name(folder, name);
        }
        ctx.error("Couldn't copy the link " + quoted(src) + ": " + std::strerror(errno));
        ok = false;
        return {};
    }
    if (S_ISREG(st.st_mode)) {
        const std::string made = copy_file(ctx, src, st, folder, name);
        if (made.empty())
            ok = false;
        else
            ++ctx.progress.items_done;
        return made;
    }
    if (!S_ISDIR(st.st_mode)) {
        ctx.error(quoted(src) + " is a special file and wasn't copied.");
        ok = false;
        return {};
    }
    // A folder that can't be read would be copied empty.
    DIR *d = ::opendir(src.c_str());
    if (!d) {
        ctx.error("Couldn't read " + quoted(src) + ": " + std::strerror(errno));
        ok = false;
        return {};
    }
    std::vector<std::string> names;
    while (dirent *e = ::readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        names.emplace_back(n);
    }
    ::closedir(d);
    // Made with its permissions plus the owner's, so a private folder is
    // never open to others even when the copy stops partway.
    std::string final_name = name;
    bool made = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (::mkdir(path_join(folder, final_name).c_str(), (st.st_mode & 07777) | 0700) == 0) {
            made = true;
            break;
        }
        if (errno != EEXIST)
            break;
        final_name = free_name(folder, name);
    }
    if (!made) {
        ctx.error("Couldn't create " + quoted(path_join(folder, name)) + ": " + std::strerror(errno));
        ok = false;
        return {};
    }
    ++ctx.progress.items_done;
    const std::string dest = path_join(folder, final_name);
    for (const auto &n : names) {
        if (ctx.cancelled()) {
            ok = false;
            break;
        }
        copy_entry(ctx, path_join(src, n), dest, n, ok, depth + 1);
    }
    ::chmod(dest.c_str(), st.st_mode & 07777);
    const timespec times[2] = {st.st_atim, st.st_mtim};
    ::utimensat(AT_FDCWD, dest.c_str(), times, AT_SYMLINK_NOFOLLOW);
    return final_name;
}

// Whether `folder` is `path` or inside it, wherever links lead (a link at
// `path` itself is copied as a link, so it doesn't count).
bool folder_inside(const std::string &folder, const std::string &path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    const std::string self = real_path(path), target = real_path(folder);
    return !self.empty() && !target.empty() && path_inside(target, self);
}

bool folder_ok(Ctx &ctx, const std::string &folder) {
    if (!is_dir(folder)) {
        ctx.error(folder + " is not a folder.");
        return false;
    }
    // Only Delete puts things in the trash, with the note Restore needs.
    if (trash::in_trash(folder)) {
        ctx.error("Files can't be put in the trash this way; use Delete.");
        return false;
    }
    return true;
}

void transfer(Ctx &ctx, const std::vector<std::string> &input, const std::string &folder_in, int mode /*0 copy,1 move,2 drop*/,
              bool same_folder_copies = false) {
    const std::string folder = clean_path(absolute_path(folder_in));
    if (!folder_ok(ctx, folder))
        return;
    const auto paths = outermost_paths(input);
    std::size_t budget = 100000;
    ctx.progress.items_total = 0;
    for (const auto &p : paths)
        scan_size(p, ctx.progress.bytes_total, ctx.progress.items_total, budget);
    if (budget == 0)
        ctx.progress.bytes_total = 0; // too big to know
    ctx.report_progress(true);
    struct stat fst;
    ::stat(folder.c_str(), &fst);
    ctx.result.folder = folder;
    for (const auto &src : paths) {
        if (ctx.cancelled())
            break;
        struct stat st;
        if (::lstat(src.c_str(), &st) != 0) {
            ctx.error(quoted(src) + " is no longer there.");
            continue;
        }
        bool move = mode == 1 || (mode == 2 && st.st_dev == fst.st_dev);
        // Moving something into the folder it's already in does nothing.
        if (move && clean_path(path_dirname(src)) == folder && !same_folder_copies)
            continue;
        if (folder_inside(folder, src)) {
            ctx.error("Can't put " + quoted(src) + " inside itself.");
            continue;
        }
        std::string name = path_basename(src);
        const Resolve r = resolve_name(ctx, src, folder, name);
        if (r == Resolve::Cancel)
            break;
        if (r == Resolve::Skip)
            continue;
        const std::string dest = path_join(folder, name);
        const FileId src_id{st.st_dev, st.st_ino, st.st_mode};
        if (move) {
            if (rename_noreplace(src, dest) == 0) {
                ctx.entry.moved.push_back({src, dest, src_id});
                ctx.result.created.push_back(dest);
                ctx.progress.items_done += 1;
                if (S_ISREG(st.st_mode))
                    ctx.progress.bytes_done += static_cast<uint64_t>(st.st_size);
                ctx.report_progress();
                continue;
            }
            if (errno != EXDEV) {
                ctx.error("Couldn't move " + quoted(src) + ": " + std::strerror(errno));
                continue;
            }
            // Across drives: copy, then the original goes to its own trash.
        }
        bool ok = true;
        const std::string made = copy_entry(ctx, src, folder, name, ok);
        if (!made.empty()) {
            const std::string made_path = path_join(folder, made);
            CreatedRecord rec;
            if (record_created(made_path, rec))
                ctx.entry.created.push_back(rec);
            ctx.result.created.push_back(made_path);
        }
        if (!ok || made.empty()) {
            if (move && !made.empty())
                ctx.error("Copied only part of " + quoted(src) + "; the original was left where it was.");
            continue;
        }
        if (move) {
            trash::Receipt receipt;
            std::string error;
            if (trash::put(src, receipt, error, &src_id))
                ctx.entry.trashed.push_back(receipt);
            else
                ctx.error("Copied " + quoted(src) + ", but couldn't move the original to the trash: " + error);
        }
    }
    ctx.report_progress(true);
}

void run_compress(Ctx &ctx, const std::vector<std::string> &input) {
    const auto paths = outermost_paths(input);
    if (paths.empty())
        return;
    // Search results and Recents can span folders: the archive goes in the
    // deepest folder holding them all, with their paths from there.
    std::string folder = path_dirname(paths.front());
    for (const auto &p : paths)
        while (!path_inside(path_dirname(p), folder))
            folder = path_dirname(folder);
    const std::string bsdtar = xdg::find_program("bsdtar");
    if (bsdtar.empty()) {
        ctx.error("Compressing needs bsdtar, from the libarchive package.");
        return;
    }
    // bsdtar writes into a hidden folder of its own beside the archive, so a
    // half-written archive stays out of sight.
    std::string tmpl = path_join(folder, ".flo-compress-XXXXXX");
    if (!::mkdtemp(&tmpl[0])) {
        ctx.error("Couldn't compress in " + folder + ": " + std::strerror(errno));
        return;
    }
    const std::string archive = archive_name_for(paths, folder);
    std::vector<std::string> argv{bsdtar, "-a", "-cf", path_join(tmpl, archive), "-C", folder, "--"};
    for (const auto &p : paths)
        argv.push_back(p == folder ? "." : p.substr(folder == "/" ? 1 : folder.size() + 1));
    ctx.progress.current = archive;
    ctx.report_progress(true);
    RunOptions opt;
    opt.timeout_ms = -1;
    opt.cancel = &ctx.state->cancel;
    opt.max_output = 64 * 1024;
    const ProcessResult r = run_process(argv, opt);
    const std::string made = path_join(tmpl, archive);
    if (r.ok()) {
        std::string name = archive;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (rename_noreplace(made, path_join(folder, name)) == 0) {
                CreatedRecord rec;
                if (record_created(path_join(folder, name), rec))
                    ctx.entry.created.push_back(rec);
                ctx.result.created.push_back(path_join(folder, name));
                ctx.result.folder = folder;
                break;
            }
            if (errno != EEXIST)
                break;
            name = archive_name_for(paths, folder);
        }
    } else if (r.cancelled) {
        ctx.result.cancelled = true;
    } else {
        std::string why = r.err;
        while (!why.empty() && (why.back() == '\n' || why.back() == ' '))
            why.pop_back();
        ctx.error("Couldn't compress: " + (why.empty() ? std::string("bsdtar failed") : why));
    }
    ::unlink(made.c_str());  // a leftover partial archive this op wrote
    ::rmdir(tmpl.c_str());   // empty by now; never removed with contents
}

void run_extract(Ctx &ctx, const std::string &archive_in) {
    const std::string archive = absolute_path(archive_in);
    const std::string folder = path_dirname(archive);
    struct stat st;
    if (::stat(archive.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        ctx.error(quoted(archive) + " is not an archive file.");
        return;
    }
    if (!folder_ok(ctx, folder))
        return;
    const std::string bsdtar = xdg::find_program("bsdtar");
    if (bsdtar.empty()) {
        ctx.error("Extracting needs bsdtar, from the libarchive package.");
        return;
    }
    std::string tmpl = path_join(folder, ".flo-extract-XXXXXX");
    if (!::mkdtemp(&tmpl[0])) {
        ctx.error("Couldn't extract in " + folder + ": " + std::strerror(errno));
        return;
    }
    ctx.progress.current = path_basename(archive);
    ctx.report_progress(true);
    // bsdtar refuses absolute paths and ".." and won't write through links
    // unless told to (-P), so nothing lands outside the hidden folder.
    RunOptions opt;
    opt.timeout_ms = -1;
    opt.cancel = &ctx.state->cancel;
    opt.max_output = 64 * 1024;
    const ProcessResult r = run_process({bsdtar, "-xf", archive, "-C", tmpl, "--no-same-owner"}, opt);
    std::vector<std::string> names;
    if (DIR *d = ::opendir(tmpl.c_str())) {
        while (dirent *e = ::readdir(d)) {
            const char *n = e->d_name;
            if (!(n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))))
                names.emplace_back(n);
        }
        ::closedir(d);
    }
    auto discard_partial = [&] {
        // What a failed or cancelled extraction left goes to the trash, not
        // away for good; an empty folder is simply removed.
        if (names.empty()) {
            ::rmdir(tmpl.c_str());
            return;
        }
        trash::Receipt receipt;
        std::string error;
        if (!trash::put(tmpl, receipt, error))
            ctx.error("The partial extraction was left in " + tmpl + ".");
    };
    if (!r.ok()) {
        if (r.cancelled)
            ctx.result.cancelled = true;
        else
            ctx.error("Couldn't extract " + quoted(archive) + ": " +
                      (r.err.empty() ? std::string("bsdtar failed") : r.err.substr(0, r.err.find('\n'))));
        discard_partial();
        return;
    }
    std::string target;
    if (names.size() == 1) {
        const std::string inner = path_join(tmpl, names[0]);
        for (int attempt = 0; attempt < 100; ++attempt) {
            const std::string name = free_name(folder, names[0]);
            if (rename_noreplace(inner, path_join(folder, name)) == 0) {
                target = path_join(folder, name);
                break;
            }
            if (errno != EEXIST)
                break;
        }
        ::rmdir(tmpl.c_str());
    } else {
        std::string stem = path_basename(archive);
        const std::size_t dot = extension_start(stem, false);
        if (dot != std::string::npos && dot > 0)
            stem = stem.substr(0, dot);
        // mkdtemp made it 0700; a plain new folder gets the usual mode.
        ::chmod(tmpl.c_str(), 0777 & ~current_umask());
        for (int attempt = 0; attempt < 100; ++attempt) {
            const std::string name = free_name(folder, stem);
            if (rename_noreplace(tmpl, path_join(folder, name)) == 0) {
                target = path_join(folder, name);
                break;
            }
            if (errno != EEXIST)
                break;
        }
    }
    if (target.empty()) {
        ctx.error("Couldn't place what was extracted; it is in " + tmpl + ".");
        return;
    }
    CreatedRecord rec;
    if (record_created(target, rec))
        ctx.entry.created.push_back(rec);
    ctx.result.created.push_back(target);
    ctx.result.folder = folder;
}

} // namespace

// ---------------------------------------------------------------------------

FileOps::FileOps(EventQueue &queue, FileOpsCallbacks callbacks) : impl_(std::make_shared<Impl>()) {
    impl_->poster = queue.poster();
    impl_->cb = std::move(callbacks);
    impl_->self = impl_;
}

FileOps::~FileOps() { cancel_all(); }

bool FileOps::busy() const { return impl_->active.load() > 0; }

void FileOps::cancel(uint64_t op) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->ops.find(op);
    if (it == impl_->ops.end())
        return;
    it->second->cancel.cancel();
    it->second->cv.notify_all();
    wake(*it->second);
}

void FileOps::cancel_all() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto &kv : impl_->ops) {
        kv.second->cancel.cancel();
        kv.second->cv.notify_all();
        wake(*kv.second);
    }
}

bool FileOps::pause(uint64_t op, bool on) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->ops.find(op);
    if (it == impl_->ops.end() || impl_->running.load() != op || it->second->cancel.cancelled())
        return false;
    {
        std::lock_guard<std::mutex> plock(it->second->pm);
        it->second->paused = on;
    }
    it->second->pcv.notify_all();
    return true;
}

bool FileOps::paused(uint64_t op) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->ops.find(op);
    return it != impl_->ops.end() && it->second->paused.load();
}

std::size_t FileOps::pending() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t n = 0;
    for (const auto &kv : impl_->ops)
        n += kv.first != impl_->running.load() && !kv.second->cancel.cancelled();
    return n;
}

std::size_t FileOps::clear_pending() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t n = 0;
    for (auto &kv : impl_->ops)
        if (kv.first != impl_->running.load() && !kv.second->cancel.cancelled()) {
            kv.second->cancel.cancel();
            ++n;
        }
    return n;
}

void FileOps::resolve_conflict(uint64_t op, ConflictChoice choice, bool apply_to_all) {
    std::shared_ptr<OpState> s;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->ops.find(op);
        if (it == impl_->ops.end())
            return;
        s = it->second;
    }
    std::lock_guard<std::mutex> lock(s->m);
    s->answered = true;
    s->choice = choice;
    s->apply_all = apply_to_all;
    s->cv.notify_all();
}

// Queues `work`, which fills the context; finishing (history, result) is common.
static uint64_t enqueue(const std::shared_ptr<FileOps::Impl> &impl, OpKind kind, ConflictPolicy policy,
                        std::function<void(Ctx &)> work) {
    auto state = std::make_shared<OpState>();
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        id = impl->next_id++;
        impl->ops[id] = state;
    }
    impl->active++;
    std::weak_ptr<FileOps::Impl> weak = impl;
    impl->exec.run([weak, id, kind, policy, state, work] {
        auto self = weak.lock();
        if (!self)
            return;
        self->running = id;
        Ctx ctx{self.get(), id, kind, state, policy, {}, {}, {}, {}};
        ctx.result.op = id;
        ctx.result.kind = kind;
        ctx.progress.op = id;
        ctx.progress.kind = kind;
        if (!ctx.cancelled())
            work(ctx);
        self->running = 0;
        ctx.result.cancelled = ctx.result.cancelled || ctx.state->cancel.cancelled();
        ctx.result.ok = ctx.result.errors.empty() && !ctx.result.cancelled;
        if (!ctx.entry.empty()) {
            ctx.entry.fence_ns = take_fence();
            if (!self->history.push(std::move(ctx.entry)))
                ctx.result.errors.push_back("Too many items to undo this later.");
            self->post_history();
        }
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->ops.erase(id);
        }
        self->active--;
        auto done = self->cb.on_done;
        if (done) {
            OpResult r = std::move(ctx.result);
            self->poster.post([done, r] { done(r); });
        }
    });
    return id;
}

uint64_t FileOps::copy(std::vector<std::string> paths, std::string folder, ConflictPolicy policy) {
    return enqueue(impl_, OpKind::Copy, policy, [paths, folder](Ctx &ctx) {
        ctx.entry.label = items_label("Copy", paths);
        transfer(ctx, paths, folder, 0);
    });
}

uint64_t FileOps::move(std::vector<std::string> paths, std::string folder, ConflictPolicy policy) {
    return enqueue(impl_, OpKind::Move, policy, [paths, folder](Ctx &ctx) {
        ctx.entry.label = items_label("Move", paths);
        transfer(ctx, paths, folder, 1);
    });
}

uint64_t FileOps::paste(std::vector<std::string> paths, std::string folder, bool cut, ConflictPolicy policy) {
    return cut ? move(std::move(paths), std::move(folder), policy) : copy(std::move(paths), std::move(folder), policy);
}

uint64_t FileOps::drop(std::vector<std::string> paths, std::string folder, ConflictPolicy policy) {
    return enqueue(impl_, OpKind::Move, policy, [paths, folder](Ctx &ctx) {
        ctx.entry.label = items_label("Move", paths);
        transfer(ctx, paths, folder, 2);
    });
}

uint64_t FileOps::duplicate(std::vector<std::string> paths) {
    return enqueue(impl_, OpKind::Duplicate, ConflictPolicy::KeepBoth, [paths](Ctx &ctx) {
        ctx.entry.label = items_label("Duplicate", paths);
        for (const auto &p : outermost_paths(paths)) {
            if (ctx.cancelled())
                break;
            transfer(ctx, {p}, path_dirname(p), 0, true);
        }
    });
}

uint64_t FileOps::rename(std::string path, std::string new_name) {
    return enqueue(impl_, OpKind::Rename, ConflictPolicy::KeepBoth, [path, new_name](Ctx &ctx) {
        const std::string from = absolute_path(path);
        std::string name = new_name;
        // Surrounding spaces are a typo, not part of the name (Folio).
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
            name.erase(0, 1);
        if (name.empty() || name == path_basename(from))
            return;
        if (!valid_file_name(name)) {
            ctx.error("A name can't contain \"/\".");
            return;
        }
        const std::string folder = path_dirname(from);
        const std::string to = path_join(folder, name);
        FileId id, other;
        if (!lstat_id(from, id)) {
            ctx.error(quoted(from) + " is no longer there.");
            return;
        }
        int r = rename_noreplace(from, to);
        // Changing only the case on a drive that ignores case: the "taken"
        // name is the item itself.
        if (r != 0 && errno == EEXIST && lstat_id(to, other) && other == id)
            r = ::rename(from.c_str(), to.c_str());
        if (r != 0) {
            ctx.error(errno == EEXIST ? name + " already exists here." : "Couldn't rename " + quoted(from) + ".");
            return;
        }
        ctx.entry.label = "Rename " + quoted(from) + " to " + quoted(to);
        ctx.entry.moved.push_back({from, to, id});
        ctx.result.created.push_back(to);
        ctx.result.folder = folder;
    });
}

uint64_t FileOps::new_folder(std::string folder, std::string name) {
    return enqueue(impl_, OpKind::NewFolder, ConflictPolicy::KeepBoth, [folder, name](Ctx &ctx) {
        const std::string f = absolute_path(folder);
        if (!folder_ok(ctx, f))
            return;
        if (!name.empty() && !valid_file_name(name)) {
            ctx.error("A name can't contain \"/\".");
            return;
        }
        for (int n = 1; n < 10000; ++n) {
            const std::string candidate =
                !name.empty() ? (n == 1 ? name : copy_name(name, true, n - 1))
                              : (n == 1 ? std::string("New Folder") : "New Folder " + std::to_string(n));
            const std::string path = path_join(f, candidate);
            if (::mkdir(path.c_str(), 0777) == 0) {
                CreatedRecord rec;
                if (record_created(path, rec, true))
                    ctx.entry.created.push_back(rec);
                ctx.entry.label = "New folder " + quoted(path);
                ctx.result.created.push_back(path);
                ctx.result.folder = f;
                return;
            }
            if (errno != EEXIST) {
                ctx.error("Couldn't create a folder here: " + std::string(std::strerror(errno)));
                return;
            }
        }
    });
}

uint64_t FileOps::compress(std::vector<std::string> paths) {
    return enqueue(impl_, OpKind::Compress, ConflictPolicy::KeepBoth, [paths](Ctx &ctx) {
        run_compress(ctx, paths);
        if (!ctx.result.created.empty())
            ctx.entry.label = "Compress into " + quoted(ctx.result.created.front());
    });
}

uint64_t FileOps::extract(std::string archive) {
    return enqueue(impl_, OpKind::Extract, ConflictPolicy::KeepBoth, [archive](Ctx &ctx) {
        run_extract(ctx, archive);
        ctx.entry.label = "Extract " + quoted(archive);
    });
}

uint64_t FileOps::trash(std::vector<std::string> paths) {
    return enqueue(impl_, OpKind::Trash, ConflictPolicy::KeepBoth, [paths](Ctx &ctx) {
        const auto roots = outermost_paths(paths);
        ctx.progress.items_total = roots.size();
        ctx.entry.label = items_label("Trash", roots);
        for (const auto &p : roots) {
            if (ctx.cancelled())
                break;
            trash::Receipt receipt;
            std::string error;
            ctx.progress.current = path_basename(p);
            if (trash::put(p, receipt, error)) {
                ctx.entry.trashed.push_back(receipt);
                ctx.result.created.push_back(receipt.files_path());
            } else {
                ctx.error(error);
            }
            ++ctx.progress.items_done;
            ctx.report_progress();
        }
        ctx.result.message = std::to_string(ctx.entry.trashed.size()) + " moved to the trash.";
    });
}

uint64_t FileOps::restore(std::vector<std::string> trashed_paths) {
    return enqueue(impl_, OpKind::Restore, ConflictPolicy::KeepBoth, [trashed_paths](Ctx &ctx) {
        for (const auto &p : trashed_paths) {
            if (ctx.cancelled())
                break;
            trash::Item item;
            if (!trash::item_for_trashed_path(p, item)) {
                ctx.error(quoted(p) + " is not an item in the trash.");
                continue;
            }
            std::string to, error;
            if (trash::restore(item, trash::OnConflict::KeepBoth, to, error)) {
                ctx.result.created.push_back(to);
                ctx.result.folder = path_dirname(to);
            } else {
                ctx.error(error);
            }
        }
        if (ctx.result.created.size() == 1)
            ctx.result.message = "Restored " + quoted(ctx.result.created.front()) + " to " + ctx.result.folder + ".";
        else if (!ctx.result.created.empty())
            ctx.result.message = "Restored " + std::to_string(ctx.result.created.size()) + " items.";
    });
}

uint64_t FileOps::delete_permanently(std::vector<std::string> trashed_paths, bool confirmed) {
    return enqueue(impl_, OpKind::DeletePermanently, ConflictPolicy::KeepBoth, [trashed_paths, confirmed](Ctx &ctx) {
        for (const auto &p : outermost_paths(trashed_paths)) {
            if (ctx.cancelled())
                break;
            std::string error;
            if (!trash::delete_permanently(p, confirmed, error))
                ctx.error(error);
        }
    });
}

uint64_t FileOps::empty_trash(bool confirmed) {
    return enqueue(impl_, OpKind::EmptyTrash, ConflictPolicy::KeepBoth, [confirmed](Ctx &ctx) {
        std::string error;
        const int n = trash::empty_all(confirmed, error);
        if (!error.empty())
            ctx.error(error);
        ctx.result.message = "Emptied the trash: " + std::to_string(n) + " items deleted.";
    });
}

uint64_t FileOps::undo() {
    return enqueue(impl_, OpKind::Undo, ConflictPolicy::KeepBoth, [](Ctx &ctx) {
        if (!ctx.impl->history.can_undo()) {
            ctx.result.message = "Nothing to undo.";
            return;
        }
        const UndoReport r = ctx.impl->history.undo();
        ctx.result.errors = r.kept;
        ctx.result.message = "Undid: " + r.label;
        ctx.impl->post_history();
    });
}

uint64_t FileOps::redo() {
    return enqueue(impl_, OpKind::Redo, ConflictPolicy::KeepBoth, [](Ctx &ctx) {
        if (!ctx.impl->history.can_redo()) {
            ctx.result.message = "Nothing to redo.";
            return;
        }
        const UndoReport r = ctx.impl->history.redo();
        ctx.result.errors = r.kept;
        ctx.result.message = "Redid: " + r.label;
        ctx.impl->post_history();
    });
}

} // namespace flo

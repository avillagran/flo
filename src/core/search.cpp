#include "core/search.h"
#include "core/fsutil.h"
#include "core/natural_sort.h"
#include "core/process.h"
#include "core/xdg.h"

#include <algorithm>
#include <cstdlib>

#include <chrono>
#include <cstring>
#include <deque>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo {

SearchStats search_walk(const std::string &root_in, const std::string &query, const SearchOptions &opt,
                        const CancelToken *cancel, const std::function<void(std::vector<Entry> &&)> &on_batch,
                        const std::unordered_set<std::string> *skip) {
    using clock = std::chrono::steady_clock;
    SearchStats stats;
    const std::string root = clean_path(root_in);
    struct stat top;
    if (::stat(root.c_str(), &top) != 0 || !S_ISDIR(top.st_mode))
        return stats;
    const std::string needle = fold_case(query);
    EntryOptions eopt;
    eopt.sniff = false; // names only; never read file contents while walking

    std::deque<std::string> folders{""}; // relative to root
    std::vector<Entry> batch;
    auto last_flush = clock::now();
    auto flush = [&] {
        if (!batch.empty() && on_batch)
            on_batch(std::move(batch));
        batch.clear();
        last_flush = clock::now();
    };

    while (!folders.empty()) {
        if (cancel && cancel->cancelled()) {
            stats.cancelled = true;
            break;
        }
        const std::string rel = std::move(folders.front());
        folders.pop_front();
        const std::string abs = rel.empty() ? root : path_join(root, rel);
        // O_NOFOLLOW: a folder replaced by a link after it was queued is not entered.
        const int fd = ::open(abs.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct stat here;
        if (::fstat(fd, &here) != 0 || here.st_dev != top.st_dev) {
            ::close(fd);
            continue;
        }
        DIR *dir = ::fdopendir(fd);
        if (!dir) {
            ::close(fd);
            continue;
        }
        ++stats.folders_walked;
        while (dirent *d = ::readdir(dir)) {
            if (cancel && cancel->cancelled()) {
                stats.cancelled = true;
                break;
            }
            const char *n = d->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
                continue;
            if (!opt.show_hidden && n[0] == '.')
                continue;
            const std::string name = n;
            const std::string child_rel = rel.empty() ? name : rel + "/" + name;
            // Walk into real folders on the same filesystem only; d_type
            // saves a stat on most filesystems.
            bool walk = false;
            if (d->d_type == DT_DIR) {
                walk = true;
            } else if (d->d_type == DT_UNKNOWN) {
                struct stat st;
                walk = ::fstatat(fd, n, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
            }
            if (walk)
                folders.push_back(child_rel);
            if (stats.matches < opt.limit && contains_folded(name, needle) && !(skip && skip->count(child_rel))) {
                Entry e;
                if (stat_entry(fd, name, e, eopt)) {
                    e.name = child_rel;
                    batch.push_back(std::move(e));
                    ++stats.matches;
                }
            }
            if (stats.matches >= opt.limit) {
                stats.truncated = true;
                break;
            }
            if (batch.size() >= opt.batch_max ||
                (!batch.empty() && clock::now() - last_flush > std::chrono::milliseconds(opt.batch_ms)))
                flush();
        }
        ::closedir(dir);
        if (stats.truncated || stats.cancelled)
            break;
    }
    if (!stats.cancelled)
        flush();
    return stats;
}

std::string locate_pattern(const std::string &root, const std::string &query) {
    auto escape = [](const std::string &t) {
        std::string o;
        for (char c : t) {
            if (std::strchr(".[]{}()\\*+?^$|", c))
                o += '\\';
            o += c;
        }
        return o;
    };
    std::string r = clean_path(root);
    if (r != "/")
        r += "/";
    return "^" + escape(r) + "(.*/)?[^/]*" + escape(query) + "[^/]*$";
}

bool search_locate(const std::string &root_in, const std::string &query, const SearchOptions &opt,
                   const CancelToken *cancel, std::vector<Entry> &out, std::unordered_set<std::string> &found) {
    if (opt.locate_program.empty() || query.empty())
        return false;
    const std::string program = opt.locate_program.find('/') != std::string::npos
                                    ? opt.locate_program
                                    : xdg::find_program(opt.locate_program);
    if (program.empty() || ::access(program.c_str(), X_OK) != 0)
        return false;
    const std::string root = clean_path(root_in);
    struct stat top;
    if (::stat(root.c_str(), &top) != 0 || !S_ISDIR(top.st_mode))
        return false;
    std::vector<std::string> argv = {program, "--ignore-case", "--null", "--limit",
                                     std::to_string(std::max<std::size_t>(1, opt.limit) * 2)};
    if (!opt.locate_db.empty())
        argv.insert(argv.end(), {"--database", opt.locate_db});
    argv.insert(argv.end(), {"--regex", "--", locate_pattern(root, query)});
    RunOptions ro;
    ro.timeout_ms = 5000;
    ro.max_output = 8u << 20;
    ro.cancel = cancel;
    // No address-space cap: plocate starts a thread per CPU for --regex and
    // aborts under one; the timeout and the output cap bound it instead.
    ProcessResult r = run_process(argv, ro);
    // plocate exits 1 for "no matches" too; only a missing database is a
    // failure (it says so on stderr and prints nothing).
    if (!r.started || r.timed_out || r.cancelled || r.term_signal != 0)
        return false;
    if (r.exit_code != 0 && (r.exit_code != 1 || !r.err.empty()))
        return false;
    const std::string needle = fold_case(query);
    const std::string prefix = root == "/" ? "/" : root + "/";
    EntryOptions eopt;
    eopt.sniff = false;
    std::size_t pos = 0;
    while (pos < r.out.size() && out.size() < opt.limit) {
        if (cancel && cancel->cancelled())
            return true;
        std::size_t end = r.out.find('\0', pos);
        if (end == std::string::npos)
            end = r.out.size();
        std::string path = r.out.substr(pos, end - pos);
        pos = end + 1;
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0)
            continue; // plocate's -i also folds the folder's own name
        std::string rel = path.substr(prefix.size());
        const std::string name = path_basename(path);
        if (!contains_folded(name, needle) || found.count(rel))
            continue;
        if (!opt.show_hidden) {
            bool hidden = rel[0] == '.' || rel.find("/.") != std::string::npos;
            if (hidden)
                continue;
        }
        // The database may be old: what's gone, moved to another
        // filesystem, or under a folder that is now a link is left out.
        const std::string parent = path_dirname(path);
        struct stat ps;
        if (::lstat(parent.c_str(), &ps) != 0 || !S_ISDIR(ps.st_mode) || ps.st_dev != top.st_dev)
            continue;
        if (parent.size() > root.size()) {
            bool linked = false;
            for (std::string p = parent; p.size() > root.size(); p = path_dirname(p)) {
                struct stat ls;
                if (::lstat(p.c_str(), &ls) != 0 || S_ISLNK(ls.st_mode)) {
                    linked = true;
                    break;
                }
            }
            if (linked)
                continue;
        }
        Entry e;
        if (!stat_path(path, e, eopt))
            continue;
        e.name = rel;
        found.insert(rel);
        out.push_back(std::move(e));
    }
    // Nearest first, as the walk orders them.
    std::stable_sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
        auto depth = [](const std::string &s) { return std::count(s.begin(), s.end(), '/'); };
        auto da = depth(a.name), db = depth(b.name);
        return da != db ? da < db : natural_less(a.name, b.name);
    });
    return true;
}

Searcher::Searcher(EventQueue &queue) : poster_(queue.poster()) {}

Searcher::~Searcher() { cancel_.cancel(); }

void Searcher::cancel() {
    cancel_.cancel();
    exec_.clear();
}

uint64_t Searcher::start(const std::string &root, const std::string &query, const SearchOptions &options,
                         BatchCallback on_batch, DoneCallback on_done) {
    cancel();
    cancel_ = CancelToken();
    const uint64_t gen = ++generation_;
    auto token = cancel_;
    auto poster = poster_;
    auto weak = alive_.weak();
    exec_.run([=] {
        if (token.cancelled())
            return;
        auto deliver = [&](std::vector<Entry> &&batch) {
            poster.post([weak, token, on_batch, b = std::move(batch)]() mutable {
                if (!weak.expired() && !token.cancelled() && on_batch)
                    on_batch(std::move(b));
            });
        };
        SearchOptions o = options;
        if (const char *p = std::getenv("FLO_PLOCATE"))
            o.locate_program = p;
        if (const char *d = std::getenv("FLO_PLOCATE_DB"))
            o.locate_db = d;
        std::vector<Entry> located;
        std::unordered_set<std::string> found;
        search_locate(root, query, o, &token, located, found);
        const std::size_t n_located = located.size();
        if (!located.empty())
            deliver(std::move(located));
        SearchStats stats;
        if (o.walk && !token.cancelled() && n_located < o.limit) {
            SearchOptions rest = o;
            rest.limit = o.limit - n_located;
            stats = search_walk(root, query, rest, &token, deliver, &found);
        }
        stats.located = n_located;
        stats.matches += n_located;
        stats.truncated = stats.truncated || n_located >= o.limit;
        poster.post([weak, token, on_done, stats] {
            if (!weak.expired() && !token.cancelled() && on_done)
                on_done(stats);
        });
    });
    return gen;
}

} // namespace flo

#include "core/search.h"
#include "core/fsutil.h"
#include "core/natural_sort.h"

#include <chrono>
#include <deque>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo {

SearchStats search_walk(const std::string &root_in, const std::string &query, const SearchOptions &opt,
                        const CancelToken *cancel, const std::function<void(std::vector<Entry> &&)> &on_batch) {
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
            if (stats.matches < opt.limit && contains_folded(name, needle)) {
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
        const SearchStats stats = search_walk(root, query, options, &token, [&](std::vector<Entry> &&batch) {
            poster.post([weak, token, on_batch, b = std::move(batch)]() mutable {
                if (!weak.expired() && !token.cancelled() && on_batch)
                    on_batch(std::move(b));
            });
        });
        poster.post([weak, token, on_done, stats] {
            if (!weak.expired() && !token.cancelled() && on_done)
                on_done(stats);
        });
    });
    return gen;
}

} // namespace flo

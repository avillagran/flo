#include "core/lister.h"
#include "core/fsutil.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

namespace flo {

std::string Listing::path_of(std::size_t i) const {
    if (i >= entries.size())
        return {};
    // Recents carry full paths (they span folders).
    const std::string &n = entries[i].name;
    return !n.empty() && n[0] == '/' ? n : path_join(folder, n);
}

static std::string error_text(int err) {
    switch (err) {
    case ENOENT: return "This folder no longer exists.";
    case EACCES:
    case EPERM: return "You don't have permission to open this folder.";
    case ENOTDIR: return "This is not a folder.";
    default: return std::string("Couldn't open this folder: ") + std::strerror(err);
    }
}

// FUSE (GVfs, sshfs), NFS and SMB: reading file heads would crawl.
static bool remote_fd(int fd) {
    struct statfs sf;
    if (::fstatfs(fd, &sf) != 0)
        return false;
    switch (static_cast<unsigned long>(sf.f_type)) {
    case 0x65735546UL: // FUSE
    case 0x6969UL:     // NFS
    case 0xFF534D42UL: // CIFS
    case 0xFE534D42UL: // SMB2
    case 0x517BUL:     // SMB
    case 0x01021997UL: // 9p
    case 0x73757245UL: // Coda
    case 0x5346414FUL: // AFS
    case 0x00C36400UL: // Ceph
        return true;
    default:
        return false;
    }
}

Listing list_folder(const std::string &folder, const ListOptions &opt, const CancelToken *cancel,
                    const std::function<void(const Listing &)> &progress) {
    using clock = std::chrono::steady_clock;
    Listing out;
    out.folder = clean_path(folder);
    const int fd = ::open(out.folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        out.error_code = errno;
        out.error = error_text(errno);
        return out;
    }
    DIR *dir = ::fdopendir(fd);
    if (!dir) {
        out.error_code = errno;
        out.error = error_text(errno);
        ::close(fd);
        return out;
    }
    EntryOptions eopt;
    eopt.sniff = opt.sniff && !remote_fd(fd);
    // Sniffing reads a file head; a huge folder of extensionless files
    // shouldn't turn into thousands of reads.
    std::size_t sniff_budget = 2000;

    const auto start = clock::now();
    auto next_progress = start + std::chrono::milliseconds(opt.first_progress_ms);
    while (dirent *d = ::readdir(dir)) {
        if (cancel && cancel->cancelled())
            break;
        const char *n = d->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        if (!opt.show_hidden && n[0] == '.')
            continue;
        Entry e;
        eopt.sniff = eopt.sniff && sniff_budget > 0;
        if (!stat_entry(fd, n, e, eopt))
            continue; // vanished between readdir and stat
        if (eopt.sniff && e.kind == Kind::Other)
            --sniff_budget;
        out.entries.push_back(std::move(e));
        if (progress && (out.entries.size() & 63) == 0 && clock::now() >= next_progress) {
            Listing snap;
            snap.folder = out.folder;
            snap.entries = out.entries;
            snap.complete = false;
            sort_entries(snap.entries, opt.sort);
            progress(snap);
            next_progress = clock::now() + std::chrono::milliseconds(opt.progress_interval_ms);
        }
    }
    ::closedir(dir);
    sort_entries(out.entries, opt.sort);
    return out;
}

int count_items(const std::string &folder, bool show_hidden) {
    DIR *dir = ::opendir(folder.c_str());
    if (!dir)
        return -1;
    int count = 0;
    while (dirent *d = ::readdir(dir)) {
        const char *n = d->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        if (!show_hidden && n[0] == '.')
            continue;
        ++count;
    }
    ::closedir(dir);
    return count;
}

Lister::Lister(EventQueue &queue) : poster_(queue.poster()) {}

Lister::~Lister() {
    list_cancel_.cancel();
    count_cancel_.cancel();
}

void Lister::cancel() {
    list_cancel_.cancel();
    list_exec_.clear();
    ++generation_;
}

uint64_t Lister::list(const std::string &folder, const ListOptions &options, Callback on_update) {
    cancel();
    list_cancel_ = CancelToken();
    const uint64_t gen = generation_;
    auto poster = poster_;
    auto token = list_cancel_;
    auto weak = alive_.weak();
    auto deliver = [poster, weak, gen, token, on_update](std::shared_ptr<Listing> l) {
        l->generation = gen;
        // A newer request cancels this token on the UI thread, so results
        // already queued for an overtaken listing are dropped there.
        poster.post([weak, token, on_update, l] {
            if (!weak.expired() && !token.cancelled())
                on_update(l);
        });
    };
    list_exec_.run([folder, options, token, deliver] {
        if (token.cancelled())
            return;
        Listing result = list_folder(folder, options, &token, [&](const Listing &snap) {
            if (!token.cancelled())
                deliver(std::make_shared<Listing>(snap));
        });
        if (!token.cancelled())
            deliver(std::make_shared<Listing>(std::move(result)));
    });
    return gen;
}

void Lister::cancel_counts() {
    count_cancel_.cancel();
    count_exec_.clear();
}

void Lister::count_items(std::vector<std::string> folders, bool show_hidden, CountCallback on_counts) {
    cancel_counts();
    count_cancel_ = CancelToken();
    auto token = count_cancel_;
    auto poster = poster_;
    auto weak = alive_.weak();
    count_exec_.run([folders = std::move(folders), show_hidden, token, poster, weak, on_counts] {
        using clock = std::chrono::steady_clock;
        std::vector<std::pair<std::string, int>> batch;
        auto flush = [&] {
            if (batch.empty() || token.cancelled())
                return;
            poster.post([weak, token, on_counts, b = std::move(batch)]() mutable {
                if (!weak.expired() && !token.cancelled())
                    on_counts(std::move(b));
            });
            batch.clear();
        };
        auto last = clock::now();
        for (const auto &f : folders) {
            if (token.cancelled())
                return;
            batch.emplace_back(f, flo::count_items(f, show_hidden));
            if (clock::now() - last > std::chrono::milliseconds(40)) {
                flush();
                last = clock::now();
            }
        }
        flush();
    });
}

} // namespace flo

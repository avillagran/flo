#include "core/thumbnail_service.h"
#include "core/entry.h"
#include "core/fsutil.h"
#include "core/process.h"
#include "core/thumbnail_protocol.h"
#include "core/thumbnailers.h"
#include "core/xdg.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo {

const char *thumbnail_status_name(ThumbnailStatus s) {
    switch (s) {
    case ThumbnailStatus::Ready: return "ready";
    case ThumbnailStatus::Failed: return "failed";
    case ThumbnailStatus::Unsupported: return "unsupported";
    case ThumbnailStatus::Skipped: return "skipped";
    }
    return "failed";
}

struct ThumbnailService::Impl {
    using clock = std::chrono::steady_clock;
    struct Request {
        uint64_t id = 0;
        std::string path;
        int pixels = 0;
        std::string mime;
        bool mark_failed = false; // an F command (no callback)
    };
    struct Worker {
        PipeChild child;
        bool busy = false;
        Request current;
        clock::time_point deadline, idle_since;
        std::string inbuf;
    };

    ThumbnailServiceOptions opt;
    ThumbnailCache cache;
    EventQueue::Poster poster;
    std::mutex mutex;
    std::deque<Request> pending; // front = newest
    std::map<uint64_t, Callback> callbacks;
    uint64_t next_id = 1;
    bool stop = false;
    int wake_fd = -1;
    std::thread thread;
    std::vector<Worker> workers;
    ThumbnailerRegistry registry;
    bool registry_loaded = false;

    void wake() {
        const uint64_t one = 1;
        ssize_t ignored = ::write(wake_fd, &one, sizeof one);
        (void)ignored;
    }

    void deliver(uint64_t id, ThumbnailResult r) {
        Callback cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = callbacks.find(id);
            if (it == callbacks.end())
                return;
            cb = std::move(it->second);
            callbacks.erase(it);
        }
        poster.post([cb, r] { cb(r); });
    }

    // Answers what can be answered without a worker. Returns true if done.
    bool answer_locally(Request &req) {
        ThumbnailResult r;
        r.path = req.path;
        r.pixels = req.pixels;
        struct stat st;
        if (::stat(req.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            r.status = ThumbnailStatus::Skipped;
            r.error = "not a regular file";
        } else if (cache.inside_cache(req.path)) {
            r.status = ThumbnailStatus::Skipped;
            r.error = "inside the thumbnail cache";
        } else {
            ThumbnailCache::Hit hit;
            if (cache.lookup(req.path, st, req.pixels, hit)) {
                r.status = ThumbnailStatus::Ready;
                r.thumbnail = hit.path;
                r.thumbnail_pixels = hit.pixels;
            } else if (cache.failed_before(req.path, st)) {
                r.status = ThumbnailStatus::Failed;
                r.error = "failed before";
            } else {
                if (!registry_loaded) {
                    registry.load(ThumbnailerRegistry::standard_dirs());
                    registry_loaded = true;
                }
                bool uncertain = false;
                const std::string mime = req.mime.empty() ? mime_for_name(req.path, &uncertain) : req.mime;
                if (!uncertain)
                    req.mime = mime;
                if (uncertain || mime == "image/jpeg" || mime == "image/png" || registry.find(mime))
                    return false; // a worker's job (it sniffs uncertain types)
                r.status = ThumbnailStatus::Unsupported;
                r.error = "no thumbnailer for " + mime;
            }
        }
        deliver(req.id, std::move(r));
        return true;
    }

    std::string worker_path() const {
        if (!opt.worker_path.empty())
            return opt.worker_path;
        if (const char *env = std::getenv("FLO_THUMBNAILER"))
            if (*env == '/')
                return env;
        const std::string beside = executable_dir() + "/flo-thumbnailer";
        if (::access(beside.c_str(), X_OK) == 0)
            return beside;
        return "flo-thumbnailer";
    }

    bool spawn(Worker &w) {
        ProcessLimits limits; // the worker limits itself; children get theirs
        std::vector<std::string> env;
        if (cache.root() != ThumbnailCache::default_root())
            env.push_back("FLO_THUMBNAIL_CACHE=" + cache.root());
        return spawn_pipe_child({worker_path()}, limits, w.child, env);
    }

    bool send(Worker &w, const Request &req) {
        std::string line;
        if (req.mark_failed)
            line = "F\t" + std::to_string(req.id) + "\t" + thumbproto::encode(req.path) + "\n";
        else
            line = "T\t" + std::to_string(req.id) + "\t" + std::to_string(req.pixels) + "\t" +
                   thumbproto::encode(req.path) + "\t" + (req.mime.empty() ? "-" : req.mime) + "\n";
        if (!write_to_child(w.child, line))
            return false;
        w.busy = true;
        w.current = req;
        w.deadline = clock::now() + std::chrono::milliseconds(opt.request_timeout_ms);
        return true;
    }

    // The worker died or hung on its request: report it, and have the next
    // worker leave a failure marker so the file isn't retried forever.
    void worker_lost(Worker &w, const char *why) {
        kill_pipe_child(w.child);
        if (w.busy && !w.current.mark_failed) {
            ThumbnailResult r;
            r.path = w.current.path;
            r.pixels = w.current.pixels;
            r.status = ThumbnailStatus::Failed;
            r.error = why;
            deliver(w.current.id, std::move(r));
            Request f;
            f.mark_failed = true;
            f.path = w.current.path;
            std::lock_guard<std::mutex> lock(mutex);
            f.id = next_id++;
            pending.push_back(f);
        }
        w.busy = false;
    }

    void handle_line(Worker &w, const std::string &line) {
        const auto f = thumbproto::split_tabs(line);
        if (f.size() < 2)
            return;
        const uint64_t id = std::strtoull(f[0].c_str(), nullptr, 10);
        if (!w.busy || id != w.current.id)
            return;
        w.busy = false;
        w.idle_since = clock::now();
        if (w.current.mark_failed)
            return;
        ThumbnailResult r;
        r.path = w.current.path;
        r.pixels = w.current.pixels;
        const std::string payload = f.size() > 2 ? thumbproto::decode(f[2]) : std::string();
        if (f[1] == "ok") {
            r.status = ThumbnailStatus::Ready;
            r.thumbnail = payload;
            // The bucket is the thumbnail's folder name.
            r.thumbnail_pixels = ThumbnailCache::bucket_pixels(path_basename(path_dirname(payload)).c_str());
        } else {
            r.status = f[1] == "none" ? ThumbnailStatus::Unsupported
                     : f[1] == "skip" ? ThumbnailStatus::Skipped
                                      : ThumbnailStatus::Failed;
            r.error = payload;
        }
        deliver(w.current.id, std::move(r));
    }

    void run() {
        const std::size_t max_workers = static_cast<std::size_t>(opt.workers);
        for (;;) {
            // Hand out work.
            for (;;) {
                Request req;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stop)
                        goto done;
                    if (pending.empty())
                        break;
                    req = pending.front();
                    pending.pop_front();
                    if (!req.mark_failed && !callbacks.count(req.id))
                        continue; // cancelled meanwhile
                }
                if (!req.mark_failed && answer_locally(req))
                    continue;
                Worker *idle = nullptr;
                for (auto &w : workers)
                    if (!w.busy && w.child.pid > 0) {
                        idle = &w;
                        break;
                    }
                if (!idle && workers.size() < max_workers) {
                    workers.emplace_back();
                    if (!spawn(workers.back())) {
                        workers.pop_back();
                        ThumbnailResult r;
                        r.path = req.path;
                        r.pixels = req.pixels;
                        r.status = ThumbnailStatus::Failed;
                        r.error = "couldn't start " + worker_path();
                        if (!req.mark_failed)
                            deliver(req.id, std::move(r));
                        continue;
                    }
                    idle = &workers.back();
                }
                if (!idle) {
                    // Every worker is busy: put it back and wait.
                    std::lock_guard<std::mutex> lock(mutex);
                    pending.push_front(req);
                    break;
                }
                if (!send(*idle, req)) {
                    worker_lost(*idle, "worker stopped");
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!req.mark_failed)
                        pending.push_front(req);
                }
            }
            // Remove dead workers.
            for (std::size_t i = 0; i < workers.size();) {
                if (workers[i].child.pid <= 0)
                    workers.erase(workers.begin() + static_cast<long>(i));
                else
                    ++i;
            }

            std::vector<pollfd> fds;
            fds.push_back({wake_fd, POLLIN, 0});
            for (auto &w : workers)
                fds.push_back({w.child.from_child, POLLIN, 0});
            int timeout = -1;
            const auto now = clock::now();
            for (auto &w : workers) {
                const auto until = w.busy ? w.deadline : w.idle_since + std::chrono::milliseconds(opt.idle_exit_ms);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
                const int t = ms < 0 ? 0 : static_cast<int>(std::min<long long>(ms, 60000));
                if (timeout < 0 || t < timeout)
                    timeout = t;
            }
            const int r = ::poll(fds.data(), fds.size(), timeout);
            if (r < 0 && errno != EINTR)
                break;
            if (fds[0].revents & POLLIN) {
                uint64_t v;
                ssize_t ignored = ::read(wake_fd, &v, sizeof v);
                (void)ignored;
            }
            for (std::size_t k = 0; k < workers.size(); ++k) {
                Worker &w = workers[k];
                const short rev = fds[k + 1].revents;
                if (rev & (POLLIN | POLLHUP | POLLERR)) {
                    char buf[4096];
                    const ssize_t n = ::read(w.child.from_child, buf, sizeof buf);
                    if (n <= 0) {
                        worker_lost(w, "worker crashed");
                        continue;
                    }
                    w.inbuf.append(buf, static_cast<std::size_t>(n));
                    if (w.inbuf.size() > (1u << 20)) {
                        worker_lost(w, "worker sent garbage");
                        continue;
                    }
                    std::size_t nl;
                    while ((nl = w.inbuf.find('\n')) != std::string::npos) {
                        const std::string line = w.inbuf.substr(0, nl);
                        w.inbuf.erase(0, nl + 1);
                        handle_line(w, line);
                    }
                }
            }
            const auto later = clock::now();
            for (auto &w : workers) {
                if (w.child.pid <= 0)
                    continue;
                if (w.busy && later >= w.deadline)
                    worker_lost(w, "timed out");
                else if (!w.busy && later - w.idle_since >= std::chrono::milliseconds(opt.idle_exit_ms))
                    kill_pipe_child(w.child); // idle: nothing in the background
            }
        }
    done:
        for (auto &w : workers)
            kill_pipe_child(w.child);
        workers.clear();
    }
};

ThumbnailService::ThumbnailService(EventQueue &queue, ThumbnailServiceOptions options)
    : cache_(options.cache_root.empty() ? ThumbnailCache::default_root() : options.cache_root),
      impl_(std::make_shared<Impl>()) {
    if (options.workers <= 0) {
        const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
        options.workers = static_cast<int>(std::max(2L, std::min(4L, cpus / 4)));
    }
    options.workers = std::max(1, std::min(options.workers, 8));
    impl_->opt = options;
    impl_->cache = cache_;
    impl_->poster = queue.poster();
    impl_->wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

ThumbnailService::~ThumbnailService() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
        impl_->callbacks.clear();
        impl_->pending.clear();
    }
    if (impl_->thread.joinable()) {
        impl_->wake();
        impl_->thread.join();
    }
    if (impl_->wake_fd >= 0)
        ::close(impl_->wake_fd);
}

uint64_t ThumbnailService::request(const std::string &path, int pixels, Callback callback, const std::string &mime) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        id = impl_->next_id++;
        Impl::Request req;
        req.id = id;
        req.path = absolute_path(path);
        req.pixels = pixels;
        req.mime = mime;
        impl_->pending.push_front(std::move(req));
        impl_->callbacks[id] = std::move(callback);
        if (!impl_->thread.joinable()) {
            Impl *p = impl_.get();
            impl_->thread = std::thread([p] { p->run(); });
        }
    }
    impl_->wake();
    return id;
}

void ThumbnailService::cancel(uint64_t ticket) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callbacks.erase(ticket);
    for (auto it = impl_->pending.begin(); it != impl_->pending.end(); ++it)
        if (it->id == ticket && !it->mark_failed) {
            impl_->pending.erase(it);
            break;
        }
}

void ThumbnailService::cancel_all() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callbacks.clear();
    std::deque<Impl::Request> keep;
    for (auto &r : impl_->pending)
        if (r.mark_failed)
            keep.push_back(r);
    impl_->pending.swap(keep);
}

bool ThumbnailService::lookup(const std::string &path, int pixels, ThumbnailCache::Hit &hit) const {
    struct stat st;
    const std::string abs = absolute_path(path);
    return ::stat(abs.c_str(), &st) == 0 && S_ISREG(st.st_mode) && cache_.lookup(abs, st, pixels, hit);
}

} // namespace flo

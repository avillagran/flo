#include "core/watcher.h"
#include "core/fsutil.h"

#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <cerrno>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace flo {

namespace {
constexpr uint32_t kFolderMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                                 IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_MODIFY;
}

struct Watcher::State {
    using clock = std::chrono::steady_clock;
    struct Sub {
        int wd = -1;
        std::string name; // watch_file: only events about this name
        std::function<void()> callback;
        bool pending = false;
        clock::time_point due;
    };

    std::mutex mutex;
    std::map<int, Sub> subs;
    int next_id = 1;
    int inotify_fd = -1;
    int wake_fd = -1;
    bool stop = false;
    int debounce_ms = 250;
    EventQueue::Poster poster;
    std::thread thread;

    void run() {
        char buf[16384] __attribute__((aligned(__alignof__(inotify_event))));
        for (;;) {
            int timeout = -1;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stop)
                    return;
                const auto now = clock::now();
                for (auto &kv : subs) {
                    if (!kv.second.pending)
                        continue;
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(kv.second.due - now).count();
                    const int ms = left < 0 ? 0 : static_cast<int>(left);
                    if (timeout < 0 || ms < timeout)
                        timeout = ms;
                }
            }
            pollfd fds[2] = {{inotify_fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
            const int r = ::poll(fds, 2, timeout);
            if (r < 0 && errno != EINTR)
                return;
            if (fds[1].revents & POLLIN) {
                uint64_t v;
                ssize_t ignored = ::read(wake_fd, &v, sizeof v);
                (void)ignored;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (stop)
                return;
            if (fds[0].revents & POLLIN) {
                const ssize_t n = ::read(inotify_fd, buf, sizeof buf);
                for (ssize_t off = 0; n > 0 && off < n;) {
                    const auto *ev = reinterpret_cast<const inotify_event *>(buf + off);
                    const std::string name = ev->len ? std::string(ev->name) : std::string();
                    for (auto &kv : subs) {
                        Sub &s = kv.second;
                        if (s.wd != ev->wd && !(ev->mask & IN_Q_OVERFLOW))
                            continue;
                        if (!s.name.empty() && name != s.name && !(ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)))
                            continue;
                        // Debounced: each event pushes the callback back, up
                        // to four times the delay for a steady stream.
                        const auto now = clock::now();
                        if (!s.pending) {
                            s.pending = true;
                            s.due = now + std::chrono::milliseconds(debounce_ms);
                        } else {
                            s.due = std::min(now + std::chrono::milliseconds(debounce_ms),
                                             s.due + std::chrono::milliseconds(debounce_ms * 3));
                        }
                    }
                    off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
                }
            }
            const auto now = clock::now();
            for (auto &kv : subs) {
                Sub &s = kv.second;
                if (s.pending && s.due <= now) {
                    s.pending = false;
                    const int id = kv.first;
                    auto self_weak = std::weak_ptr<State>(self_ref);
                    poster.post([self_weak, id] {
                        auto self = self_weak.lock();
                        if (!self)
                            return;
                        std::function<void()> cb;
                        {
                            std::lock_guard<std::mutex> l(self->mutex);
                            auto it = self->subs.find(id);
                            if (it == self->subs.end())
                                return;
                            cb = it->second.callback;
                        }
                        if (cb)
                            cb();
                    });
                }
            }
        }
    }
    std::weak_ptr<State> self_ref;
};

Watcher::Watcher(EventQueue &queue, int debounce_ms) : state_(std::make_shared<State>()) {
    state_->poster = queue.poster();
    state_->debounce_ms = debounce_ms;
    state_->self_ref = state_;
    state_->inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    state_->wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state_->inotify_fd >= 0 && state_->wake_fd >= 0) {
        State *s = state_.get();
        state_->thread = std::thread([s] { s->run(); });
    }
}

Watcher::~Watcher() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop = true;
        state_->subs.clear();
    }
    if (state_->wake_fd >= 0) {
        const uint64_t one = 1;
        ssize_t ignored = ::write(state_->wake_fd, &one, sizeof one);
        (void)ignored;
    }
    if (state_->thread.joinable())
        state_->thread.join();
    if (state_->inotify_fd >= 0)
        ::close(state_->inotify_fd);
    if (state_->wake_fd >= 0)
        ::close(state_->wake_fd);
}

static int add(const std::shared_ptr<Watcher::State> &, const std::string &, const std::string &,
               std::function<void()>);

int Watcher::watch_folder(const std::string &folder, std::function<void()> on_change) {
    return add(state_, folder, {}, std::move(on_change));
}

int Watcher::watch_file(const std::string &file, std::function<void()> on_change) {
    return add(state_, path_dirname(file), path_basename(file), std::move(on_change));
}

static int add(const std::shared_ptr<Watcher::State> &s, const std::string &dir, const std::string &name,
               std::function<void()> cb) {
    if (s->inotify_fd < 0)
        return 0;
    const int wd = ::inotify_add_watch(s->inotify_fd, dir.c_str(), kFolderMask | IN_ONLYDIR);
    if (wd < 0)
        return 0;
    std::lock_guard<std::mutex> lock(s->mutex);
    const int id = s->next_id++;
    Watcher::State::Sub sub;
    sub.wd = wd;
    sub.name = name;
    sub.callback = std::move(cb);
    s->subs[id] = std::move(sub);
    return id;
}

void Watcher::unwatch(int id) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto it = state_->subs.find(id);
    if (it == state_->subs.end())
        return;
    const int wd = it->second.wd;
    state_->subs.erase(it);
    // inotify shares one wd per inode; remove it only when nobody uses it.
    for (const auto &kv : state_->subs)
        if (kv.second.wd == wd)
            return;
    ::inotify_rm_watch(state_->inotify_fd, wd);
}

} // namespace flo

#include "core/event_queue.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <chrono>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>

namespace flo {

struct EventQueue::Poster::State {
    std::mutex mutex;
    std::deque<std::function<void()>> pending;
    int fd = -1;
    bool closed = false;

    void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed || fd < 0)
            return;
        const bool was_empty = pending.empty();
        pending.push_back(std::move(fn));
        if (was_empty) {
            const uint64_t one = 1;
            ssize_t ignored = ::write(fd, &one, sizeof one);
            (void)ignored;
        }
    }
};

EventQueue::EventQueue() : state_(std::make_shared<Poster::State>()) {
    state_->fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

EventQueue::~EventQueue() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
    state_->pending.clear();
    if (state_->fd >= 0)
        ::close(state_->fd);
    state_->fd = -1;
}

int EventQueue::fd() const { return state_->fd; }

void EventQueue::post(std::function<void()> fn) const { state_->post(std::move(fn)); }

void EventQueue::Poster::post(std::function<void()> fn) const {
    if (state_)
        state_->post(std::move(fn));
}

EventQueue::Poster EventQueue::poster() const {
    Poster p;
    p.state_ = state_;
    return p;
}

std::size_t EventQueue::dispatch(std::size_t max) {
    std::size_t ran = 0;
    while (ran < max) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->pending.empty()) {
                uint64_t value;
                ssize_t ignored = ::read(state_->fd, &value, sizeof value);
                (void)ignored;
                break;
            }
            fn = std::move(state_->pending.front());
            state_->pending.pop_front();
        }
        if (fn)
            fn();
        ++ran;
    }
    return ran;
}

bool EventQueue::wait(int timeout_ms) const {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->pending.empty())
            return true;
    }
    pollfd p{state_->fd, POLLIN, 0};
    int r;
    do {
        r = ::poll(&p, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}

// ---------------------------------------------------------------------------

struct Executor::State {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    bool running = false; // a thread exists
    bool stopped = false;
};

Executor::Executor() : state_(std::make_shared<State>()) {}

Executor::~Executor() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopped = true;
    state_->jobs.clear();
    state_->cv.notify_all();
}

void Executor::clear() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->jobs.clear();
}

void Executor::run(std::function<void()> job) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped)
        return;
    state_->jobs.push_back(std::move(job));
    state_->cv.notify_one();
    if (state_->running)
        return;
    state_->running = true;
    std::shared_ptr<State> s = state_;
    std::thread([s] {
        std::unique_lock<std::mutex> lock(s->mutex);
        for (;;) {
            if (s->stopped)
                break;
            if (s->jobs.empty()) {
                // Idle threads go away; the next job starts a new one.
                if (!s->cv.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return s->stopped || !s->jobs.empty(); }))
                    break;
                continue;
            }
            auto job = std::move(s->jobs.front());
            s->jobs.pop_front();
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
        }
        s->running = false;
    }).detach();
}

} // namespace flo

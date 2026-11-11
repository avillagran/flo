// Delivery of results from worker threads to the UI thread.
//
// Every asynchronous service in flo-core reports through an EventQueue:
// worker threads post closures, and the UI thread runs them by calling
// dispatch(). The queue owns an eventfd that becomes readable while closures
// are pending, so an FLTK UI integrates it with
//
//     Fl::add_fd(queue.fd(), FL_READ, [](int, void *q) {
//         static_cast<flo::EventQueue *>(q)->dispatch();
//     }, &queue);
//
// and a CLI or test simply loops on wait() + dispatch(). Callbacks given to
// any flo-core service always run inside dispatch(), on the thread that calls
// it, never on a worker thread.
#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <cstddef>

namespace flo {

class EventQueue {
public:
    EventQueue();
    ~EventQueue();
    EventQueue(const EventQueue &) = delete;
    EventQueue &operator=(const EventQueue &) = delete;

    // Readable (poll/select/Fl::add_fd) while closures are pending.
    int fd() const;

    // Thread-safe. Posting after the queue was destroyed is a no-op, so
    // workers that outlive it never touch freed memory.
    void post(std::function<void()> fn) const;

    // Runs pending closures on the calling thread; returns how many ran.
    std::size_t dispatch(std::size_t max = static_cast<std::size_t>(-1));

    // Blocks up to timeout_ms (-1 = forever) until something is pending.
    bool wait(int timeout_ms) const;

    // A cheap copyable handle workers keep instead of a reference, so a
    // worker that outlives the queue posts into nothing.
    class Poster {
    public:
        Poster() = default;
        void post(std::function<void()> fn) const;
        explicit operator bool() const { return static_cast<bool>(state_); }

    private:
        friend class EventQueue;
        struct State;
        std::shared_ptr<State> state_;
    };
    Poster poster() const;

private:
    std::shared_ptr<Poster::State> state_;
};

// A cancellation flag shared between the requester and a job.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() const { flag_->store(true, std::memory_order_relaxed); }
    bool cancelled() const { return flag_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// Lifetime guard for objects that post callbacks capturing `this`: posted
// closures hold a weak reference and do nothing once the owner is gone.
class AliveGuard {
public:
    AliveGuard() : alive_(std::make_shared<char>(0)) {}
    std::weak_ptr<char> weak() const { return alive_; }
    void reset() { alive_ = std::make_shared<char>(0); }

private:
    std::shared_ptr<char> alive_;
};

// A serial job runner on its own thread. The thread starts when the first
// job is queued and ends after a few idle seconds, so idle services cost no
// thread. Destroying the executor drops queued jobs and does not wait for a
// running one (which should check its CancelToken); jobs therefore must only
// touch state they own or hold through shared pointers.
class Executor {
public:
    Executor();
    ~Executor();
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;

    void run(std::function<void()> job);
    // Drops jobs that have not started yet.
    void clear();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace flo

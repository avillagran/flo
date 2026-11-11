#include "core/gmain_thread.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include <glib.h>

namespace flo {

struct GMainThread::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    std::thread thread;
    std::thread::id id;
    bool started = false, stopped = false;

    void start() {
        // Called with the mutex held.
        if (started)
            return;
        started = true;
        context = g_main_context_new();
        loop = g_main_loop_new(context, FALSE);
        thread = std::thread([this] {
            g_main_context_push_thread_default(context);
            {
                std::lock_guard<std::mutex> lock(mutex);
                id = std::this_thread::get_id();
            }
            cv.notify_all();
            g_main_loop_run(loop);
            g_main_context_pop_thread_default(context);
        });
    }
};

GMainThread &GMainThread::instance() {
    static GMainThread t;
    return t;
}

GMainThread::GMainThread() : impl_(new Impl) {}

GMainThread::~GMainThread() {
    stop();
    delete impl_;
}

void GMainThread::stop() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->stopped)
        return;
    impl_->stopped = true;
    g_main_loop_quit(impl_->loop);
    lock.unlock();
    if (impl_->thread.joinable())
        impl_->thread.join();
    g_main_loop_unref(impl_->loop);
    g_main_context_unref(impl_->context);
    impl_->loop = nullptr;
    impl_->context = nullptr;
}

GMainContext *GMainThread::context() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->start();
    return impl_->context;
}

bool GMainThread::on_thread() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->started && std::this_thread::get_id() == impl_->id;
}

static gboolean run_once(gpointer data) {
    auto *fn = static_cast<std::function<void()> *>(data);
    (*fn)();
    return G_SOURCE_REMOVE;
}

static void free_fn(gpointer data) { delete static_cast<std::function<void()> *>(data); }

void GMainThread::invoke(std::function<void()> fn) {
    GMainContext *ctx;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopped)
            return;
        impl_->start();
        ctx = impl_->context;
    }
    GSource *source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, run_once, new std::function<void()>(std::move(fn)), free_fn);
    g_source_attach(source, ctx);
    g_source_unref(source);
}

void GMainThread::invoke_sync(std::function<void()> fn) {
    if (on_thread()) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    invoke([&] {
        fn();
        std::lock_guard<std::mutex> lock(m);
        done = true;
        cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(m);
    {
        std::lock_guard<std::mutex> l2(impl_->mutex);
        if (impl_->stopped)
            return;
    }
    cv.wait(lock, [&] { return done; });
}

} // namespace flo

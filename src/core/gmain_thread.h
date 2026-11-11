// A GLib main loop on its own thread, for the GIO pieces that need one:
// GVfs mounts (and their questions), the volume monitor and the D-Bus
// services. FLTK runs its own event loop, so instead of iterating a
// GMainContext from FLTK, flo-core keeps a dedicated context on one thread
// and reports back through the EventQueue. GIO is already loaded by FLTK, so
// this costs a thread and no extra libraries; a helper process would have
// duplicated GIO's memory. The thread starts on first use.
#pragma once

#include <functional>

typedef struct _GMainContext GMainContext;

namespace flo {

class GMainThread {
public:
    // The process-wide instance (started lazily, stopped at exit).
    static GMainThread &instance();

    // Runs `fn` on the GLib thread (with its context as thread default).
    void invoke(std::function<void()> fn);
    // Runs `fn` there and waits for it.
    void invoke_sync(std::function<void()> fn);
    bool on_thread() const;
    GMainContext *context() const;

    void stop();
    ~GMainThread();

private:
    GMainThread();
    struct Impl;
    Impl *impl_;
};

} // namespace flo

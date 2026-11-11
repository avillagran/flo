// Live reload: inotify watches on folders (and single files such as
// recently-used.xbel, which GTK replaces on every write), debounced so a
// burst of changes (a copy, a download) produces one callback.
#pragma once

#include "core/event_queue.h"

#include <functional>
#include <memory>
#include <string>

namespace flo {

class Watcher {
public:
    explicit Watcher(EventQueue &queue, int debounce_ms = 250);
    ~Watcher();
    Watcher(const Watcher &) = delete;
    Watcher &operator=(const Watcher &) = delete;

    // Calls `on_change` (UI thread) once things in `folder` settle after a
    // change: entries created, deleted, renamed, written or changed
    // attributes, or the folder itself deleted or moved. Returns an id for
    // unwatch(), or 0 when the folder can't be watched.
    int watch_folder(const std::string &folder, std::function<void()> on_change);
    // Watches one file through its folder, so replacing it is seen too.
    int watch_file(const std::string &file, std::function<void()> on_change);
    void unwatch(int id);

    struct State; // implementation detail

private:
    std::shared_ptr<State> state_;
};

} // namespace flo

// The GUI process: one App holding what every window shares (the event
// queue, thumbnails, live reload, drives, the tailnet, settings, the file
// clipboard and the preview process), and any number of MainWindows. Ctrl+N
// and `flo PATH` (through D-Bus) open more windows in this same process.
#pragma once

#include "core/dbus_service.h"
#include "core/event_queue.h"
#include "core/folder_views.h"
#include "core/starred.h"
#include "core/lister.h"
#include "core/network.h"
#include "core/places.h"
#include "core/process.h"
#include "core/settings.h"
#include "core/thumbnail_service.h"
#include "core/watcher.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo::ui {

class MainWindow;
class ThumbImages;
class FolderSizes;

// Keeps big short-lived buffers out of the heap (the window and the preview).
void tune_malloc();

// The sidebar's data, shared by every window.
struct PlacesData {
    std::string home;
    std::vector<Place> home_places, pinned, remote, views, drives;
    std::vector<TailnetPeer> peers;
    std::string dropbox; // synced / syncing / stopped / ""
};

// `flo preview PATH` as a child process, as Folio does: a crash in a
// decoder takes the preview down, never the window. One line per path
// each way over its stdin/stdout: Flo writes the item selected, the preview
// writes the item it stepped to.
class PreviewClient {
public:
    ~PreviewClient();
    bool running() const { return child_.pid > 0; }
    // Starts the preview on `path`, or closes it when it is running.
    void toggle(const std::string &path, bool show_hidden, const std::string &sort,
                std::function<void(const std::string &)> on_step, const void *owner);
    // The selection moved: the preview follows (when `owner` started it).
    void follow(const std::string &path, const void *owner);
    void close();
    const void *owner() const { return owner_; }

private:
    static void readable(int fd, void *data);
    void reap();
    PipeChild child_;
    std::string buf_, last_;
    std::function<void(const std::string &)> on_step_;
    const void *owner_ = nullptr;
};

class App {
public:
    static App &get();
    // `restore`: Flo started without a folder (Files' session restore).
    int run(const std::string &folder, const std::string &select, bool restore = false);

    EventQueue &queue() { return *queue_; }
    ThumbnailService &thumbnail_service() { return *thumb_service_; }
    ThumbImages &thumbs() { return *thumbs_; }
    // Folder sizes (Files' automatic sizes and Recalculate).
    FolderSizes &sizes() { return *sizes_; }
    Watcher &watcher() { return *watcher_; }
    Devices &devices() { return *devices_; }
    Network &network() { return *network_; }
    Settings &settings() { return settings_; }
    // Files' saved folder views (read once, at the first use).
    FolderViews &folder_views();
    // Files' starred items (Ctrl+E), read once at the first use.
    Starred &starred();
    // Saves this window's tabs as the session ("Save session"); the
    // window then keeps it saved when it closes.
    void save_session(MainWindow *w, bool quiet = false);
    // Folders just left by any window, for instant Back/Forward.
    ListingCache &listings() { return listings_; }
    const PlacesData &places() const { return places_; }
    PreviewClient &preview() { return preview_; }

    MainWindow *open_window(const std::string &folder, const std::string &select);
    void window_closed(MainWindow *w);
    const std::vector<MainWindow *> &windows() const { return windows_; }

    // Re-reads home folders, bookmarks and views (cheap, synchronous) and
    // tells every window.
    void refresh_places();
    void save_settings();
    // Gives freed memory back to the system once a burst of work (a folder
    // listed, thumbnails decoded and scaled) has settled.
    void trim_soon();

    // The file clipboard. Other apps get the paths as text; Flo windows get
    // the paths and whether they were cut.
    void copy_files(const std::vector<std::string> &paths, bool cut);
    void copy_text(const std::string &text);
    // Turns pasted clipboard text into paths (ours when it is what we put
    // there, so a cut stays a cut).
    std::vector<std::string> paths_from_clipboard(const std::string &text, bool &cut) const;
    bool clipboard_has_files() const { return !clip_paths_.empty(); }
    void clipboard_used_for_move(); // a cut was pasted: the clipboard empties

private:
    App() = default;
    void places_changed();
    void start_network_poll();
    static void network_tick(void *);
    std::unique_ptr<EventQueue> queue_;
    std::unique_ptr<ThumbnailService> thumb_service_;
    std::unique_ptr<ThumbImages> thumbs_;
    std::unique_ptr<FolderSizes> sizes_;
    std::unique_ptr<Watcher> watcher_;
    std::unique_ptr<Devices> devices_;
    std::unique_ptr<Network> network_;
    std::unique_ptr<DBusService> dbus_;
    Settings settings_;
    FolderViews folder_views_;
    bool folder_views_loaded_ = false;
    Starred starred_;
    bool starred_loaded_ = false;
    ListingCache listings_;
    PlacesData places_;
    PreviewClient preview_;
    std::vector<MainWindow *> windows_;
    std::vector<std::string> clip_paths_;
    bool clip_cut_ = false;
    std::string clip_text_;
    int bookmarks_watch_ = 0;
    bool devices_started_ = false, polling_ = false;
};

} // namespace flo::ui

#include "ui/app.h"
#include "ui/sizes.h"

#include "core/clipboard.h"
#include "core/fsutil.h"
#include "core/xdg.h"
#include "ui/main_window.h"
#include "ui/pane.h"
#include "ui/strings.h"
#include "ui/thumbs.h"

#include "fltk-omarchy/app.h"
#include "fltk-omarchy/dialog.h"
#include "fltk-omarchy/i18n.h"

#include <FL/Fl.H>

#include <malloc.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace flo::ui {

// --- PreviewClient ------------------------------------------------------------

PreviewClient::~PreviewClient() { close(); }

void PreviewClient::toggle(const std::string &path, bool show_hidden, const std::string &sort,
                           std::function<void(const std::string &)> on_step, const void *owner) {
    if (running()) {
        close();
        return;
    }
    if (path.empty())
        return;
    std::vector<std::string> argv = {"/proc/self/exe", "preview", path, "--sort", sort};
    if (show_hidden)
        argv.push_back("--hidden");
    // The real path of our executable, so the child is this same build.
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        argv[0] = exe;
    }
    ProcessLimits limits; // inherit, except no core dumps
    if (!spawn_pipe_child(argv, limits, child_))
        return;
    on_step_ = std::move(on_step);
    owner_ = owner;
    last_ = path;
    buf_.clear();
    Fl::add_fd(child_.from_child, FL_READ, readable, this);
}

void PreviewClient::follow(const std::string &path, const void *owner) {
    if (!running() || owner != owner_ || path == last_)
        return;
    last_ = path;
    write_to_child(child_, path + "\n");
}

void PreviewClient::readable(int fd, void *data) {
    auto *c = static_cast<PreviewClient *>(data);
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        c->reap(); // the preview closed
        return;
    }
    c->buf_.append(buf, size_t(n));
    size_t nl;
    while ((nl = c->buf_.find('\n')) != std::string::npos) {
        std::string line = c->buf_.substr(0, nl);
        c->buf_.erase(0, nl + 1);
        if (!line.empty() && line[0] == '/') {
            c->last_ = line;
            if (c->on_step_)
                c->on_step_(line);
        }
    }
}

void PreviewClient::reap() {
    if (child_.from_child >= 0)
        Fl::remove_fd(child_.from_child);
    if (child_.pid > 0) {
        int status = 0;
        // It said goodbye (EOF): wait briefly, then make sure.
        for (int i = 0; i < 50 && waitpid(child_.pid, &status, WNOHANG) == 0; ++i)
            usleep(2000);
    }
    kill_pipe_child(child_);
    child_ = PipeChild();
    owner_ = nullptr;
    on_step_ = nullptr;
}

void PreviewClient::close() {
    if (!running())
        return;
    // Closing its stdin asks it to quit; the group is killed regardless.
    if (child_.to_child >= 0)
        shutdown(child_.to_child, SHUT_WR);
    reap();
}

// --- App ------------------------------------------------------------------------

namespace {
Fl_Event_Dispatch g_prev_dispatch = nullptr;

// Tab (sidebar <-> panes) and the mouse's thumb buttons are the window's,
// before any widget sees them.
int flo_dispatch(int event, Fl_Window *w) {
    if (auto *mw = dynamic_cast<MainWindow *>(w))
        if (mw->intercept(event))
            return 1;
    return g_prev_dispatch ? g_prev_dispatch(event, w) : Fl::handle_(event, w);
}
} // namespace

App &App::get() {
    static App app;
    return app;
}

MainWindow *App::open_window(const std::string &folder, const std::string &select) {
    int w = std::max(720, settings_.window_width), h = std::max(460, settings_.window_height);
    auto *win = new MainWindow(w, h, folder.empty() ? xdg::home() : folder, select);
    // A second window cascades from the last one (tiling compositors place
    // windows themselves).
    if (!windows_.empty() && windows_.back()->shown())
        win->position(windows_.back()->x() + 32, windows_.back()->y() + 32);
    windows_.push_back(win);
    win->show();
    if (win->pane(0))
        win->pane(0)->focus_view();
    if (!polling_)
        start_network_poll();
    return win;
}

void App::window_closed(MainWindow *w) {
    auto it = std::find(windows_.begin(), windows_.end(), w);
    if (it == windows_.end())
        return;
    windows_.erase(it);
    save_settings();
    if (preview_.running()) {
        bool owned = false;
        for (int i = 0; i < 2; ++i)
            if (w->pane(i) && preview_.owner() == w->pane(i))
                owned = true;
        if (owned)
            preview_.close();
    }
    Fl::delete_widget(w);
}

void App::shortcuts_changed() {
    for (MainWindow *w : windows_)
        w->shortcuts_changed();
}

void App::save_settings() { settings_.save(); }

void App::trim_soon() {
    static auto trim = [](void *) { malloc_trim(0); };
    Fl::remove_timeout(trim, nullptr);
    Fl::add_timeout(1.5, trim, nullptr);
}

void App::refresh_places() {
    places_.home = xdg::home();
    places_.places = places::xdg_places();
    auto bookmarks = places::read_bookmarks();
    places_.pinned = places::pinned(bookmarks);
    places_.remote = places::remote_bookmarks(bookmarks);
    places_.views = places::views();
    if (!devices_started_)
        places_.drives = places::drives();
    places_changed();
}

void App::places_changed() {
    for (auto *w : windows_)
        w->rebuild_sidebar();
}

void App::start_network_poll() {
    polling_ = true;
    network_tick(this);
}

// The tailnet and Dropbox's state (for Share) are refreshed every 15 s
// while a window is open;
// nothing runs when no window is.
void App::network_tick(void *data) {
    auto *app = static_cast<App *>(data);
    if (app->windows_.empty()) {
        app->polling_ = false;
        return;
    }
    app->network_->refresh_tailnet([app](std::vector<TailnetPeer> peers) {
        bool same = peers.size() == app->places_.peers.size() &&
                    std::equal(peers.begin(), peers.end(), app->places_.peers.begin(),
                               [](const TailnetPeer &a, const TailnetPeer &b) { return a.host == b.host; });
        app->places_.peers = std::move(peers);
        if (!same)
            app->places_changed();
    });
    app->network_->refresh_dropbox([app](std::string status) { app->places_.dropbox = std::move(status); });
    app->refresh_shares();
    Fl::add_timeout(15.0, network_tick, app);
}

void App::refresh_shares() {
    network_->refresh_shares([this](std::vector<GvfsShare> shares) {
        if (shares == places_.shares)
            return;
        places_.shares = std::move(shares);
        places_changed();
    });
}

void App::network_job(int delta) {
    network_jobs_ = std::max(0, network_jobs_ + delta);
    places_changed();
}

void App::copy_files(const std::vector<std::string> &paths, bool cut) {
    clip_paths_ = paths;
    clip_cut_ = cut;
    clip_text_ = clipboard::plain_text(paths);
    Fl::copy(clip_text_.c_str(), int(clip_text_.size()), 1);
}

void App::copy_text(const std::string &text) {
    clip_paths_.clear();
    clip_cut_ = false;
    clip_text_ = text;
    Fl::copy(text.c_str(), int(text.size()), 1);
}

std::vector<std::string> App::paths_from_clipboard(const std::string &text, bool &cut) const {
    cut = false;
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == '\0'))
        t.pop_back();
    std::string ours = clip_text_;
    while (!ours.empty() && (ours.back() == '\n' || ours.back() == '\r'))
        ours.pop_back();
    if (!clip_paths_.empty() && t == ours) {
        cut = clip_cut_;
        return clip_paths_;
    }
    if (t.find("file://") != std::string::npos) {
        auto v = clipboard::parse_uri_list(t);
        if (!v.empty())
            return v;
    }
    return clipboard::parse_any("text/plain", t, cut);
}

void App::clipboard_used_for_move() {
    // A cut pasted once is gone; the text stays for other apps.
    clip_cut_ = false;
    clip_paths_.clear();
}

void tune_malloc() {
    // A fixed mmap threshold: glibc raises it each time a large block is
    // freed, after which every decoded thumbnail (a 256 px PNG is ~150 KB)
    // comes from the heap and leaves holes malloc_trim can't give back.
    // Fixed at 64 KB they are mapped and unmapped whole: ~0.7 MiB less PSS
    // after scrolling through a folder of photos.
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
}

FolderViews &App::folder_views() {
    if (!folder_views_loaded_) {
        folder_views_loaded_ = true;
        folder_views_.load();
    }
    return folder_views_;
}

Starred &App::starred() {
    if (!starred_loaded_) {
        starred_loaded_ = true;
        starred_.load();
    }
    return starred_;
}

void App::save_session(MainWindow *w, bool quiet) {
    if (!w)
        return;
    for (MainWindow *o : windows_)
        o->session_owner(o == w);
    Session s = w->session_state();
    bool ok = s.save();
    if (!quiet)
        fo::Toast::show(w, ok ? fo::tr("Session saved") : fo::tr("The session can't be saved."), 2.5,
                        ok ? fo::Toast::Info : fo::Toast::Error);
}

int App::run(const std::string &folder, const std::string &select, bool restore) {
    tune_malloc();
    settings_ = Settings::load();
    register_catalogs();
    fo::InitOptions opts;
    opts.app_class = "flo";
    fo::init(opts);
    g_prev_dispatch = Fl::event_dispatch();
    Fl::event_dispatch(flo_dispatch);

    queue_ = std::make_unique<EventQueue>();
    Fl::add_fd(queue_->fd(), FL_READ, [](int, void *q) { static_cast<EventQueue *>(q)->dispatch(); }, queue_.get());
    thumb_service_ = std::make_unique<ThumbnailService>(*queue_);
    thumbs_ = std::make_unique<ThumbImages>(*thumb_service_);
    sizes_ = std::make_unique<FolderSizes>(*queue_);
    sizes_->on_result([this](const std::string &) {
        for (MainWindow *w : windows_)
            w->sizes_changed();
    });
    watcher_ = std::make_unique<Watcher>(*queue_);
    devices_ = std::make_unique<Devices>(*queue_);
    network_ = std::make_unique<Network>(*queue_);

    places_.home = xdg::home();
    refresh_places();
    bookmarks_watch_ = watcher_->watch_file(places::bookmarks_path(), [this] { refresh_places(); });

    MainWindow *first = open_window(folder, select);
    if (restore) {
        // Files restores the last session when it starts without a folder;
        // this window keeps it saved when it closes.
        first->session_owner(true);
        Session s;
        if (Session::load(s))
            first->restore_session(s);
    }
    // Omarchy's language picked live: every window relabels (Folio does too).
    fo::i18n::on_change([this] {
        for (MainWindow *w : windows_)
            w->language_changed();
    });

    // Drives: GIO's volume monitor, started once the window is up.
    Fl::add_timeout(0.5, [](void *d) {
        auto *app = static_cast<App *>(d);
        if (std::getenv("FLO_NO_VOLUME_MONITOR"))
            return;
        app->devices_started_ = true;
        app->devices_->watch([app](std::vector<Place> drives) {
            app->places_.drives = std::move(drives);
            app->places_changed();
        });
    }, this);

    // Single instance and "Show in folder" from other apps.
    DBusHandlers h;
    h.open_window = [this](const std::string &f, const std::string &s) { open_window(f, s); };
    h.show_folders = [this](const std::vector<std::string> &paths) {
        if (!paths.empty())
            open_window(paths[0], {});
    };
    h.show_items = [this](const std::vector<std::string> &paths) {
        for (const auto &p : paths)
            open_window(path_dirname(p), path_basename(p));
    };
    h.show_item_properties = h.show_items;
    dbus_ = std::make_unique<DBusService>(*queue_, h);
    if (!std::getenv("FLO_NO_DBUS"))
        dbus_->start(true);

    int rc = Fl::run();
    preview_.close();
    save_settings();
    // Services stop before the queue goes.
    dbus_.reset();
    sizes_.reset();
    thumbs_.reset();
    thumb_service_.reset();
    devices_.reset();
    network_.reset();
    watcher_.reset();
    Fl::remove_fd(queue_->fd());
    return rc;
}

} // namespace flo::ui

int run_gui(const std::string &folder, const std::string &select, bool restore) {
    signal(SIGPIPE, SIG_IGN);
    return flo::ui::App::get().run(folder, select, restore);
}

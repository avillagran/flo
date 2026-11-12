#include "ui/pane.h"

#include "core/folder_views.h"
#include "core/fsutil.h"
#include "core/name_filter.h"
#include "core/tree_size.h"
#include "core/network.h"
#include "core/places.h"
#include "core/recents.h"
#include "core/shortcuts.h"
#include "core/starred.h"
#include "core/trash.h"
#include "core/uri.h"
#include "core/xdg.h"
#include "ui/app.h"
#include "ui/properties.h"
#include "ui/columns.h"
#include "ui/file_view.h"
#include "ui/main_window.h"
#include "ui/sizes.h"
#include "ui/space_view.h"

#include "fltk-omarchy/dialog.h"
#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/menu.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/process.h"
#include "fltk-omarchy/shell.h"
#include "fltk-omarchy/text.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <unistd.h>

namespace flo::ui {

namespace {

const int kBanner = 40, kTrashBar = 48, kDualHeader = 28, kListHeader = 30, kDualFooter = 24, kGridHeader = 36;

std::string quoted(const std::string &s) { return "\xE2\x80\x9C" + s + "\xE2\x80\x9D"; }

const SortMode kSorts[] = {SortMode::Name, SortMode::Kind, SortMode::Newest, SortMode::Oldest, SortMode::Size};

} // namespace

// ===========================================================================
// Pane

Pane::Pane(MainWindow *win, int x, int y, int w, int h, const std::string &folder, View view)
    : Fl_Group(x, y, w, h), win_(win), view_(view) {
    box(FL_NO_BOX);
    App &app = App::get();
    hidden_ = app.settings().show_hidden;
    sort_ = app.settings().sort;
    dismiss_ = new fo::IconButton(0, 0, 32, 32, "x", "Dismiss");
    dismiss_->hide();
    cancel_ = new fo::Button(0, 0, 88, 28, nullptr);
    cancel_->hide();
    empty_trash_ = new fo::Button(0, 0, 140, 32, nullptr);
    empty_trash_->hide();
    pause_ = new fo::Button(0, 0, 80, 27, nullptr);
    pause_->hide();
    cancel_job_ = new fo::Button(0, 0, 120, 27, nullptr);
    cancel_job_->hide();
    clear_pending_ = new fo::Button(0, 0, 138, 27, nullptr);
    clear_pending_->hide();
    list_header_ = new fo::SortHeader(x, y, w, kListHeader);
    grid_ = new FileView(x, y, w, h, &model_);
    columns_ = new ColumnsView(x, y, w, h, this);
    space_ = new SpaceView(x, y, w, h, this);
    end();
    resizable(nullptr);
    wire(grid_);
    wire(columns_->here());
    grid_->on_new_folder([this] { new_folder(); });
    grid_->on_zoom([this](double f) { zoom_by(f); });
    list_header_->on_sort([this](int c) { sort_clicked(c); });
    dismiss_->on_click([this] { dismiss_message(); });
    cancel_->on_click([this] {
        if (ops_ && progress_on_)
            ops_->cancel(progress_.op);
    });
    empty_trash_->on_click([this] { empty_trash(); });
    pause_->on_click([this] {
        if (!ops_ || !progress_on_)
            return;
        ops_->pause(progress_.op, !ops_->paused(progress_.op));
        relabel_card();
        redraw();
    });
    cancel_job_->on_click([this] {
        if (ops_ && progress_on_)
            ops_->cancel(progress_.op);
    });
    clear_pending_->on_click([this] {
        if (!ops_)
            return;
        size_t n = ops_->clear_pending();
        fo::Toast::show(win_, fo::trn("%n pending operations cleared", int(n)), 2.5);
        redraw();
    });

    lister_ = std::make_unique<Lister>(app.queue());
    searcher_ = std::make_unique<Searcher>(app.queue());
    FileOpsCallbacks cb;
    cb.on_progress = [this](const OpProgress &p) { op_progress(p); };
    cb.on_conflict = [this](const OpConflict &c) { op_conflict(c); };
    cb.on_done = [this](const OpResult &r) { op_done(r); };
    cb.on_history = [this](const std::string &u, const std::string &r) {
        undo_label_ = u;
        redo_label_ = r;
    };
    ops_ = std::make_unique<FileOps>(app.queue(), cb);

    relabel();
    set_view(view);
    navigate(folder.empty() ? xdg::home() : folder, {}, false);
}

void Pane::relabel() {
    dismiss_->tooltip_text(fo::tr("Dismiss"));
    cancel_->copy_label(fo::tr("Cancel"));
    empty_trash_->copy_label(fo::tr("Empty Trash"));
    relabel_card();
    if (!folder_.empty()) {
        build_crumbs(); // Recents, Trash, Home
        win_->pane_view_changed(this);
    }
    list_header_->columns(list_header_columns());
    files()->empty_text(empty_label());
    redraw();
}

Pane::~Pane() {
    remember_listing();
    unwatch();
    if (App::get().preview().owner() == this)
        App::get().preview().close();
    Fl::remove_timeout(nullptr, this);
}

void Pane::wire(FileView *v) {
    v->on_activate([this](int i) { open_index(i); });
    v->on_selection_change([this] { selection_changed(); });
    v->on_context([this](int i, int ex, int ey) { context_menu(i, ex, ey); });
    v->on_drag_start([this] { start_drag(); });
    v->accepts_drop([this](int i) {
        if (in_recents())
            return false;
        if (i < 0)
            return true;
        return i < model_.count() && model_.entries[size_t(i)].is_dir;
    });
    v->on_drop([this](int i, const std::string &text) {
        bool cut = false;
        auto paths = App::get().paths_from_clipboard(text, cut);
        drop_into(paths, i < 0 ? folder_ : model_.path(i));
    });
    v->on_key([this, v](int k) {
        int st = Fl::event_state();
        if (st & (FL_CTRL | FL_ALT | FL_META))
            return false;
        const char *t = Fl::event_text();
        // Files: typing filters the folder (the header shows the filter).
        if (t && Fl::event_length() > 0 && static_cast<unsigned char>(t[0]) > 0x20 && t[0] != 0x7f &&
            k != FL_BackSpace && k != FL_Delete && k != FL_Escape && k != FL_Enter && k != FL_KP_Enter) {
            if (!in_recents() && v->mode() != FileView::Mode::Column)
                win_->begin_filter(std::string(t, size_t(Fl::event_length())));
            return true;
        }
        if ((v->layout() == fo::Collection::Layout::List || v->layout() == fo::Collection::Layout::Bands) &&
            (k == FL_Right || k == FL_Left) && v->mode() != FileView::Mode::Column) {
            if (k == FL_Left) {
                go_up();
            } else if (const Entry *e = current_entry(); e && e->is_dir) {
                navigate(current_path());
            }
            return true;
        }
        return false;
    });
}

FileView *Pane::files() const { return view_ == View::Columns && !dual_ ? columns_->here() : grid_; }

bool Pane::can_go_up() const {
    if (in_recents() || folder_ == "/")
        return false;
    return !(!crumbs_.empty() && crumbs_.front().second == folder_ && folder_ != xdg::home());
}

std::vector<fo::SortHeader::Column> Pane::list_header_columns() const {
    return {{fo::tr("Name"), "sort"}, {fo::tr("Size"), "database"}, {fo::tr("Date"), "clock"}, {fo::tr("Type"), "file"}};
}

std::string Pane::empty_label() const {
    if (!query_.empty() || !filter_.empty())
        return fo::tr("No matches");
    if (in_starred())
        return fo::tr("No starred items \xC2\xB7 Ctrl+E stars the focused item in any folder");
    if (in_recents())
        return fo::tr("No recent files");
    return fo::tr("Empty");
}

std::string Pane::title() const {
    if (in_starred())
        return fo::tr("Starred");
    if (in_recents())
        return fo::tr("Recents");
    if (folder_ == trash::home_trash() + "/files")
        return fo::tr("Trash");
    if (folder_ == xdg::home())
        return fo::tr("Home");
    return crumbs_.empty() ? folder_ : crumbs_.back().first;
}

// --- Views -----------------------------------------------------------------

Pane::View Pane::from_mode(ViewMode m) {
    switch (m) {
    case ViewMode::Grid: return View::Grid;
    case ViewMode::List: return View::List;
    case ViewMode::Columns: return View::Columns;
    case ViewMode::Space: return View::Space;
    case ViewMode::Groups: return View::Groups;
    }
    return View::Groups;
}

ViewMode Pane::to_mode(View v) {
    switch (v) {
    case View::Grid: return ViewMode::Grid;
    case View::List: return ViewMode::List;
    case View::Columns: return ViewMode::Columns;
    case View::Space: return ViewMode::Space;
    case View::Groups: return ViewMode::Groups;
    }
    return ViewMode::Groups;
}

void Pane::set_view(View v, bool remember) {
    FileView *old = files();
    std::vector<std::string> sel_paths;
    for (int i : old->selection())
        sel_paths.push_back(model_.path(i));
    std::string cur_path = model_.path(old->current());
    // Hiding the focused view hands the keyboard to some other widget, so
    // whether the files had it is decided before.
    const bool had_keyboard = !Fl::focus() || Fl::focus() == old || Fl::focus() == space_ ||
                              win_->remembered_focus() == old || win_->remembered_focus() == space_;
    bool reorder = (view_ == View::Groups) != (v == View::Groups);
    view_ = v;
    if (reorder)
        order_for_view();
    FileView *now = files();
    if (now != old || reorder) {
        now->reload(true);
        now->set_current(-1, true);
        for (const auto &p : sel_paths) {
            int at = model_.index_of_path(p);
            if (at >= 0)
                now->select(at, true);
        }
        int c = model_.index_of_path(cur_path);
        if (c >= 0)
            now->set_current(c, false);
    }
    if (v == View::Columns && !dual_)
        columns_->sync();
    if (v == View::Space && !dual_)
        space_->sync();
    if (remember && !dual_ && win_->index_of(this) == 0) {
        App::get().settings().view = to_mode(v);
        App::get().save_settings();
    }
    place_children();
    win_->pane_view_changed(this);
    if (had_keyboard)
        focus_view();
}

void Pane::dual(bool on) {
    dual_ = on;
    place_children();
    if (on && view_ == View::Groups) {
        // The dual view lists rows: the model goes back to the plain sort.
        View keep = view_;
        view_ = View::List;
        order_for_view();
        view_ = keep;
        files()->reload(false);
    } else if (!on) {
        order_for_view();
        files()->reload(false);
        if (view_ == View::Columns)
            columns_->sync();
        if (view_ == View::Space)
            space_->sync();
    }
    redraw();
}

// Files' Groups order: the folders, then the files by age section (the
// sort holds within each part); every other view keeps the plain sort.
void Pane::order_for_view() {
    if (query_.size() || in_recents()) {
        int n = model_.count();
        int ends[5] = {0, n, n, n, n};
        grid_->groups(ends);
        return;
    }
    sort_entries(model_.entries, sort_);
    int folders = 0;
    while (folders < model_.count() && model_.entries[size_t(folders)].is_dir)
        ++folders;
    if (view_ != View::Groups || dual_) {
        int n = model_.count();
        int ends[5] = {folders, n, n, n, n};
        grid_->groups(ends);
        return;
    }
    int64_t now = int64_t(std::time(nullptr));
    std::stable_sort(model_.entries.begin() + folders, model_.entries.end(), [now](const Entry &a, const Entry &b) {
        return int(age_section(a.mtime_ns, now)) < int(age_section(b.mtime_ns, now));
    });
    int ends[5];
    ends[0] = folders;
    int i = folders;
    for (int sec = 0; sec < 4; ++sec) {
        while (i < model_.count() && int(age_section(model_.entries[size_t(i)].mtime_ns, now)) == sec)
            ++i;
        ends[sec + 1] = i;
    }
    grid_->groups(ends);
}

// --- Going places ------------------------------------------------------------

void Pane::navigate(const std::string &path, const std::string &select, bool history) {
    if (path.empty())
        return;
    std::string p = is_virtual_folder(path) ? path : clean_path(path);
    if (!is_virtual_folder(p) && !is_dir(p)) {
        show_message(fo::i18n::arg(fo::tr("%1 can't be opened."), quoted(tilde(p))));
        return;
    }
    if (history && !folder_.empty() && p != folder_) {
        back_.push_back(folder_);
        if (back_.size() > 100)
            back_.erase(back_.begin());
        forward_.clear();
    }
    if (searcher_)
        searcher_->cancel();
    query_.clear();
    filter_.clear();
    all_.clear();
    if (win_->active_pane() == this)
        win_->end_field(false);
    App::get().sizes().cancel();
    message_.clear();
    if (p != folder_)
        remember_listing();
    pending_select_ = select;
    keep_paths_.clear();
    keep_current_.clear();
    folder_ = p;
    // Files' saved folder view: the view (not in the dual pane), the sort
    // and the zoom; folders without one keep what the pane shows.
    if (const FolderView *fv = App::get().folder_views().get(p)) {
        if (!dual_)
            view_ = from_mode(fv->view);
        sort_ = fv->sort;
        grid_zoom_ = std::clamp(fv->zoom_percent / 100.0, kZoomMin, kZoomMax);
        grid_->zoom(grid_zoom_);
        list_header_->columns(list_header_columns());
    }
    model_.base = p;
    model_.entries.clear();
    model_.peeks.clear();
    model_.error.clear();
    model_.loading = true;
    const bool recalled = recall_listing();
    files()->reload(true);
    files()->set_current(-1, true);
    if (recalled && !pending_select_.empty()) {
        int at = model_.index_of_name(pending_select_);
        if (at >= 0)
            files()->set_current(at, true);
    }
    build_crumbs();
    after_folder_change();
    list_now(false);
    watch();
}

void Pane::after_folder_change() {
    if (view_ == View::Columns && !dual_)
        columns_->sync();
    if (view_ == View::Space && !dual_)
        space_->sync();
    files()->empty_text(empty_label());
    // Recents and Starred show each item's folder beside its name.
    files()->show_folders(in_recents());
    place_children();
    win_->pane_moved(this);
    redraw();
}

void Pane::build_crumbs() {
    crumbs_.clear();
    if (in_recents()) {
        crumbs_.push_back({title(), folder_});
        return;
    }
    const std::string trash_files = trash::home_trash() + "/files";
    if (path_inside(folder_, trash_files)) {
        crumbs_.push_back({fo::tr("Trash"), trash_files});
        std::string rest = folder_.substr(trash_files.size());
        std::string acc = trash_files;
        size_t i = 1;
        while (i < rest.size()) {
            size_t j = rest.find('/', i);
            std::string part = rest.substr(i, j == std::string::npos ? std::string::npos : j - i);
            acc += "/" + part;
            crumbs_.push_back({part, acc});
            if (j == std::string::npos)
                break;
            i = j + 1;
        }
        return;
    }
    const std::string home = xdg::home(), gvfs = gvfs_root();
    std::string root = "/", root_label = "/";
    const Place *drive = nullptr;
    for (const auto &d : App::get().places().drives)
        if (d.mounted && !d.path.empty() && d.path != "/" && path_inside(folder_, d.path) &&
            (!drive || d.path.size() > drive->path.size()))
            drive = &d;
    if (drive && !path_inside(drive->path, home)) {
        // A drive is its own root, named as the sidebar names it.
        root = drive->path;
        root_label = drive->name;
    } else if (!gvfs.empty() && path_inside(folder_, gvfs) && folder_ != gvfs) {
        std::string rest = folder_.substr(gvfs.size() + 1);
        std::string mount = rest.substr(0, rest.find('/'));
        root = gvfs + "/" + mount;
        auto host = mount.find("host=");
        root_label = host == std::string::npos ? mount : mount.substr(host + 5);
        root_label = root_label.substr(0, root_label.find_first_of(".,"));
    }
    // Files' chips: every folder from the root, the home folder named Home.
    crumbs_.push_back({root == home ? fo::tr("Home") : root_label, root});
    std::string rest = folder_.substr(root.size());
    std::string acc = root == "/" ? "" : root;
    size_t i = rest.empty() ? 0 : (rest[0] == '/' ? 1 : 0);
    while (i < rest.size()) {
        size_t j = rest.find('/', i);
        std::string part = rest.substr(i, j == std::string::npos ? std::string::npos : j - i);
        acc += "/" + part;
        if (!part.empty())
            crumbs_.push_back({acc == home ? fo::tr("Home") : part, acc});
        if (j == std::string::npos)
            break;
        i = j + 1;
    }
}

void Pane::list_now(bool keep) {
    if (in_recents()) {
        auto poster = App::get().queue().poster();
        std::weak_ptr<char> weak = alive_.weak();
        bool hidden = hidden_;
        Pane *self = this;
        std::string which = folder_;
        // Starred: newest star first. As in Files, an item that is gone stays
        // listed (unavailable: mode 0) so it can still be taken off the list.
        std::vector<std::string> stars;
        if (in_starred())
            stars.assign(App::get().starred().items().rbegin(), App::get().starred().items().rend());
        recents_exec_.clear();
        recents_exec_.run([poster, weak, hidden, self, keep, which, stars] {
            std::shared_ptr<Listing> l;
            if (which == kStarred) {
                l = std::make_shared<Listing>();
                l->folder = kStarred;
                EntryOptions opt;
                opt.sniff = false;
                for (const auto &p : stars) {
                    Entry e;
                    if (!stat_path(p, e, opt))
                        e = Entry();
                    e.name = p; // the list spans folders: the entry carries its full path
                    l->entries.push_back(std::move(e));
                }
            } else {
                l = std::make_shared<Listing>(recents::list(recents::xbel_path(), hidden));
            }
            poster.post([weak, l, self, keep, which] {
                if (weak.expired() || self->folder() != which)
                    return;
                self->landed(l, keep);
            });
        });
        return;
    }
    ListOptions o;
    o.show_hidden = hidden_;
    o.sort = sort_;
    lister_->list(folder_, o, [this, keep](ListingPtr l) { landed(l, keep); });
}

void Pane::landed(ListingPtr l, bool keep) {
    if (!query_.empty())
        return; // search results are showing
    FileView *v = files();
    std::string cur_before = keep ? keep_current_ : std::string();
    int cur_row = v->current();
    bool same = false;
    if (showing_cached_) {
        // A remembered listing stays up until the re-read is complete, and
        // stays as it is (with its counts and peeks) when nothing changed.
        if (!l->complete)
            return;
        showing_cached_ = false;
        same = l->error.empty() && same_listing(model_.entries, l->entries);
    }
    if (!same) {
        if (!filter_.empty()) {
            all_ = l->entries;
            NameFilter f(filter_);
            model_.entries.clear();
            for (const auto &e : all_)
                if (f.matches(e))
                    model_.entries.push_back(e);
        } else {
            model_.entries = l->entries;
        }
        order_for_view();
    }
    model_.error = l->error;
    model_.loading = !l->complete;
    if (!same)
        v->reload(false);
    if ((!restore_marks_.empty() || !restore_current_.empty()) && l->complete) {
        v->clear_selection();
        int first = -1;
        for (const auto &n : restore_marks_) {
            int at = model_.index_of_name(n);
            if (at >= 0) {
                v->select(at, true);
                if (first < 0)
                    first = at;
            }
        }
        int c = model_.index_of_name(restore_current_);
        if (c >= 0)
            v->set_current(c, restore_marks_.empty());
        else if (first >= 0)
            v->set_current(first, false);
        else
            v->set_current(model_.count() > 0 ? 0 : -1, true);
        if (c >= 0 && !restore_marks_.empty())
            for (const auto &n : restore_marks_)
                if (int at = model_.index_of_name(n); at >= 0)
                    v->select(at, true);
        restore_marks_.clear();
        restore_current_.clear();
        pending_select_.clear();
    } else if (!pending_select_.empty()) {
        int at = model_.index_of_name(pending_select_);
        if (at < 0)
            at = model_.index_of_path(pending_select_);
        if (at >= 0) {
            v->set_current(at, true);
            if (l->complete)
                pending_select_.clear();
        } else {
            v->clear_selection();
        }
    } else if (keep) {
        v->clear_selection();
        int first = -1;
        for (const auto &p : keep_paths_) {
            int at = model_.index_of_path(p);
            if (at >= 0) {
                v->select(at, true);
                if (first < 0)
                    first = at;
            }
        }
        int c = model_.index_of_path(cur_before);
        if (c >= 0)
            v->set_current(c, false);
        else if (!keep_current_.empty() && cur_row >= 0 && model_.count() > 0)
            v->set_current(std::min(cur_row, model_.count() - 1), true); // the item went: the next one
    } else if (l->complete) {
        // Files focuses (and marks) the first entry of a folder it arrives in.
        v->set_current(model_.count() > 0 ? 0 : -1, true);
    }
    if (l->complete) {
        keep_paths_.clear();
        keep_current_.clear();
        count_folders();
        request_sizes();
        App::get().trim_soon();
    }
    if (view_ == View::Columns && !dual_)
        columns_->selection_changed();
    if (view_ == View::Space && !dual_)
        space_->sync();
    place_children();
    selection_changed();
    redraw();
}

void Pane::remember_listing() {
    // A remembered listing still being re-read is as good as when it was kept.
    const bool complete = !model_.loading || showing_cached_;
    showing_cached_ = false;
    if (folder_.empty() || in_recents() || !query_.empty() || !complete || !model_.error.empty() ||
        model_.entries.empty())
        return;
    CachedListing l;
    l.entries = filter_.empty() ? std::move(model_.entries) : std::move(all_);
    l.peeks = std::move(model_.peeks);
    model_.entries.clear();
    all_.clear();
    model_.peeks.clear();
    App::get().listings().put(ListingCache::key(folder_, hidden_, sort_), std::move(l));
}

bool Pane::recall_listing() {
    showing_cached_ = false;
    if (in_recents())
        return false;
    CachedListing l;
    if (!App::get().listings().take(ListingCache::key(folder_, hidden_, sort_), l))
        return false;
    model_.entries = std::move(l.entries);
    model_.peeks = std::move(l.peeks);
    showing_cached_ = true;
    order_for_view();
    static const bool trace = std::getenv("FLO_TRACE_LISTINGS") != nullptr;
    if (trace)
        std::fprintf(stderr, "flo: recalled %s (%d entries, %zu remembered listings)\n", folder_.c_str(),
                     model_.count(), App::get().listings().listings());
    return true;
}

void Pane::count_folders() {
    std::vector<std::string> folders;
    for (int i = 0; i < model_.count(); ++i)
        if (model_.entries[size_t(i)].is_dir && model_.entries[size_t(i)].item_count < 0)
            folders.push_back(model_.path(i));
    if (folders.empty() || in_recents())
        return;
    lister_->count_items(
        folders, hidden_,
        [this](std::vector<FolderSummary> batch) {
            for (auto &b : batch) {
                if (!filter_.empty())
                    for (auto &e : all_)
                        if (path_join(folder_, e.name) == b.folder)
                            e.item_count = b.count;
                int at = model_.index_of_path(b.folder);
                if (at < 0)
                    continue;
                model_.entries[size_t(at)].item_count = b.count;
                if (b.peek.empty())
                    model_.peeks.erase(b.folder);
                else
                    model_.peeks[b.folder] = std::move(b.peek);
            }
            files()->redraw();
        },
        2);
}

void Pane::watch() {
    unwatch();
    auto again = [this] {
        if (!query_.empty())
            return;
        reload(true);
    };
    if (in_recents())
        watch_id_ = App::get().watcher().watch_file(recents::xbel_path(), again);
    else
        watch_id_ = App::get().watcher().watch_folder(folder_, again);
}

void Pane::unwatch() {
    if (watch_id_)
        App::get().watcher().unwatch(watch_id_);
    watch_id_ = 0;
}

void Pane::reload(bool keep) {
    if (keep) {
        FileView *v = files();
        keep_paths_.clear();
        for (int i : v->selection())
            keep_paths_.push_back(model_.path(i));
        keep_current_ = model_.path(v->current());
    }
    list_now(keep);
}

void Pane::go_up() {
    if (in_recents() || folder_ == "/")
        return;
    if (!crumbs_.empty() && crumbs_.front().second == folder_ && folder_ != xdg::home())
        return; // a trash or a mount root: nothing above to show
    navigate(path_dirname(folder_), path_basename(folder_));
}

void Pane::back() {
    if (back_.empty())
        return;
    std::string to = back_.back();
    back_.pop_back();
    forward_.push_back(folder_);
    std::string select = path_dirname(folder_) == to ? path_basename(folder_) : std::string();
    navigate(to, select, false);
}

void Pane::forward() {
    if (forward_.empty())
        return;
    std::string to = forward_.back();
    forward_.pop_back();
    back_.push_back(folder_);
    navigate(to, {}, false);
}

void Pane::go_home() { navigate(xdg::home()); }

// Files' omnibar starts from the absolute path, all of it selected.
std::string Pane::typed_path() const { return in_recents() ? std::string() : folder_; }

bool Pane::go_typed(const std::string &text) {
    if (is_network_uri(text)) {
        // Files' parse(): a bad address stays in the field to be fixed.
        NetworkLocation loc;
        std::string error = parse_network_uri(text, loc);
        if (!error.empty()) {
            fo::Toast::show(win_, fo::i18n::arg(fo::tr("Path: %1"), fo::tr(error.c_str())), 4, fo::Toast::Error);
            return false;
        }
        open_uri(loc.uri, loc.inline_password);
        return true;
    }
    if (!uri_scheme(text).empty() && uri_scheme(text) != "file") {
        open_uri(text);
        return true;
    }
    std::string p = expand_typed(text, folder_);
    if (p.empty())
        return false;
    if (is_dir(p)) {
        navigate(p);
        return true;
    }
    if (exists_nofollow(p)) {
        show_item(p);
        return true;
    }
    fo::Toast::show(win_, fo::i18n::arg(fo::tr("There's no folder %1"), quoted(tilde(p))), 3, fo::Toast::Error);
    return false;
}

void Pane::open_index(int i) {
    if (i < 0 || i >= model_.count())
        return;
    open_path(model_.path(i));
}

void Pane::open_path(const std::string &path) {
    if (path.empty())
        return;
    if (is_dir(path)) {
        navigate(path);
        return;
    }
    if (in_trash_folder(path)) {
        fo::Toast::show(win_, fo::tr("Restore it first to open it"), 3);
        return;
    }
    open_default(path);
    Entry e;
    std::string mime = stat_path(path, e, EntryOptions{false}) ? e.mime : "";
    fo::tasks::run([path, mime] { recents::add(path, mime); });
}

void Pane::show_item(const std::string &path) {
    std::string dir = path_dirname(path);
    if (dir == folder_) {
        select_path(path);
        return;
    }
    navigate(dir, path_basename(path));
}

void Pane::open_peer(const std::string &host) {
    password_given_ = false;
    inline_password_.clear();
    busy_ = fo::i18n::arg(fo::tr("Connecting to %1\xE2\x80\xA6"), host.substr(0, host.find('.')));
    place_children();
    redraw();
    if (!mounter_)
        mounter_ = std::make_unique<Mounter>(App::get().queue());
    MountCallbacks cb;
    cb.on_request = [this](const MountRequest &r) { ask_mount(r); };
    cb.on_mounted = [this](const std::string &local, const std::string &landing) {
        busy_.clear();
        App::get().network_job(-1);
        App::get().refresh_shares();
        navigate(landing.empty() ? local : landing);
    };
    cb.on_failed = [this](const std::string &error) {
        busy_.clear();
        App::get().network_job(-1);
        if (!error.empty())
            show_message(error);
        place_children();
        redraw();
    };
    if (!mounter_->mount(sftp_uri(host), App::get().network().is_tailnet_host(host), cb)) {
        busy_.clear();
        fo::Toast::show(win_, fo::tr("Still connecting to another machine"), 3);
        return;
    }
    App::get().network_job(+1);
}

void Pane::open_uri(const std::string &uri, const std::string &password) {
    password_given_ = false;
    inline_password_ = password;
    busy_ = fo::i18n::arg(fo::tr("Connecting to %1\xE2\x80\xA6"), uri);
    place_children();
    redraw();
    if (!mounter_)
        mounter_ = std::make_unique<Mounter>(App::get().queue());
    MountCallbacks cb;
    cb.on_request = [this](const MountRequest &r) { ask_mount(r); };
    cb.on_mounted = [this, uri](const std::string &local, const std::string &landing) {
        busy_.clear();
        inline_password_.clear();
        App::get().network_job(-1);
        App::get().refresh_shares();
        navigate(landing.empty() ? local : landing);
        fo::Toast::show(win_, fo::i18n::arg(fo::tr("Connected to %1"), uri), 3);
    };
    cb.on_failed = [this, uri](const std::string &error) {
        busy_.clear();
        inline_password_.clear();
        App::get().network_job(-1);
        if (!error.empty())
            show_message(uri + ": " + error);
        place_children();
        redraw();
    };
    if (!mounter_->mount(uri, false, cb)) {
        busy_.clear();
        inline_password_.clear();
        // Files: one network job at a time.
        fo::Toast::show(win_, fo::tr("Wait for the network connection in progress"), 3);
        return;
    }
    App::get().network_job(+1);
}

// GVfs's questions come in paragraphs. The dialog wraps each one; this only
// tidies spacing: tabs become spaces, runs of spaces and of blank lines
// shrink to one, and the ends are trimmed.
static std::string tidy_paragraphs(const std::string &text) {
    std::string out;
    for (char c : text) {
        if (c == '\r')
            continue;
        if (c == '\t')
            c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n'))
            continue;
        if (c == '\n') {
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            if (out.empty() || (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n'))
                continue;
        }
        out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n'))
        out.pop_back();
    return out;
}

void Pane::ask_mount(const MountRequest &r) {
    fo::DialogSpec s;
    s.scrim_cancels = false;
    s.message = tidy_paragraphs(r.message);
    uint64_t id = r.id;
    if (r.kind == MountRequest::Password && !inline_password_.empty() && !password_given_) {
        // The password typed in the URI answers the first request once.
        std::string pw;
        pw.swap(inline_password_);
        password_given_ = true;
        if (mounter_)
            mounter_->reply_password(id, r.user, pw, r.domain);
        return;
    }
    if (r.kind == MountRequest::Password) {
        s.title = fo::tr("Sign in");
        // Asked again after an answer: the password didn't work.
        if (password_given_)
            s.message = std::string(fo::tr("That password didn't work. Try again.")) + " " + s.message;
        password_given_ = false;
        bool user = r.needs_user, domain = r.needs_domain;
        if (user)
            s.fields.push_back({fo::tr("User"), r.user});
        if (domain)
            s.fields.push_back({fo::tr("Domain"), r.domain});
        fo::DialogField pw;
        pw.placeholder = fo::tr("Password");
        pw.secret = true;
        s.fields.push_back(pw);
        s.focus_field = int(s.fields.size()) - 1;
        s.buttons = {fo::tr("Cancel"), fo::tr("Connect")};
        std::string default_user = r.user, default_domain = r.domain;
        fo::Dialog::ask(win_, s, [this, id, user, domain, default_user, default_domain](const fo::DialogAnswer &a) {
            if (!mounter_)
                return;
            if (a.button != 1) {
                mounter_->abort(id);
                return;
            }
            size_t k = 0;
            std::string u = user ? a.values[k++] : default_user;
            std::string d = domain ? a.values[k++] : default_domain;
            password_given_ = true;
            mounter_->reply_password(id, u, a.values[k], d);
        });
    } else {
        s.title = fo::tr("Connect");
        s.buttons = r.choices.empty() ? std::vector<std::string>{fo::tr("OK")} : r.choices;
        fo::Dialog::ask(win_, s, [this, id](const fo::DialogAnswer &a) {
            if (!mounter_)
                return;
            if (a.button < 0)
                mounter_->abort(id);
            else
                mounter_->reply_choice(id, a.button);
        });
    }
}

void Pane::mount_volume(const std::string &id) {
    busy_ = fo::tr("Mounting\xE2\x80\xA6");
    place_children();
    App::get().devices().mount(id, [this](bool ok, const std::string &path, const std::string &error) {
        busy_.clear();
        if (ok && !path.empty())
            navigate(path);
        else
            show_message(error.empty() ? fo::tr("The drive couldn't be mounted.") : error);
        place_children();
        redraw();
    });
}

// --- Selection ------------------------------------------------------------------

int Pane::current() const { return files()->current(); }

const Entry *Pane::current_entry() const {
    int i = current();
    return i >= 0 && i < model_.count() ? &model_.entries[size_t(i)] : nullptr;
}

std::string Pane::current_path() const { return model_.path(current()); }

std::vector<std::string> Pane::selected_paths() const {
    std::vector<std::string> out;
    for (int i : files()->selection())
        out.push_back(model_.path(i));
    if (out.empty() && current() >= 0)
        out.push_back(current_path());
    return out;
}

void Pane::select_path(const std::string &path) {
    int at = model_.index_of_path(path);
    if (at >= 0)
        files()->set_current(at, true);
    else
        pending_select_ = path;
}

void Pane::focus_view() {
    win_->set_active(this);
    files()->take_focus();
    // The window gives the keyboard back here when it regains the focus.
    win_->remember_focus(files());
}

void Pane::selection_changed() {
    if (dual_)
        redraw(); // the footer's marks
    if (view_ == View::Columns)
        columns_->selection_changed();
    if (view_ == View::Space && !dual_)
        space_->redraw(); // marks
    win_->pane_selected(this);
    PreviewClient &pv = App::get().preview();
    if (pv.running() && pv.owner() == this) {
        std::string p = current_path();
        if (!p.empty())
            pv.follow(p, this);
    }
}

// --- Actions ----------------------------------------------------------------------

bool Pane::in_trash() const { return in_trash_folder(folder_); }

bool Pane::typing() const { return win_->typing() || win_->modal_open(); }

void Pane::copy(bool cut) {
    auto paths = selected_paths();
    if (paths.empty())
        return;
    if (cut && (in_trash() || in_recents()))
        cut = false;
    App::get().copy_files(paths, cut);
    fo::Toast::show(win_, cut ? fo::trn("Cut %n items", int(paths.size())) : fo::trn("Copied %n items", int(paths.size())),
                    2);
}

void Pane::paste(const std::string &into) {
    std::string target = into.empty() ? folder_ : into;
    if (in_recents() && into.empty()) {
        fo::Toast::show(win_, fo::tr("Recents can't hold files"), 3, fo::Toast::Error);
        return;
    }
    if (in_trash_folder(target)) {
        fo::Toast::show(win_, fo::tr("Delete moves things to the trash; pasting there doesn't"), 3, fo::Toast::Error);
        return;
    }
    paste_pending_ = true;
    paste_into_ = target;
    Fl::paste(*this, 1, Fl::clipboard_plain_text);
}

void Pane::copy_path() {
    auto paths = selected_paths();
    if (paths.empty() && !in_recents())
        paths.push_back(folder_);
    std::string text;
    for (const auto &p : paths)
        text += (text.empty() ? "" : "\n") + p;
    App::get().copy_text(text);
    fo::Toast::show(win_, paths.size() == 1 ? fo::tr("Copied the path") : fo::trn("Copied %n paths", int(paths.size())), 2);
}

void Pane::rename() {
    const Entry *e = current_entry();
    if (!e || in_trash() || in_recents() || !query_.empty()) {
        if (e && (in_recents() || !query_.empty()))
            fo::Toast::show(win_, fo::tr("Rename it from its folder"), 2);
        return;
    }
    std::string path = current_path(), name = display_name(*e);
    size_t ext = extension_start(name, e->is_dir);
    auto *d = fo::Dialog::prompt(win_, fo::tr("Rename"), "", name, fo::tr("Rename"),
                                 [this, path, name](bool ok, const std::string &text) {
                                     if (ok && text != name)
                                         ops_->rename(path, text);
                                 },
                                 0, ext == std::string::npos ? -1 : int(ext));
    d->on_validate([d, path, name](const std::string &s) {
        if (!valid_file_name(s)) {
            d->set_error(fo::tr("A name can't be empty, contain / or be . or .."));
            return false;
        }
        if (s != name && exists_nofollow(path_join(path_dirname(path), s))) {
            d->set_error(fo::tr("That name is taken"));
            return false;
        }
        return true;
    });
}

void Pane::duplicate() {
    if (in_trash() || in_recents())
        return;
    auto paths = selected_paths();
    if (!paths.empty())
        ops_->duplicate(paths);
}

void Pane::new_folder() {
    if (in_trash() || in_recents())
        return;
    ops_->new_folder(folder_);
}

void Pane::delete_selected() {
    auto paths = selected_paths();
    if (paths.empty())
        return;
    if (in_starred()) {
        // Files: Delete takes the entry off the list; the file is not touched.
        Starred &st = App::get().starred();
        int n = st.remove(paths);
        if (n > 0 && st.save()) {
            fo::Toast::show(win_, n == 1 ? fo::i18n::arg(fo::tr("Removed from Starred: %1 \xC2\xB7 the file was not touched"),
                                                          path_basename(paths[0]))
                                         : fo::trn("%n items removed from starred", n),
                            3);
            reload(true);
        }
        return;
    }
    if (in_recents())
        return;
    if (in_trash())
        confirm_delete(paths);
    else
        ops_->trash(paths);
}

void Pane::confirm_delete(const std::vector<std::string> &paths) {
    if (paths.empty())
        return;
    std::string msg = paths.size() == 1
                          ? fo::i18n::arg(fo::tr("%1 will be deleted permanently. This can't be undone."),
                                          quoted(path_basename(paths[0])))
                          : fo::trn("%n items will be deleted permanently. This can't be undone.", int(paths.size()));
    fo::Dialog::confirm(win_, fo::tr("Delete for good?"), msg, fo::tr("Delete"),
                        [this, paths](bool yes) {
                            if (yes)
                                ops_->delete_permanently(paths, true);
                        },
                        true);
}

void Pane::empty_trash() {
    int count = in_trash() ? model_.count() : int(trash::list().size());
    if (count == 0)
        return;
    fo::Dialog::confirm(win_, fo::tr("Empty the trash?"),
                        fo::trn("%n items will be deleted permanently. This can't be undone.", count),
                        fo::tr("Empty Trash"),
                        [this](bool yes) {
                            if (yes)
                                ops_->empty_trash(true);
                        },
                        true);
}

void Pane::restore(const std::vector<std::string> &paths) {
    if (!paths.empty())
        ops_->restore(paths);
}

void Pane::undo() { ops_->undo(); }
void Pane::redo() { ops_->redo(); }

void Pane::toggle_hidden() {
    hidden_ = !hidden_;
    App::get().settings().show_hidden = hidden_;
    App::get().save_settings();
    win_->pane_view_changed(this);
    if (!query_.empty())
        set_query(query_);
    else
        reload(true);
    if (view_ == View::Columns)
        columns_->refresh();
}

void Pane::set_sort(SortMode m, bool remember) {
    sort_ = m;
    if (remember) {
        App::get().settings().sort = m;
        App::get().save_settings();
    }
    win_->pane_view_changed(this);
    if (query_.empty())
        reload(true);
    if (view_ == View::Columns)
        columns_->refresh();
}

void Pane::sort_menu() {
    std::vector<fo::MenuItem> items;
    const char *labels[] = {"Name", "Kind", "Newest", "Oldest", "Size"};
    for (int k = 0; k < 5; ++k) {
        SortMode m = kSorts[k];
        items.push_back(fo::MenuItem(fo::tr(labels[k]), "", [this, m] { set_sort(m); }).check(m == sort_));
    }
    if (list_header_->visible())
        fo::PopupMenu::open_below(win_, list_header_->x(), list_header_->y(), 200, list_header_->h(), items);
    else
        fo::PopupMenu::open_at(win_, x() + 40, y() + 40, items);
}

void Pane::toggle_pin(const std::string &path) {
    std::string target = path;
    if (target.empty()) {
        const Entry *e = current_entry();
        target = e && e->is_dir ? current_path() : folder_;
    }
    if (target.empty() || is_virtual_folder(target))
        return;
    if (places::is_pinned(target)) {
        places::unpin(target);
        fo::Toast::show(win_, fo::i18n::arg(fo::tr("Unpinned %1"), path_basename(target)), 2);
    } else {
        std::string problem = places::pin(target);
        if (!problem.empty()) {
            fo::Toast::show(win_, problem, 3, fo::Toast::Error);
            return;
        }
        fo::Toast::show(win_, fo::i18n::arg(fo::tr("Pinned %1"), path_basename(target)), 2);
    }
    App::get().refresh_places();
}

void Pane::toggle_star() {
    std::vector<std::string> paths;
    for (const auto &p : selected_paths())
        if (!p.empty() && p[0] == '/' && !in_trash_folder(p))
            paths.push_back(p);
    if (paths.empty()) {
        fo::Toast::show(win_, fo::tr("Star: select a file or folder"), 2.5);
        return;
    }
    Starred &st = App::get().starred();
    int n = st.toggle(paths);
    if (n == 0 || !st.save()) {
        fo::Toast::show(win_, st.error().empty() ? fo::tr("The starred list can't be saved.") : fo::tr(st.error().c_str()),
                        3, fo::Toast::Error);
        if (n != 0)
            st.load();
        return;
    }
    std::string msg = n == 1    ? fo::tr("Starred \xC2\xB7 Ctrl+Alt+E opens Starred")
                      : n > 1   ? fo::trn("%n items starred", n)
                      : n == -1 ? fo::tr("Removed from starred")
                                : fo::trn("%n items removed from starred", -n);
    fo::Toast::show(win_, msg, 2.5);
    if (in_starred())
        reload(true);
    else
        files()->redraw();
}

void Pane::show_properties() {
    std::vector<std::string> paths;
    for (const auto &p : selected_paths())
        if (!p.empty() && p[0] == '/')
            paths.push_back(p);
    PropertiesDialog::open(win_, paths);
}

void Pane::preview() {
    PreviewClient &pv = App::get().preview();
    std::string p = current_path();
    if (p.empty() && !pv.running())
        return;
    pv.toggle(p, hidden_, sort_mode_name(sort_), [this](const std::string &to) { select_path(to); }, this);
}

void Pane::open_with(const std::string &path, const std::string &app) {
    if (!launch_app(app, {path}))
        show_message(fo::tr("The application couldn't be started."));
}

void Pane::set_default(const std::string &mime, const std::string &app, const std::string &name) {
    auto error = std::make_shared<std::string>();
    std::weak_ptr<char> weak = alive_.weak();
    fo::tasks::run([error, mime, app] { *error = set_default_app(mime, app); },
                   [this, weak, error, mime, name] {
                       if (weak.expired())
                           return;
                       if (!error->empty()) {
                           show_message(fo::i18n::arg(fo::tr("The default couldn't be changed: %1"), *error));
                           return;
                       }
                       std::string type = type_description(mime);
                       fo::Toast::show(win_,
                                       fo::i18n::arg(fo::i18n::arg(fo::tr("%1 now opens %2 files"), name),
                                                     type.empty() ? mime : type),
                                       3);
                   });
}

// Share offers only what this machine has, as in Folio: a Dropbox link
// for things in the Dropbox folder while Dropbox runs, LocalSend when it is
// installed, and Taildrop to the online tailnet machines (its own menu,
// last). With none of them there is no Share at all.
std::vector<fo::MenuItem> Pane::share_menu(const std::vector<std::string> &paths, const std::string &target) {
    using fo::MenuItem;
    std::vector<MenuItem> share;
    if (paths.empty() || target.empty())
        return share;
    const PlacesData &pd = App::get().places();
    const std::string dropbox = xdg::home() + "/Dropbox/";
    if (target.compare(0, dropbox.size(), dropbox) == 0 && !pd.dropbox.empty() && pd.dropbox != "stopped")
        share.push_back(MenuItem(fo::tr("Dropbox Link"), "", [this, target] { dropbox_link(target); }));
    if (!xdg::find_program("localsend").empty())
        share.push_back(MenuItem(fo::tr("LocalSend\xE2\x80\xA6"), "", [this, paths] { share_localsend(paths); }));
    if (!pd.peers.empty()) {
        std::vector<MenuItem> sub;
        for (const auto &p : pd.peers) {
            std::string host = p.host.empty() ? p.name : p.host, name = p.name;
            sub.push_back(MenuItem(p.name, "", [this, paths, host, name] { share_taildrop(paths, host, name); }));
        }
        share.push_back(MenuItem(fo::tr("Taildrop"), "", nullptr, sub));
    }
    return share;
}

void Pane::run_tool(const std::vector<std::string> &argv, const std::string &busy, int timeout_ms,
                    std::function<void(const ProcessResult &)> done) {
    busy_ = busy;
    place_children();
    redraw();
    auto result = std::make_shared<ProcessResult>();
    std::weak_ptr<char> weak = alive_.weak();
    fo::tasks::run(
        [result, argv, timeout_ms] {
            RunOptions o;
            o.timeout_ms = timeout_ms;
            o.max_output = 64 * 1024;
            *result = run_process(argv, o);
        },
        [this, weak, result, done] {
            if (weak.expired())
                return;
            busy_.clear();
            place_children();
            redraw();
            done(*result);
        });
}

static std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    size_t a = s.find_first_not_of(" \n");
    return a == std::string::npos ? std::string() : s.substr(a);
}

void Pane::dropbox_link(const std::string &path) {
    const std::string name = path_basename(path);
    if (xdg::find_program("dropbox-cli").empty()) {
        show_message(fo::tr("Dropbox links need dropbox-cli, which isn't installed."));
        return;
    }
    run_tool({"dropbox-cli", "sharelink", path},
             fo::i18n::arg(fo::tr("Getting a Dropbox link for %1\xE2\x80\xA6"), name), 30000,
             [this, name](const ProcessResult &r) {
                 // dropbox-cli prints the link, or a sentence saying why not.
                 std::string out = trimmed(r.out);
                 std::string link = trimmed(out.substr(out.rfind('\n') == std::string::npos ? 0 : out.rfind('\n') + 1));
                 if (!r.ok() || link.compare(0, 8, "https://") != 0) {
                     std::string why = out.empty() ? trimmed(r.err) : out;
                     show_message(fo::i18n::arg(fo::i18n::arg(fo::tr("Couldn't get a Dropbox link for %1: %2"), name), why));
                     return;
                 }
                 App::get().copy_text(link);
                 fo::Toast::show(win_, fo::i18n::arg(fo::tr("Dropbox link for %1 copied."), name), 3);
             });
}

void Pane::share_localsend(const std::vector<std::string> &paths) {
    // The same command Omarchy's Nautilus extension runs.
    std::vector<std::string> argv = {"localsend", "--headless", "send"};
    argv.insert(argv.end(), paths.begin(), paths.end());
    if (paths.empty())
        return;
    if (xdg::find_program("localsend").empty() || !spawn_detached(argv))
        show_message(fo::tr("Sharing nearby needs LocalSend, which isn't installed."));
}

void Pane::share_taildrop(const std::vector<std::string> &paths, const std::string &host, const std::string &name) {
    if (paths.empty())
        return;
    if (xdg::find_program("tailscale").empty()) {
        show_message(fo::tr("Sending with Taildrop needs tailscale, which isn't installed."));
        return;
    }
    std::vector<std::string> argv = {"tailscale", "file", "cp"};
    argv.insert(argv.end(), paths.begin(), paths.end());
    argv.push_back(host + ":");
    run_tool(argv, fo::i18n::arg(fo::tr("Sending to %1\xE2\x80\xA6"), name), -1, [this, name](const ProcessResult &r) {
        if (!r.ok())
            show_message(fo::i18n::arg(fo::i18n::arg(fo::tr("Couldn't send to %1: %2"), name), trimmed(r.err)));
        else
            fo::Toast::show(win_, fo::i18n::arg(fo::tr("Sent to %1"), name), 3);
    });
}

void Pane::compress() {
    auto paths = selected_paths();
    if (!paths.empty() && !in_trash())
        ops_->compress(paths);
}

void Pane::extract(const std::string &path) {
    if (!path.empty())
        ops_->extract(path);
}

void Pane::drop_into(const std::vector<std::string> &paths, const std::string &folder) {
    if (paths.empty() || folder.empty() || is_virtual_folder(folder))
        return;
    bool all_here = std::all_of(paths.begin(), paths.end(),
                                [&](const std::string &p) { return path_dirname(p) == folder || p == folder; });
    if (all_here)
        return;
    if (in_trash_folder(folder)) {
        ops_->trash(paths);
        return;
    }
    ops_->drop(paths, folder, ConflictPolicy::Ask);
}

void Pane::start_drag() {
    auto paths = selected_paths();
    if (paths.empty())
        return;
    std::string text = uri_list_for_drag(paths);
    Fl::copy(text.c_str(), int(text.size()), 0);
    Fl::dnd();
}

bool Pane::escape() {
    FileView *v = files();
    if (v->selection_count() > 1) {
        v->set_current(v->current(), true);
        return true;
    }
    if (!filter_.empty()) {
        set_filter("");
        return true;
    }
    if (!query_.empty()) {
        set_query("");
        return true;
    }
    return false;
}

void Pane::set_query(const std::string &q) {
    std::string t = q;
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    while (!t.empty() && t.front() == ' ')
        t.erase(0, 1);
    if (t == query_ && !t.empty())
        return;
    if (query_.empty() && !t.empty())
        remember_listing(); // the folder comes back at once when the search ends
    query_ = t;
    if (query_.empty()) {
        searcher_->cancel();
        searching_ = false;
        files()->show_folders(false);
        files()->empty_text(fo::tr("Empty"));
        model_.entries.clear();
        model_.peeks.clear();
        model_.loading = true;
        if (recall_listing())
            files()->reload(true);
        list_now(false);
        place_children();
        return;
    }
    model_.entries.clear();
    model_.error.clear();
    model_.loading = true;
    searching_ = true;
    files()->reload(true);
    files()->set_current(-1, true);
    files()->show_folders(true);
    files()->empty_text(fo::tr("No matches"));
    SearchOptions o;
    o.show_hidden = hidden_;
    searcher_->start(
        folder_, query_, o,
        [this](std::vector<Entry> &&batch) {
            for (auto &e : batch)
                model_.entries.push_back(std::move(e));
            files()->reload(false);
        },
        [this](const SearchStats &) {
            searching_ = false;
            model_.loading = false;
            place_children();
            files()->redraw();
        });
    place_children();
    redraw();
}

void Pane::menu_from_keyboard() {
    int cur = current();
    int x, y, w, h;
    if (cur >= 0 && files()->item_rect(cur, x, y, w, h))
        context_menu(cur, x + std::min(w / 2, 120), y + h / 2);
    else
        context_menu(-1, files()->x() + 40, files()->y() + 40);
}

void Pane::context_menu(int index, int x, int y, FolderModel *where, const std::string &folder) {
    win_->set_active(this);
    std::vector<fo::MenuItem> m = menu_for(index, where, folder);
    remap_menu_keys(m);
    fo::PopupMenu::open_at(win_, x, y, m);
}

std::vector<fo::MenuItem> Pane::menu_for(int index, FolderModel *where, const std::string &folder_in) {
    using fo::MenuItem;
    if (!where)
        where = &model_;
    std::string folder = folder_in.empty() ? folder_ : folder_in;
    bool item = index >= 0 && index < where->count();
    Entry e = item ? where->entries[size_t(index)] : Entry();
    std::string target = item ? where->path(index) : std::string();
    bool for_folder = item && e.is_dir;
    bool for_selection = item && where == &model_ && files()->selected(index) && files()->selection_count() > 1;
    std::vector<std::string> paths = !item ? std::vector<std::string>() : for_selection ? selected_paths()
                                                                                         : std::vector<std::string>{target};
    bool trash_view = in_trash_folder(folder) || (item && in_trash_folder(target));
    bool recents = is_virtual_folder(folder);
    int n = int(paths.size());
    std::vector<MenuItem> m;
    if (item && recents) {
        // Files' menu of a Recents / Starred row: only listing commands, no
        // file action (context_menu.rs open_virtual).
        const bool gone = e.mode == 0;
        const bool starred = App::get().starred().contains(target);
        m.push_back(MenuItem(fo::tr("Open"), "Enter", [this, target] { open_path(target); })
                        .with_icon("open")
                        .disabled(gone));
        m.push_back(MenuItem(fo::tr("Open containing folder"), "Ctrl+Alt+O", [this, target] { show_item(target); })
                        .with_icon("folder-open")
                        .disabled(!is_dir(path_dirname(target))));
        if (folder == kStarred)
            m.push_back(MenuItem(fo::tr("Remove from Starred (the file is not deleted)"), "Delete", [this, target] {
                select_path(target);
                delete_selected();
            }).with_icon("x"));
        m.push_back(MenuItem(starred ? fo::tr("Remove from starred") : fo::tr("Star"), "Ctrl+E", [this, target] {
            select_path(target);
            toggle_star();
        }).with_icon(starred ? "star-outline" : "star"));
        return m;
    }
    if (item && trash_view)
        m.push_back(MenuItem(n > 1 ? fo::trn("Restore %n Items", n) : fo::tr("Restore"), "", [this, paths] { restore(paths); }));
    if (!item && trash_view)
        m.push_back(MenuItem(fo::tr("Empty Trash\xE2\x80\xA6"), "", [this] { empty_trash(); }));
    if (item) {
        m.push_back(MenuItem(fo::tr("Open focused item"), "Enter", [this, target] { open_path(target); }).with_icon("open"));
        if (!for_folder && !trash_view) {
            auto apps = apps_for(e.mime);
            if (apps.size() > 1) {
                std::vector<MenuItem> sub;
                for (const auto &a : apps) {
                    std::string id = a.id;
                    sub.push_back(MenuItem(a.is_default ? fo::i18n::arg(fo::tr("%1 (default)"), a.name) : a.name, "",
                                           [this, target, id] { open_with(target, id); }));
                }
                // Changing the default is its own choice, never a side
                // effect of opening with another application.
                std::vector<MenuItem> defaults;
                std::string mime = e.mime ? e.mime : "";
                for (const auto &a : apps) {
                    if (a.is_default)
                        continue;
                    std::string id = a.id, name = a.name;
                    defaults.push_back(MenuItem(name, "", [this, mime, id, name] { set_default(mime, id, name); }));
                }
                if (!defaults.empty() && !mime.empty()) {
                    sub.push_back(MenuItem::separator());
                    sub.push_back(MenuItem(fo::tr("Set Default"), "", nullptr, defaults));
                }
                m.push_back(MenuItem(fo::tr("Open with\xE2\x80\xA6"), "Ctrl+Shift+O", nullptr, sub).with_icon("external-link"));
            }
        }
        m.push_back(MenuItem(fo::tr("Preview"), "Space", [this, target, where] {
            if (where == &model_) {
                select_path(target);
                preview();
            } else {
                show_item(target);
            }
        }).with_icon("eye"));
        m.push_back(MenuItem(fo::tr("Properties"), "Alt+Enter", [this, target] {
            PropertiesDialog::open(win_, {target});
        }).with_icon("info").disabled(n > 1));
        m.push_back(MenuItem::separator());
    }
    bool change = !trash_view && !recents;
    if (change && item && where == &model_ && query_.empty()) {
        m.push_back(MenuItem(fo::tr("Rename"), "F2", [this, target] {
            select_path(target);
            rename();
        }).with_icon("edit"));
        m.push_back(MenuItem(fo::tr("Duplicate"), "", [this, paths] { ops_->duplicate(paths); }).with_icon("copy"));
    }
    if (item)
        m.push_back(MenuItem(fo::tr("Copy"), "Ctrl+C", [this, paths] {
            App::get().copy_files(paths, false);
            fo::Toast::show(win_, fo::trn("Copied %n items", int(paths.size())), 2);
        }).with_icon("copy"));
    if (item && change)
        m.push_back(MenuItem(fo::tr("Cut"), "Ctrl+X", [this, paths] {
            App::get().copy_files(paths, true);
            fo::Toast::show(win_, fo::trn("Cut %n items", int(paths.size())), 2);
        }).with_icon("cut"));
    if (change && (!item || for_folder)) {
        std::string into = item ? target : folder;
        m.push_back(MenuItem(for_folder ? fo::tr("Paste into folder") : fo::tr("Paste"), item ? "" : "Ctrl+V",
                             [this, into] { paste(into); }).with_icon("paste"));
    }
    m.push_back(MenuItem(fo::tr("Copy path"), "Ctrl+Alt+C", [this, paths, folder] {
        std::vector<std::string> ps = paths.empty() ? std::vector<std::string>{folder} : paths;
        std::string text;
        for (const auto &p : ps)
            text += (text.empty() ? "" : "\n") + p;
        App::get().copy_text(text);
        fo::Toast::show(win_, fo::tr("Copied the path"), 2);
    }).with_icon("link"));
    if (!item && change)
        m.push_back(MenuItem(fo::tr("Create folder"), "F7", [this, folder] { ops_->new_folder(folder); }).with_icon("folder-plus"));
    if (!item && change)
        m.push_back(MenuItem(fo::tr("Create file"), "Ctrl+N", [this] { new_file(); }).with_icon("file"));
    m.push_back(MenuItem::separator());
    if (item && change) {
        m.push_back(MenuItem(fo::tr("Compress"), "", [this, paths] { ops_->compress(paths); }).with_icon("compress"));
        if (e.kind == Kind::Archive && !e.is_dir)
            m.push_back(MenuItem(fo::tr("Extract here"), "", [this, target] { extract(target); }).with_icon("extract"));
        auto share = share_menu(paths, target);
        if (!share.empty())
            m.push_back(MenuItem(fo::tr("Share"), "", nullptr, share).with_icon("share"));
    }
    if (item && !trash_view) {
        bool starred = !paths.empty() && std::all_of(paths.begin(), paths.end(), [](const std::string &p) {
            return App::get().starred().contains(p);
        });
        m.push_back(MenuItem(starred ? fo::tr("Remove from starred") : fo::tr("Star"), "Ctrl+E", [this, paths] {
            for (const auto &p : paths)
                if (model_.index_of_path(p) >= 0 && !files()->selected(model_.index_of_path(p)))
                    select_path(p);
            toggle_star();
        }).with_icon(starred ? "star-outline" : "star"));
    }
    if (for_folder && !trash_view)
        m.push_back(MenuItem(places::is_pinned(target) ? fo::tr("Unpin") : fo::tr("Pin"), "Ctrl+D",
                             [this, target] { toggle_pin(target); }));
    if ((!item && !recents) || for_folder) {
        std::string dir = item ? target : folder;
        m.push_back(MenuItem(fo::tr("Terminal here"), "Ctrl+.", [this, dir] {
            if (!open_terminal(dir))
                show_message(fo::tr("No terminal was found."));
        }).with_icon("terminal"));
    }
    if (!item)
        m.push_back(MenuItem(hidden_ ? fo::tr("Hide Hidden Files") : fo::tr("Show Hidden Files"), "Ctrl+H",
                             [this] { toggle_hidden(); }));
    if (!item && !undo_label_.empty())
        m.push_back(MenuItem(fo::i18n::arg(fo::tr("Undo %1"), undo_label_), "Ctrl+Z", [this] { undo(); }));
    if (!item && !redo_label_.empty())
        m.push_back(MenuItem(fo::i18n::arg(fo::tr("Redo %1"), redo_label_), "Ctrl+Shift+Z", [this] { redo(); }));
    if (item) {
        m.push_back(MenuItem::separator());
        if (trash_view)
            m.push_back(MenuItem(fo::tr("Delete Permanently\xE2\x80\xA6"), "Delete", [this, paths] { confirm_delete(paths); }));
        else
            m.push_back(MenuItem(n > 1 ? fo::trn("Send %n items to Trash", n) : fo::tr("Send to Trash"), "Delete",
                                 [this, paths] { ops_->trash(paths); }).with_icon("trash"));
    }
    // No separator at the ends or twice in a row.
    std::vector<MenuItem> out;
    for (auto &i : m) {
        if (i.is_separator && (out.empty() || out.back().is_separator))
            continue;
        out.push_back(std::move(i));
    }
    while (!out.empty() && out.back().is_separator)
        out.pop_back();
    return out;
}

// --- File operations ---------------------------------------------------------

void Pane::op_progress(const OpProgress &p) {
    const bool card = copy_card();
    progress_ = p;
    if (!progress_on_ || card != copy_card()) {
        progress_on_ = true;
        relabel_card();
        place_children();
    }
    redraw();
}

bool Pane::copy_card() const {
    if (!progress_on_)
        return false;
    switch (progress_.kind) {
    case OpKind::Copy:
    case OpKind::Move:
    case OpKind::Duplicate:
    case OpKind::Compress:
    case OpKind::Extract: return true;
    default: return false;
    }
}

void Pane::relabel_card() {
    const bool paused = ops_ && progress_on_ && ops_->paused(progress_.op);
    pause_->copy_label(paused ? fo::tr("Resume") : fo::tr("Pause"));
    cancel_job_->copy_label(card_h_ > 0 && w() < 420 ? fo::tr("Cancel") : fo::tr("Cancel job"));
    clear_pending_->copy_label(fo::tr("Clear pending"));
}

// Files' copy card (copy_ui.rs): what is copied, bytes and entries so far,
// the operations waiting, the current item, Pause / Resume, Cancel job and
// Clear pending.
void Pane::draw_copy_card() {
    const fo::Theme &t = fo::theme();
    const int mx = dual_ ? 8 : 16, cx = x() + mx, cw = std::max(10, w() - 2 * mx);
    const bool narrow = cw < 340;
    const bool paused = ops_ && ops_->paused(progress_.op);
    const size_t pending = ops_ ? ops_->pending() : 0;
    fo::paint::round_fill(cx, card_y_, cw, card_h_, t.radius() > 0 ? 8 : 0, t.dark_background());
    std::string label;
    switch (progress_.kind) {
    case OpKind::Move: label = fo::tr("Moving"); break;
    case OpKind::Compress: label = fo::tr("Compressing"); break;
    case OpKind::Extract: label = fo::tr("Extracting"); break;
    case OpKind::Duplicate: label = fo::tr("Duplicating"); break;
    default: label = fo::tr("Copying"); break;
    }
    if (paused)
        label = fo::tr("Paused");
    const std::string bytes = si_size(progress_.bytes_done);
    const std::string entries = fo::trn("%n entries", int(progress_.items_done));
    std::string line = narrow ? label + " \xC2\xB7 " + fo::trn("%n queued", int(pending))
                              : label + " \xC2\xB7 " + bytes + " \xC2\xB7 " + entries + " \xC2\xB7 " +
                                    fo::trn("%n pending", int(pending));
    if (!narrow && progress_.bytes_total > 0) {
        const int pw = 120, px = cx + cw - 12 - pw, py = card_y_ + 15;
        double f = std::min(1.0, double(progress_.bytes_done) / double(progress_.bytes_total));
        fo::paint::fill(px, py, pw, 4, t.selection());
        fo::paint::fill(px, py, int(pw * f), 4, paused ? t.dim() : t.accent());
    }
    fo::text::draw(line, cx + 12, card_y_ + 4, cw - 24 - (narrow ? 0 : 132), narrow ? 22 : 26, t.bright_foreground(),
                   fo::text::REGULAR, 11);
    if (narrow)
        fo::text::draw(bytes + " \xC2\xB7 " + entries, cx + 12, card_y_ + 26, cw - 24, 20, t.dim(), fo::text::REGULAR, 10);
    if (!progress_.current.empty())
        fo::text::draw(progress_.current, cx + 12, card_y_ + (narrow ? 46 : 30), cw - 24, 20, t.dim(),
                       fo::text::REGULAR, 10, FL_ALIGN_LEFT, fo::text::Elide::Middle);
}

void Pane::op_conflict(const OpConflict &c) {
    fo::DialogSpec s;
    std::string name = path_basename(c.destination);
    s.title = fo::i18n::arg(fo::tr("%1 already exists"), quoted(name));
    s.message = fo::i18n::arg(c.destination_is_dir ? fo::tr("There's already a folder with this name in %1.")
                                                   : fo::tr("There's already a file with this name in %1."),
                              quoted(tilde(path_dirname(c.destination))));
    s.message += std::string(" ") + fo::tr("Replacing moves it to the trash.");
    s.buttons = {fo::tr("Skip"), fo::tr("Replace"), fo::tr("Keep Both")};
    s.check = fo::tr("Apply to all");
    s.scrim_cancels = false;
    s.focus_field = -1;
    uint64_t op = c.op;
    fo::Dialog::ask(win_, s, [this, op](const fo::DialogAnswer &a) {
        ConflictChoice ch = a.button == 0   ? ConflictChoice::Skip
                            : a.button == 1 ? ConflictChoice::Replace
                            : a.button == 2 ? ConflictChoice::KeepBoth
                                            : ConflictChoice::Cancel;
        ops_->resolve_conflict(op, ch, a.checked);
    });
}

void Pane::op_done(const OpResult &r) {
    bool was = progress_on_;
    if (ops_ && ops_->busy() && r.op != progress_.op)
        was = false; // a cleared pending operation: the running one goes on
    else
        progress_on_ = false;
    if (was)
        place_children();
    if (r.kind == OpKind::Undo || r.kind == OpKind::Redo) {
        if (!r.message.empty())
            fo::Toast::show(win_, r.message, 3, r.ok ? fo::Toast::Info : fo::Toast::Error);
    } else if (!r.ok && !r.cancelled) {
        std::string text = r.message;
        if (!r.errors.empty())
            text = r.errors.size() == 1 ? r.errors[0] : r.message + " (" + r.errors[0] + ")";
        show_message(text.empty() ? fo::tr("Something went wrong.") : text);
    } else if (r.kind == OpKind::Trash && r.ok) {
        fo::Toast::show(win_, (r.message.empty() ? fo::tr("Moved to the trash") : r.message) + " \xC2\xB7 " +
                                  fo::tr("Ctrl+Z undoes"),
                        3);
    } else if ((r.kind == OpKind::Compress || r.kind == OpKind::Extract || r.kind == OpKind::Restore ||
                r.kind == OpKind::EmptyTrash || r.kind == OpKind::DeletePermanently) &&
               !r.message.empty()) {
        fo::Toast::show(win_, r.message, 3);
    }
    // What was made gets selected (the pasted file, the new folder, the new name).
    if (!r.created.empty() && r.folder == folder_) {
        pending_select_ = r.created[0];
        reload(false);
    } else if (r.kind == OpKind::Trash || r.kind == OpKind::DeletePermanently || r.kind == OpKind::EmptyTrash ||
               r.kind == OpKind::Undo || r.kind == OpKind::Redo || r.kind == OpKind::Restore) {
        reload(true);
    }
    redraw();
}

// --- Banner, layout, events ----------------------------------------------------

void Pane::show_message(const std::string &text, bool error) {
    message_ = text;
    message_error_ = error;
    place_children();
    redraw();
}

void Pane::dismiss_message() {
    message_.clear();
    place_children();
    redraw();
}

std::string Pane::banner_text(bool &error, bool &progress) const {
    error = false;
    progress = false;
    if (!message_.empty()) {
        error = message_error_;
        return message_;
    }
    if (!model_.error.empty()) {
        error = true;
        return fo::tr(model_.error.c_str()); // the core's sentences are in the catalogs
    }
    if (progress_on_ && !copy_card()) {
        progress = true;
        std::string verb;
        switch (progress_.kind) {
        case OpKind::Copy: verb = fo::tr("Copying"); break;
        case OpKind::Move: verb = fo::tr("Moving"); break;
        case OpKind::Trash: verb = fo::tr("Moving to the trash"); break;
        case OpKind::Compress: verb = fo::tr("Compressing"); break;
        case OpKind::Extract: verb = fo::tr("Extracting"); break;
        case OpKind::Duplicate: verb = fo::tr("Duplicating"); break;
        case OpKind::Restore: verb = fo::tr("Restoring"); break;
        case OpKind::DeletePermanently:
        case OpKind::EmptyTrash: verb = fo::tr("Deleting"); break;
        default: verb = fo::tr("Working"); break;
        }
        std::string s = verb;
        if (progress_.items_total > 1)
            s += " " + std::to_string(progress_.items_done + 1 > progress_.items_total ? progress_.items_total
                                                                                         : progress_.items_done + 1) +
                 " / " + std::to_string(progress_.items_total);
        if (!progress_.current.empty())
            s += " \xC2\xB7 " + path_basename(progress_.current);
        return s;
    }
    if (!busy_.empty())
        return busy_;
    if (searching_)
        return fo::tr("Searching\xE2\x80\xA6");
    return {};
}

int Pane::banner_height() const {
    bool e, p;
    return banner_text(e, p).empty() ? 0 : kBanner;
}

int Pane::header_height() const {
    int h = dual_ ? kDualHeader : 0;
    if (dual_ || view_ == View::List)
        h += kListHeader;
    return h;
}

void Pane::place_children() {
    int bh = banner_height();
    bool trash_bar = in_trash() && model_.count() > 0;
    bool err, prog;
    banner_text(err, prog);
    // Files' content margins: 16 px from the rail and the inspector, 4 px
    // under the tabs, 8 px above the status bar.
    const int mx = dual_ ? 8 : 16, top = y() + 4 + (dual_ ? kDualHeader : 0);
    const int cx = x() + mx, cw = std::max(10, w() - 2 * mx);
    if (bh && !message_.empty()) {
        dismiss_->resize(x() + w() - 12 - 32, top + (kBanner - 32) / 2, 32, 32);
        dismiss_->show();
    } else {
        dismiss_->hide();
    }
    if (bh && prog && message_.empty() && model_.error.empty()) {
        cancel_->resize(x() + w() - 12 - 88, top + (kBanner - 28) / 2, 88, 28);
        cancel_->show();
    } else {
        cancel_->hide();
    }
    int ty = top + bh;
    if (trash_bar) {
        empty_trash_->resize(x() + (w() - 140) / 2, ty + (kTrashBar - 32) / 2, 140, 32);
        empty_trash_->show();
        ty += kTrashBar;
    } else {
        empty_trash_->hide();
    }
    bool list_header = dual_ || view_ == View::List;
    if (list_header) {
        list_header_->resize(cx, ty, cw, kListHeader);
        list_header_->show();
        // Files' list columns (list_columns.rs) over the rows.
        const bool d = dual_;
        const int start = d ? 30 : 48, pad = d ? 6 : 10, min_name = d ? 128 : 160;
        const int size_w = d ? 74 : 104, date_w = d ? 0 : 86, kind_w = d ? 0 : 92;
        int avail = std::max(0, cw - start - pad);
        bool sz = avail >= min_name + 8 + size_w;
        bool dt = date_w > 0 && avail >= min_name + 16 + size_w + date_w;
        bool kd = dt && kind_w > 0 && avail >= min_name + 24 + size_w + date_w + kind_w;
        int name_w = avail - (sz ? 8 + size_w : 0) - (dt ? 8 + date_w : 0) - (kd ? 8 + kind_w : 0);
        auto cols = list_header_columns();
        cols[0].x = start, cols[0].w = name_w;
        std::vector<fo::SortHeader::Column> shown = {cols[0]};
        if (sz)
            cols[1].x = start + name_w + 8, cols[1].w = size_w, cols[1].right = true, shown.push_back(cols[1]);
        if (dt)
            cols[2].x = start + name_w + 16 + size_w, cols[2].w = date_w, cols[2].right = true, shown.push_back(cols[2]);
        if (kd)
            cols[3].x = start + name_w + 24 + size_w + date_w, cols[3].w = kind_w, cols[3].right = true,
            shown.push_back(cols[3]);
        list_header_->columns(shown);
        int sorted = sort_ == SortMode::Size ? 1 : sort_ == SortMode::Newest || sort_ == SortMode::Oldest ? 2
                     : sort_ == SortMode::Kind ? 3 : 0;
        if (sorted >= int(shown.size()))
            sorted = 0;
        list_header_->sorted(sorted, sort_ == SortMode::Newest || sort_ == SortMode::Size);
        ty += kListHeader;
    } else {
        list_header_->hide();
    }
    grid_header_y_ = ty;
    if (grid_header())
        ty += kGridHeader;
    int vh = std::max(10, y() + h() - 8 - ty - (dual_ ? kDualFooter : 0));
    card_h_ = 0;
    if (copy_card() && message_.empty() && model_.error.empty()) {
        // Files: the card sits at the bottom of the list, 88 px (138 narrow).
        const bool narrow = cw < 340;
        card_h_ = narrow ? 138 : 88;
        vh = std::max(10, vh - card_h_ - 12);
        card_y_ = ty + vh + 12;
        const int by = card_y_ + (narrow ? 70 : 54), clear_y = card_y_ + (narrow ? 104 : 54);
        pause_->resize(cx + 12, by, 80, 27);
        cancel_job_->resize(cx + 100, by, narrow ? std::max(60, cw - 112) : 120, 27);
        clear_pending_->resize(narrow ? cx + 12 : cx + cw - 150, clear_y, narrow ? cw - 24 : 138, 27);
        pause_->show();
        cancel_job_->show();
        clear_pending_->show();
    } else {
        pause_->hide();
        cancel_job_->hide();
        clear_pending_->hide();
    }
    bool cols = view_ == View::Columns && !dual_, space = view_ == View::Space && !dual_;
    cols ? columns_->show() : columns_->hide();
    space ? space_->show() : space_->hide();
    (cols || space) ? grid_->hide() : grid_->show();
    if (dual_)
        grid_->mode(FileView::Mode::Dual);
    else if (view_ == View::Grid)
        grid_->mode(FileView::Mode::Grid);
    else if (view_ == View::List)
        grid_->mode(FileView::Mode::List);
    else
        grid_->mode(FileView::Mode::Groups);
    if (cols)
        columns_->resize(cx, ty, cw, vh);
    if (space)
        space_->resize(cx, ty, cw, vh);
    grid_->resize(cx, ty, cw, vh);
    redraw();
}

void Pane::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    place_children();
}

void Pane::draw() {
    const fo::Theme &t = fo::theme();
    if (!(damage() & ~FL_DAMAGE_CHILD)) {
        draw_children(); // only a child changed
        return;
    }
    fo::paint::fill(x(), y(), w(), h(), t.background());
    int top = y() + 4;
    if (dual_) {
        // Files' dual header: the folder, its path, the filter glyph; the
        // active pane in the accent.
        bool active = win_->active_pane() == this;
        fo::Rgb c = active ? t.accent() : t.dim();
        fo::icon::draw("folder", x() + 10, top + 6, 13, c);
        std::string where = title() + " \xC2\xB7 " + (in_recents() ? std::string() : folder_);
        fo::text::draw(where, x() + 28, top, w() - 60, kDualHeader - 4, c, fo::text::REGULAR, 11);
        fo::icon::draw("filter", x() + w() - 26, top + 7, 11, filter_.empty() ? t.dim() : t.accent());
        if (active)
            fo::paint::fill(x() + 8, top + kDualHeader - 3, w() - 16, 1, t.accent());
        top += kDualHeader;
    }
    bool err, prog;
    std::string text = banner_text(err, prog);
    int bh = text.empty() ? 0 : kBanner;
    if (bh) {
        int r = t.radius() > 0 ? 7 : 0;
        fo::paint::round_fill(x() + 16, top, w() - 32, bh - 4, r, err ? t.tint(t.red(), 0.16) : t.card());
        int right = x() + w() - 12 - (dismiss_->visible() ? 40 : cancel_->visible() ? 100 : 0);
        if (prog && progress_.bytes_total > 0) {
            double f = std::min(1.0, double(progress_.bytes_done) / double(progress_.bytes_total));
            int pw = 120;
            right -= pw + 12;
            fo::paint::fill(right + 6, top + bh / 2 - 2, pw, 4, t.selection());
            fo::paint::fill(right + 6, top + bh / 2 - 2, int(pw * f), 4, t.accent());
        }
        fo::text::draw(text, x() + 32, top, right - x() - 32, bh - 4, err ? t.red() : t.foreground(),
                       fo::text::REGULAR, 12);
    }
    if (grid_header()) {
        // Files' Grid header: "Grid · n · thumbnails", the zoom and − / +.
        int mx, px, zy;
        zoom_rects(mx, px, zy);
        int n = 0;
        if (App::get().settings().thumbnails)
            for (const auto &e : model_.entries)
                n += !e.is_dir && (e.kind == Kind::Image || e.kind == Kind::Video);
        std::string cap = std::string(fo::tr("Grid")) + " \xC2\xB7 " + std::to_string(model_.count()) + " \xC2\xB7 " +
                          (App::get().settings().thumbnails ? fo::trn("%n thumbnails", n)
                                                            : std::string(fo::tr("thumbnails off")));
        int cx = x() + 16;
        fo::text::draw(cap, cx, zy, mx - 72 - cx, 28, t.dim(), fo::text::REGULAR, 12);
        char pct[16];
        std::snprintf(pct, sizeof pct, "%.0f%%", grid_zoom_ * 100.0);
        fo::text::draw(pct, mx - 72, zy, 64, 28, t.dim(), fo::text::REGULAR, 11, FL_ALIGN_RIGHT);
        int r = t.radius() > 0 ? 5 : 0;
        fo::paint::round_fill(mx, zy, 36, 28, r, t.card());
        fo::paint::round_fill(px, zy, 36, 28, r, t.card());
        fo::text::draw("\xE2\x88\x92", mx, zy, 36, 28, grid_zoom_ > kZoomMin + 1e-6 ? t.accent() : t.dim(),
                       fo::text::REGULAR, 13, FL_ALIGN_CENTER, fo::text::Elide::None);
        fo::text::draw("+", px, zy, 36, 28, grid_zoom_ < kZoomMax - 1e-6 ? t.accent() : t.dim(), fo::text::REGULAR,
                       13, FL_ALIGN_CENTER, fo::text::Elide::None);
    }
    if (card_h_ > 0)
        draw_copy_card();
    draw_children();
    if (dual_) {
        // Files' dual footer: visible and loaded counts, the marks, Tab.
        std::string foot = fo::trn("%n visible", model_.count()) + " / " +
                           fo::trn("%n loaded", unfiltered_count()) + " \xC2\xB7 " +
                           fo::trn("%n marked", files()->selection_count());
        if (win_->active_pane() == this)
            foot += std::string(" \xC2\xB7 ") + fo::tr("Tab switches pane");
        fo::text::draw(foot, x() + 14, y() + h() - 8 - kDualFooter, w() - 28, kDualFooter, t.dim(), fo::text::REGULAR,
                       10);
    }
}

int Pane::handle(int event) {
    switch (event) {
    case FL_PASTE:
        if (paste_pending_) {
            paste_pending_ = false;
            bool cut = false;
            std::string text(Fl::event_text() ? Fl::event_text() : "", size_t(std::max(0, Fl::event_length())));
            auto paths = App::get().paths_from_clipboard(text, cut);
            if (paths.empty()) {
                fo::Toast::show(win_, fo::tr("Nothing to paste"), 2);
                return 1;
            }
            bool all_here = std::all_of(paths.begin(), paths.end(),
                                        [&](const std::string &p) { return path_dirname(p) == paste_into_; });
            ops_->paste(paths, paste_into_, cut, all_here ? ConflictPolicy::KeepBoth : ConflictPolicy::Ask);
            if (cut)
                App::get().clipboard_used_for_move();
            return 1;
        }
        break;
    case FL_PUSH:
        win_->set_active(this);
        if (grid_header() && Fl::event_button() == FL_LEFT_MOUSE) {
            int mx, px, zy;
            zoom_rects(mx, px, zy);
            int ex = Fl::event_x(), ey = Fl::event_y();
            if (ey >= zy && ey < zy + 28) {
                if (ex >= mx && ex < mx + 36) {
                    zoom_by(1.0 / kZoomStep);
                    return 1;
                }
                if (ex >= px && ex < px + 36) {
                    zoom_by(kZoomStep);
                    return 1;
                }
            }
        }
        break;
    }
    return Fl_Group::handle(event);
}

// --- Grid zoom ------------------------------------------------------------------

void Pane::zoom_rects(int &minus_x, int &plus_x, int &zy) const {
    // Files: − at the right edge minus 84, + minus 40, both 36 x 28.
    int right = x() + w() - 16;
    minus_x = right - 84;
    plus_x = right - 40;
    zy = grid_header_y_;
}

void Pane::set_grid_zoom(double z) {
    if (!std::isfinite(z) || z <= 0)
        return;
    z = std::clamp(z, kZoomMin, kZoomMax);
    if (std::fabs(z - grid_zoom_) < 1e-9)
        return;
    grid_zoom_ = z;
    grid_->zoom(z);
    grid_->scroll_to(0, false);
    if (grid_->current() >= 0)
        grid_->ensure_visible(grid_->current(), false);
    redraw();
}

void Pane::zoom_by(double factor) {
    if (!std::isfinite(factor) || factor <= 0)
        return;
    set_grid_zoom(grid_zoom_ * factor);
}

void Pane::zoom_step(int step) {
    if (view_ != View::Grid || dual_) {
        fo::Toast::show(win_, fo::i18n::arg(fo::tr("Zoom only exists in the Grid view (%1)"),
                                              primary_chord(App::get().settings().shortcuts, Cmd::ViewGrid)), 2.5, fo::Toast::Error);
        return;
    }
    if (step == 0)
        set_grid_zoom(1.0);
    else
        zoom_by(step > 0 ? kZoomStep : 1.0 / kZoomStep);
}

// --- Files' filter, selection, sizes and transfers --------------------------------------

void Pane::set_filter(const std::string &text) {
    if (text == filter_ || in_recents())
        return;
    FileView *v = files();
    std::string cur = model_.path(v->current());
    std::vector<std::string> sel;
    for (int i : v->selection())
        sel.push_back(model_.path(i));
    if (filter_.empty() && !text.empty())
        all_ = model_.entries;
    filter_ = text;
    if (filter_.empty()) {
        if (!all_.empty() || model_.entries.empty())
            model_.entries = std::move(all_);
        all_.clear();
    } else {
        NameFilter f(filter_);
        model_.entries.clear();
        for (const auto &e : all_)
            if (f.matches(e))
                model_.entries.push_back(e);
    }
    order_for_view();
    v->reload(true);
    v->set_current(-1, true);
    for (const auto &p : sel) {
        int at = model_.index_of_path(p);
        if (at >= 0)
            v->select(at, true);
    }
    int c = model_.index_of_path(cur);
    if (c >= 0)
        v->set_current(c, false);
    else if (model_.count() > 0 && !filter_.empty())
        v->set_current(0, true);
    v->empty_text(empty_label());
    if (view_ == View::Space && !dual_)
        space_->sync();
    win_->pane_view_changed(this);
    selection_changed();
    redraw();
}

void Pane::select_filtered() {
    if (filter_.empty())
        return;
    std::vector<std::string> marks;
    for (int i = 0; i < model_.count(); ++i)
        marks.push_back(model_.path(i));
    set_filter("");
    win_->end_field(false);
    FileView *v = files();
    v->clear_selection();
    int first = -1;
    for (const auto &p : marks) {
        int at = model_.index_of_path(p);
        if (at >= 0) {
            v->select(at, true);
            if (first < 0)
                first = at;
        }
    }
    if (first >= 0)
        v->set_current(first, false);
    focus_view();
}

void Pane::invert_selection() {
    FileView *v = files();
    for (int i = 0; i < model_.count(); ++i)
        v->select(i, !v->selected(i));
}

void Pane::clear_selection() {
    files()->set_current(-1, true);
    selection_changed();
}

void Pane::select_all() { files()->select_all(); }

void Pane::mark_and_advance() {
    FileView *v = files();
    int c = v->current();
    if (c < 0) {
        if (model_.count() > 0)
            v->set_current(0, false);
        return;
    }
    v->select(c, !v->selected(c));
    if (c + 1 < model_.count()) {
        // Moves on without dropping the batch.
        std::vector<int> keep = v->selection();
        v->set_current(c + 1, false);
        for (int i : keep)
            v->select(i, true);
    }
}

void Pane::toggle_mark() {
    FileView *v = files();
    int c = v->current();
    if (c < 0) {
        if (model_.count() > 0)
            v->set_current(0, false);
        return;
    }
    v->select(c, !v->selected(c));
}

void Pane::new_file() {
    if (in_trash() || in_recents())
        return;
    auto *d = fo::Dialog::prompt(win_, fo::tr("Create file"), "", fo::tr("New file.txt"), fo::tr("Create"),
                                 [this](bool ok, const std::string &name) {
                                     if (!ok)
                                         return;
                                     // Files: the worker creates it exclusively and
                                     // records it, so Ctrl+Z sends it to the trash.
                                     ops_->new_file(folder_, name);
                                 },
                                 0, 8);
    d->on_validate([d, this](const std::string &s) {
        if (!valid_file_name(s)) {
            d->set_error(fo::tr("A name can't be empty, contain / or be . or .."));
            return false;
        }
        if (exists_nofollow(path_join(folder_, s))) {
            d->set_error(fo::tr("That name is taken"));
            return false;
        }
        return true;
    });
}

void Pane::transfer(bool move, const std::string &folder) {
    auto paths = selected_paths();
    if (paths.empty())
        return;
    if (move && (in_trash() || in_recents()))
        move = false;
    std::string dest = folder;
    Pane *other = win_->other_pane(this);
    if (dest.empty() && other && !other->in_recents())
        dest = other->folder();
    int n = int(paths.size());
    std::string what = n == 1 ? quoted(path_basename(paths[0])) : fo::trn("%n items", n);
    // Files reviews the destination before anything is queued.
    fo::Dialog::prompt(win_, move ? fo::tr("Move") : fo::tr("Copy"),
                       fo::i18n::arg(move ? fo::tr("Move %1 to:") : fo::tr("Copy %1 to:"), what),
                       tilde(dest.empty() ? folder_ : dest), move ? fo::tr("Move") : fo::tr("Copy"),
                       [this, paths, move](bool ok, const std::string &to) {
                           if (!ok)
                               return;
                           std::string target = expand_typed(to, folder_);
                           if (!is_dir(target)) {
                               show_message(fo::i18n::arg(fo::tr("%1 can't be opened."), quoted(tilde(target))));
                               return;
                           }
                           if (move)
                               ops_->move(paths, target, ConflictPolicy::Ask);
                           else
                               ops_->copy(paths, target, ConflictPolicy::Ask);
                       });
}

void Pane::request_sizes() {
    if (!App::get().settings().auto_sizes || in_recents() || !query_.empty() || in_trash())
        return;
    std::vector<std::string> folders;
    for (int i = 0; i < model_.count() && folders.size() < 200; ++i)
        if (model_.entries[size_t(i)].is_dir && !model_.entries[size_t(i)].is_link)
            folders.push_back(model_.path(i));
    if (!folders.empty())
        App::get().sizes().request(folders);
}

void Pane::measure_focused() {
    const Entry *e = current_entry();
    std::string p = e && e->is_dir ? current_path() : folder_;
    if (p.empty() || is_virtual_folder(p))
        return;
    App::get().sizes().request({p}, true);
    win_->sizes_changed();
}

void Pane::measure_listed() {
    std::vector<std::string> folders;
    for (int i = 0; i < model_.count(); ++i)
        if (model_.entries[size_t(i)].is_dir && !model_.entries[size_t(i)].is_link)
            folders.push_back(model_.path(i));
    if (!folders.empty())
        App::get().sizes().request(folders, true);
    win_->sizes_changed();
}

// --- Folder views and the session ------------------------------------------------

bool Pane::has_folder_view() const { return App::get().folder_views().get(folder_) != nullptr; }

void Pane::save_folder_view() {
    if (in_recents() || folder_.empty() || folder_[0] != '/')
        return;
    FolderViews &fv = App::get().folder_views();
    FolderView v;
    v.path = folder_;
    v.view = to_mode(view_);
    v.sort = sort_;
    v.zoom_percent = int(std::lround(grid_zoom_ * 100.0));
    if (!fv.set(v)) {
        fo::Toast::show(win_, fo::tr(fv.error().c_str()), 3, fo::Toast::Error);
        return;
    }
    if (!fv.save()) {
        fo::Toast::show(win_, fo::tr("The folder views can't be saved."), 3, fo::Toast::Error);
        return;
    }
    fo::Toast::show(win_, fo::tr("This folder's view is saved"), 2.5);
}

void Pane::forget_folder_view() {
    FolderViews &fv = App::get().folder_views();
    if (!fv.forget(folder_)) {
        fo::Toast::show(win_, fo::tr("This folder has no saved view"), 2.5);
        return;
    }
    if (!fv.save()) {
        fo::Toast::show(win_, fo::tr("The folder views can't be saved."), 3, fo::Toast::Error);
        return;
    }
    fo::Toast::show(win_, fo::tr("Preference removed; the folder is untouched"), 2.5);
}

TabState Pane::tab_state() const {
    TabState t;
    t.folder = in_recents() ? xdg::home() : folder_;
    for (const auto &b : back_)
        if (!is_virtual_folder(b))
            t.back.push_back(b);
    for (const auto &f : forward_)
        if (!is_virtual_folder(f))
            t.forward.push_back(f);
    while (t.back.size() + t.forward.size() > Session::kMaxHistory)
        t.back.size() > t.forward.size() ? (void)t.back.erase(t.back.begin()) : (void)t.forward.erase(t.forward.begin());
    t.view = to_mode(view_);
    t.sort = sort_;
    t.zoom_percent = std::clamp(int(std::lround(grid_zoom_ * 100.0)), 65, 180);
    t.filter = filter_.size() <= 256 ? filter_ : std::string();
    if (!in_recents() && query_.empty()) {
        FileView *v = files();
        for (int i : v->selection())
            if (t.selected.size() < Session::kMaxSelected && model_.entries[size_t(i)].name.find('/') == std::string::npos)
                t.selected.push_back(model_.entries[size_t(i)].name);
        if (v->current() >= 0 && v->current() < model_.count())
            t.current = model_.entries[size_t(v->current())].name;
        // Files marks the focused item on arrival; that mark alone is not kept.
        if (t.selected.size() == 1 && t.selected[0] == t.current)
            t.selected.clear();
    }
    return t;
}

void Pane::apply_tab_state(const TabState &t) {
    if (!is_dir(t.folder))
        return;
    view_ = from_mode(t.view);
    sort_ = t.sort;
    grid_zoom_ = std::clamp(t.zoom_percent / 100.0, kZoomMin, kZoomMax);
    grid_->zoom(grid_zoom_);
    navigate(t.folder, {}, false);
    // A saved folder view applied on arrival; the session's own state wins.
    if (!dual_)
        view_ = from_mode(t.view);
    sort_ = t.sort;
    grid_zoom_ = std::clamp(t.zoom_percent / 100.0, kZoomMin, kZoomMax);
    grid_->zoom(grid_zoom_);
    back_ = t.back;
    forward_ = t.forward;
    filter_ = t.filter;
    all_.clear();
    restore_marks_ = t.selected;
    restore_current_ = t.current;
    // The listing asked for by navigate() lands with these (filter, marks).
    order_for_view();
    files()->reload(true);
    after_folder_change();
}

void Pane::sizes_arrived() {
    if (view_ == View::Space && !dual_ && visible())
        space_->sync();
}

void Pane::sort_clicked(int column) {
    // Files' List header: name, size (biggest first), date (newest first,
    // then oldest), type.
    const auto &cols = list_header_columns();
    (void)cols;
    switch (column) {
    case 1: set_sort(SortMode::Size); break;
    case 2: set_sort(sort_ == SortMode::Newest ? SortMode::Oldest : SortMode::Newest); break;
    case 3: set_sort(SortMode::Kind); break;
    default: set_sort(SortMode::Name); break;
    }
    place_children();
}

std::string Pane::status_text() const {
    FileView *v = files();
    int marked = v->selection_count();
    std::string head = marked == 0 && v->current() < 0 ? std::string(fo::tr("No selection")) : fo::trn("%n marked", marked);
    std::string items = fo::trn("%n items", model_.count());
    if (!filter_.empty())
        items += " " + fo::i18n::arg(fo::tr("of %1"), std::to_string(all_.size()));
    std::string s = head + " \xC2\xB7 " + items;
    if (model_.loading)
        s += std::string(" \xC2\xB7 ") + fo::tr("loading\xE2\x80\xA6");
    return s;
}

} // namespace flo::ui

#include "ui/main_window.h"

#include "core/fsutil.h"
#include "core/network.h"
#include "core/path_completion.h"
#include "core/places.h"
#include "core/rail_order.h"
#include "core/shortcuts.h"
#include "core/settings.h"
#include "core/tree_size.h"
#include "core/xdg.h"
#include "ui/app.h"
#include "ui/commands.h"
#include "ui/details.h"
#include "ui/favorites.h"
#include "ui/operation_bar.h"
#include "ui/file_view.h"
#include "ui/pane.h"

#include "fltk-omarchy/app.h"
#include "fltk-omarchy/dialog.h"
#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/menu.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/shell.h"
#include "fltk-omarchy/sidebar.h"
#include "fltk-omarchy/text.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>
#include <FL/Fl_Input_.H>

#include <sys/statvfs.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace flo::ui {

namespace {

Pane::View view_from_settings(ViewMode m) {
    switch (m) {
    case ViewMode::Grid: return Pane::View::Grid;
    case ViewMode::List: return Pane::View::List;
    case ViewMode::Columns: return Pane::View::Columns;
    case ViewMode::Space: return Pane::View::Space;
    case ViewMode::Groups: return Pane::View::Groups;
    }
    return Pane::View::Groups;
}

// Files' header: the base right edge before the controls (toolbar.rs).
int base_right(int w) { return w - 12 - (w < 640 ? 5 : 10) * 40 + 4 - 14; }

// The filesystem type and free space under a folder, as Files' status bar
// shows them ("ext4 · 640 GB free").
std::string fs_summary(const std::string &folder) {
    if (folder.empty() || folder[0] != '/')
        return {};
    std::string type, best;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        // id parent major:minor root mountpoint options ... - fstype source
        size_t p = 0;
        for (int k = 0; k < 4 && p != std::string::npos; ++k)
            p = line.find(' ', p + 1);
        if (p == std::string::npos)
            continue;
        size_t e = line.find(' ', p + 1);
        std::string mp = line.substr(p + 1, e - p - 1);
        size_t dash = line.find(" - ");
        if (dash == std::string::npos)
            continue;
        size_t te = line.find(' ', dash + 3);
        std::string fs = line.substr(dash + 3, te - dash - 3);
        if (path_inside(folder, mp) && mp.size() >= best.size()) {
            best = mp;
            type = fs;
        }
    }
    struct statvfs sv;
    if (statvfs(folder.c_str(), &sv) != 0)
        return type;
    uint64_t free_b = uint64_t(sv.f_bavail) * sv.f_frsize;
    return (type.empty() ? std::string() : type + " \xC2\xB7 ") +
           fo::i18n::arg(fo::tr("%1 free"), si_size(free_b));
}

// The process's PSS, Files' live footer.
std::string pss_text() {
    std::ifstream in("/proc/self/smaps_rollup");
    std::string key;
    long kib = -1;
    while (in >> key) {
        if (key == "Pss:") {
            in >> kib;
            break;
        }
        std::string rest;
        std::getline(in, rest);
    }
    if (kib < 0)
        return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, "PSS %.1f MiB", kib / 1024.0);
    return buf;
}

// Files' status tips (status_tips.rs): each names its keys through the
// active shortcut profile; a command without a key there is reached from
// the command palette.
struct Tip {
    const char *text;
    std::vector<Cmd> keys; // fill %1, %2, ... in order
};
const std::vector<Tip> &tips() {
    static const std::vector<Tip> all = {
        {"%1: type >command or :place and press Enter to open the filtered catalog; then pick an action or a destination.",
         {Cmd::Location}},
        {"Home/End go to the ends. Shift extends the range; Ctrl moves the focus without replacing marks.", {}},
        {"Filter with *.rs or ext:pdf,md and press %1 to mark the matches. %2 clears the selection.",
         {Cmd::SelectFiltered, Cmd::ClearSelection}},
        {"%1 measures the listed folders and %2 the focused one; %3 reloads the folder.",
         {Cmd::SizesListed, Cmd::SizeFocused, Cmd::Refresh}},
        {"%1 filters names: *.pdf, ext:rs,md, type:dir, !draft. %2 marks and moves on; %3 marks without moving the focus.",
         {Cmd::Filter, Cmd::MarkAdvance, Cmd::MarkToggle}},
        {"File browser \xC2\xB7 Preferences (%1) \xC2\xB7 Actions (%2). %3 shows or hides hidden files.",
         {Cmd::Settings, Cmd::Actions, Cmd::Hidden}},
        {"%1 Groups, %2 List, %3 Columns, %4 Space and %5 Grid. Ctrl+wheel zooms the Grid.",
         {Cmd::ViewGroups, Cmd::ViewList, Cmd::ViewColumns, Cmd::ViewSpace, Cmd::ViewGrid}},
        {"%1 renames the focused item; %2 undoes it.", {Cmd::Rename, Cmd::Undo}},
        {"%1 previews the selected file; Esc returns to the folder. %2 opens the actions of the focused item.",
         {Cmd::Preview, Cmd::ContextMenu}},
    };
    return all;
}

} // namespace

// --- Construction ---------------------------------------------------------------

MainWindow::MainWindow(int W, int H, const std::string &folder, const std::string &select)
    : fo::Window(W, H, "Flo") {
    size_range(560, 400);
    const Settings &s = App::get().settings();
    details_on_ = s.details_pane;
    build_header();
    move_area([this](int x, int y) { return free_header(x, y); });
    tab_strip_ = new fo::TabStrip(0, kHeader, W, kTabs);
    rail_ = new fo::Sidebar(0, kTop, kRail, H - kTop - kStatus);
    rail_->rail(true, kRail, kRailWide);
    rail_->pinned(s.rail_pinned);
    details_ = new DetailsPane(W - kInspector, kTop, kInspector, H - kTop - kStatus);
    status_ = new fo::StatusBar(0, H - kStatus, W, kStatus);
    opbar_ = new OperationBar(0, H - kStatus - OperationBar::kHeight, W, OperationBar::kHeight);
    opbar_->reason([this](int i) { return operation_reason(i); });
    opbar_->on_run([this](int i) { run_operation(i); });
    if (!s.operation_bar)
        opbar_->hide();
    end();
    resizable(nullptr);
    tabs_.push_back(make_pane(folder, int(view_from_settings(s.view))));

    tab_strip_->on_select([this](int i) { select_tab(i); });
    tab_strip_->on_close([this](int i) { defer([this, i] { close_tab(i); }); });
    tab_strip_->on_new([this] { new_tab(active_pane()->folder()); });
    tab_strip_->on_reorder([this](int from, int to) { move_tab(from, to); });
    tab_strip_->on_context([this](int i, int x, int y) {
        // Files' tab menu (tab_context.rs), then Flo's New / Reopen.
        const int n = int(tabs_.size());
        std::vector<fo::MenuItem> m;
        m.push_back(fo::MenuItem(fo::tr("Close tab"), "Ctrl+W", [this, i] { defer([this, i] { close_tab(i); }); }));
        m.push_back(fo::MenuItem(fo::tr("Close other tabs"), "", [this, i] { defer([this, i] { close_other_tabs(i); }); })
                        .disabled(n <= 1));
        m.push_back(fo::MenuItem(fo::tr("Close tabs to the right"), "",
                                 [this, i] { defer([this, i] { close_tabs_right(i); }); })
                        .disabled(i + 1 >= n));
        m.push_back(fo::MenuItem(fo::tr("Move left"), "Ctrl+Shift+PgUp", [this, i] { move_tab(i, i - 1); })
                        .disabled(i <= 0));
        m.push_back(fo::MenuItem(fo::tr("Move right"), "Ctrl+Shift+PgDn", [this, i] { move_tab(i, i + 1); })
                        .disabled(i + 1 >= n));
        m.push_back(fo::MenuItem::separator());
        m.push_back(fo::MenuItem(fo::tr("New tab"), "Ctrl+T", [this] { new_tab(active_pane()->folder()); }));
        m.push_back(fo::MenuItem(fo::tr("Reopen closed tab"), "Ctrl+Shift+T", [this] { reopen_tab(); })
                        .disabled(closed_tabs_.empty()));
        remap_menu_keys(m);
        fo::PopupMenu::open_at(this, x, y, m);
    });
    status_->on_hint([this] { edit_path(); });

    rail_->on_width([this] { layout(); });
    rail_->on_pin([](bool on) {
        App::get().settings().rail_pinned = on;
        App::get().save_settings();
    });
    rail_->on_activate([this](const fo::SidebarRow &r) { activate_row(r); });
    // A row that opened the location field ("Connect…") keeps the keyboard there.
    rail_->on_finished([this] {
        if (!typing())
            active_pane()->focus_view();
    });
    rail_->on_eject([this](const fo::SidebarRow &r) {
        std::string path = r.id;
        // Network locations disconnect (Files: Desconectar).
        for (const auto &s : App::get().places().shares)
            if (s.path == path || (path.compare(0, 8, "tailnet:") == 0 && s.protocol == "SFTP" &&
                                   s.title == path.substr(8))) {
                disconnect_share(s.path);
                return;
            }
        auto done = [this, path](bool ok, const std::string &, const std::string &error) {
            fo::Toast::show(this, ok ? fo::i18n::arg(fo::tr("You can remove %1 now"), path_basename(path))
                                     : (error.empty() ? fo::tr("The drive is busy") : error),
                            3, ok ? fo::Toast::Info : fo::Toast::Error);
            for (Pane *p : tabs_)
                if (path_inside(p->folder(), path))
                    p->go_home();
            if (dual_ && path_inside(dual_->folder(), path))
                dual_->go_home();
        };
        App::get().devices().eject(path, done);
    });
    rail_->on_context([this](const fo::SidebarRow &r, int x, int y) { sidebar_context(r, x, y); });
    rail_->accepts_drop([](const fo::SidebarRow &r) { return !r.id.empty() && r.id[0] == '/'; });
    rail_->accepts_reorder([this](const fo::SidebarRow &r) {
        return r.section == places_sec_ || r.section == marks_sec_ || r.section == drives_sec_;
    });
    rail_->on_reorder([this](const fo::SidebarRow &r, int index) { reorder_row(r, index); });
    rail_->on_drop([this](const fo::SidebarRow &r, const std::string &text) {
        bool cut = false;
        active_pane()->drop_into(App::get().paths_from_clipboard(text, cut), r.id);
    });

    build_shortcuts();
    rebuild_sidebar();
    update_tabs();
    update_header();
    layout();
    if (!select.empty())
        tabs_[0]->select_path(path_join(folder, select));
    Fl::add_timeout(0.5, pss_tick, this);
}

MainWindow::~MainWindow() {
    Fl::remove_timeout(nullptr, this);
    Fl::remove_timeout(pss_tick, this);
    // Panes go first, while the header and inspector they talk to exist.
    std::vector<Pane *> all = tabs_;
    if (dual_)
        all.push_back(dual_);
    tabs_.clear();
    dual_ = nullptr;
    for (Pane *p : all) {
        remove(p);
        delete p;
    }
}

Pane *MainWindow::make_pane(const std::string &folder, int view) {
    Fl_Group::current(nullptr);
    auto *p = new Pane(this, kRail, kTop, std::max(100, w() - kRail), std::max(100, h() - kTop - kStatus), folder,
                       Pane::View(view));
    insert(*p, 0);
    return p;
}

void MainWindow::build_header() {
    nav_ = new fo::ToolBar(12, 13, 102, 32);
    nav_->plain(true);
    nav_->metrics(30, 6);
    crumbs_ = new fo::Breadcrumbs(136, 12, 300, 34);
    field_ = new fo::TextField(124, 10, 300, 38);
    field_->hide();
    field_ok_ = new fo::IconButton(0, 10, 36, 36, "check", nullptr);
    field_cancel_ = new fo::IconButton(0, 10, 36, 36, "x", nullptr);
    field_ok_->hide();
    field_cancel_->hide();
    tools_ = new fo::ToolBar(400, 10, 300, 36);
    tools_->on_relayout([this] { layout(); });

    crumbs_->on_navigate([this](int i) {
        const auto &segs = crumbs_->segments();
        if (i >= 0 && i + 1 < int(segs.size()))
            active_pane()->navigate(segs[size_t(i)].path);
    });
    crumbs_->on_empty_click([this] { edit_path(); });
    field_->on_escape([this] {
        if (field_mode_ == Field::Filter)
            active_pane()->set_filter("");
        else if (field_mode_ == Field::Search)
            active_pane()->set_query("");
        end_field();
    });
    field_->on_change([this] {
        if (field_mode_ == Field::Filter)
            active_pane()->set_filter(field_->value());
    });
    field_->on_enter([this] {
        std::string text = field_->value();
        if (field_mode_ == Field::Path) {
            run_omnibar(text);
        } else if (field_mode_ == Field::Search) {
            active_pane()->set_query(text);
            active_pane()->focus_view();
        } else {
            active_pane()->focus_view(); // Files: Enter returns to the list
        }
    });
    // Files' line editing: components between "/" for Ctrl+arrows,
    // Ctrl+Backspace/Delete and double clicks.
    field_->separators("/");
    field_->on_key([this](int k) {
        if (field_mode_ == Field::Path)
            return path_key(k);
        if ((k == FL_Down || k == FL_Tab) && field_mode_ != Field::Path) {
            active_pane()->focus_view();
            return true;
        }
        return false;
    });
    field_ok_->on_click([this] { run_omnibar(field_->value()); });
    field_cancel_->on_click([this] { end_field(); });
}

bool MainWindow::free_header(int x, int y) const {
    if (x < 0 || y < 0 || x >= w() || y >= kHeader)
        return false;
    auto inside = [x, y](int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; };
    if (inside(address_x_, 10, address_w_, 38))
        return false;
    for (const Fl_Widget *wd : {static_cast<const Fl_Widget *>(field_), static_cast<const Fl_Widget *>(crumbs_),
                                static_cast<const Fl_Widget *>(field_ok_), static_cast<const Fl_Widget *>(field_cancel_)})
        if (wd->visible() && inside(wd->x(), wd->y(), wd->w(), wd->h()))
            return false;
    for (fo::ToolBar *bar : {nav_, tools_}) {
        if (!bar->visible())
            continue;
        for (int i = 0; i < int(bar->items().size()); ++i) {
            int bx, by, bw, bh;
            if (bar->item_rect(i, bx, by, bw, bh) && inside(bx, by, bw, bh))
                return false;
        }
    }
    return true;
}

// --- Panes and tabs ----------------------------------------------------------------

int MainWindow::index_of(const Pane *p) const {
    if (p && !tabs_.empty() && p == tabs_[size_t(tab_)])
        return 0;
    if (p && p == dual_)
        return 1;
    return -1;
}

Pane *MainWindow::other_pane(const Pane *p) const {
    if (!dual_)
        return nullptr;
    return p == dual_ ? tabs_[size_t(tab_)] : dual_;
}

void MainWindow::set_active(Pane *p) {
    int i = index_of(p);
    if (i < 0)
        return;
    bool changed = i != active_;
    active_ = i;
    if (changed) {
        if (field_mode_ != Field::None)
            end_field(false);
        if (tabs_[size_t(tab_)])
            tabs_[size_t(tab_)]->redraw();
        if (dual_)
            dual_->redraw();
        pane_moved(p);
        pane_selected(p);
    }
}

void MainWindow::new_tab(const std::string &folder, bool background) {
    if (int(tabs_.size()) >= kMaxTabs) {
        fo::Toast::show(this, fo::i18n::arg(fo::tr("Up to %1 tabs"), std::to_string(kMaxTabs)), 2);
        return;
    }
    Pane *from = active_pane();
    Pane *p = make_pane(folder.empty() ? from->folder() : folder, int(from->view()));
    p->set_grid_zoom(from->grid_zoom());
    tabs_.insert(tabs_.begin() + tab_ + 1, p);
    if (background) {
        p->hide();
        update_tabs();
        status_->left(fo::tr("Tab created"));
        return;
    }
    select_tab(tab_ + 1);
}

void MainWindow::select_tab(int i) {
    if (i < 0 || i >= int(tabs_.size()))
        return;
    if (field_mode_ != Field::None)
        end_field(false);
    tab_ = i;
    active_ = 0;
    for (size_t k = 0; k < tabs_.size(); ++k)
        int(k) == tab_ ? tabs_[k]->show() : tabs_[k]->hide();
    layout();
    update_tabs();
    pane_moved(tabs_[size_t(tab_)]);
    pane_selected(tabs_[size_t(tab_)]);
    tabs_[size_t(tab_)]->focus_view();
}

void MainWindow::move_tab(int from, int to) {
    const int n = int(tabs_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    Pane *active = tabs_[size_t(tab_)];
    Pane *moved = tabs_[size_t(from)];
    tabs_.erase(tabs_.begin() + from);
    tabs_.insert(tabs_.begin() + to, moved);
    tab_ = int(std::find(tabs_.begin(), tabs_.end(), active) - tabs_.begin());
    update_tabs();
}

void MainWindow::close_tab(int i) {
    if (i < 0 || i >= int(tabs_.size()))
        return;
    if (tabs_.size() == 1) {
        defer([this] { hide(); }); // the last tab closes the window
        return;
    }
    Pane *p = tabs_[size_t(i)];
    closed_tabs_.push_back(p->folder());
    if (closed_tabs_.size() > 16)
        closed_tabs_.erase(closed_tabs_.begin());
    if (App::get().preview().owner() == p)
        App::get().preview().close();
    tabs_.erase(tabs_.begin() + i);
    remove(p);
    Fl::delete_widget(p);
    if (tab_ >= int(tabs_.size()) || i < tab_)
        tab_ = std::max(0, tab_ - 1);
    select_tab(tab_);
}

void MainWindow::close_tabs_right(int i) {
    while (i >= 0 && i + 1 < int(tabs_.size()))
        close_tab(int(tabs_.size()) - 1);
}

void MainWindow::close_other_tabs(int i) {
    if (i < 0 || i >= int(tabs_.size()))
        return;
    close_tabs_right(i);
    for (int k = 0; k < i; ++k)
        close_tab(0);
}

void MainWindow::reopen_tab() {
    if (closed_tabs_.empty())
        return;
    std::string f = closed_tabs_.back();
    closed_tabs_.pop_back();
    new_tab(f);
}

Session MainWindow::session_state() const {
    Session s;
    for (Pane *p : tabs_)
        if (s.tabs.size() < Session::kMaxTabs)
            s.tabs.push_back(p->tab_state());
    s.active = std::min(tab_, int(s.tabs.size()) - 1);
    for (const auto &c : closed_tabs_)
        if (!c.empty() && c[0] == '/')
            s.closed.push_back(c);
    while (s.closed.size() > Session::kMaxClosed)
        s.closed.erase(s.closed.begin());
    return s;
}

void MainWindow::restore_session(const Session &s) {
    std::vector<const TabState *> ok;
    for (const auto &t : s.tabs)
        if (is_dir(t.folder))
            ok.push_back(&t);
    if (ok.empty())
        return;
    int active = 0;
    for (size_t i = 0; i < ok.size(); ++i)
        if (ok[i] == &s.tabs[size_t(std::clamp(s.active, 0, int(s.tabs.size()) - 1))])
            active = int(i);
    tabs_[0]->apply_tab_state(*ok[0]);
    for (size_t i = 1; i < ok.size() && int(tabs_.size()) < kMaxTabs; ++i) {
        Pane *p = make_pane(ok[i]->folder, int(Pane::from_mode(ok[i]->view)));
        p->apply_tab_state(*ok[i]);
        p->hide();
        tabs_.push_back(p);
    }
    closed_tabs_ = s.closed;
    select_tab(std::min(active, int(tabs_.size()) - 1));
    if (!active_pane()->filter().empty()) {
        field_mode_ = Field::Filter;
        field_->placeholder(fo::tr("Type to filter: *.pdf, ext:md, type:dir, !draft"));
        field_->set_text(active_pane()->filter());
        layout();
        update_header();
    }
}

void MainWindow::update_tabs() {
    std::vector<std::string> labels;
    for (Pane *p : tabs_)
        labels.push_back(p->title());
    tab_strip_->tabs(labels, tab_);
}

void MainWindow::toggle_dual() { dual_ ? close_split() : open_split(); }

void MainWindow::open_split() {
    if (dual_) {
        dual_->focus_view();
        return;
    }
    Pane *from = tabs_[size_t(tab_)];
    Pane *other = from;
    dual_ = make_pane(other->folder(), int(Pane::View::List));
    dual_->dual(true);
    from->dual(true);
    if (field_mode_ != Field::None)
        end_field(false);
    layout();
    dual_->focus_view();
    update_header();
}

void MainWindow::close_split() {
    if (!dual_)
        return;
    Pane *p = dual_;
    dual_ = nullptr;
    active_ = 0;
    if (App::get().preview().owner() == p)
        App::get().preview().close();
    remove(p);
    p->hide();
    Fl::delete_widget(p);
    tabs_[size_t(tab_)]->dual(false);
    layout();
    tabs_[size_t(tab_)]->focus_view();
    pane_moved(tabs_[size_t(tab_)]);
}

void MainWindow::toggle_details() {
    details_on_ = !details_on_;
    App::get().settings().details_pane = details_on_;
    App::get().save_settings();
    layout();
    update_header();
    details_->show_from(active_pane());
}

// --- Layout ---------------------------------------------------------------------

void MainWindow::relayout_frame(void *data) { static_cast<MainWindow *>(data)->layout(); }

void MainWindow::layout() {
    if (tabs_.empty())
        return;
    const int W = w(), H = h();
    int rw = rail_->desired_width();
    const int ob = opbar_height();
    int body = std::max(40, H - kTop - kStatus - ob);
    rail_->resize(0, kTop, rw, body);
    tab_strip_->resize(0, kHeader, W, kTabs);
    status_->resize(0, H - kStatus, W, kStatus);
    if (ob) {
        opbar_->resize(0, H - kStatus - ob, W, ob);
        opbar_->space(active_pane()->view() == Pane::View::Space && !split());
        opbar_->show();
    } else {
        opbar_->hide();
    }
    bool insp = details_on_ && !dual_ && W >= 800;
    int iw = insp ? kInspector : 0;
    details_->resize(W - kInspector, kTop, kInspector, body);
    insp ? details_->show() : details_->hide();
    int cx = rw, cw = std::max(100, W - rw - iw);
    Pane *main = tabs_[size_t(tab_)];
    if (!dual_) {
        main->resize(cx, kTop, cw, body);
    } else {
        int half = cw / 2;
        main->resize(cx, kTop, half, body);
        dual_->resize(cx + half + 1, kTop, cw - half - 1, body);
    }
    for (Pane *p : tabs_)
        if (p != main)
            p->resize(cx, kTop, cw, body);

    // The header (toolbar.rs): back/forward/up, the address, the controls.
    nav_->resize(12, 13, 102, 32);
    if (field_mode_ == Field::Path) {
        tools_->hide();
        crumbs_->hide();
        field_->resize(124, 10, std::max(1, W - 228), 38);
        address_w_ = field_->w();
        field_->show();
        field_ok_->resize(W - 84, 10, 36, 36);
        field_cancel_->resize(W - 44, 10, 36, 36);
        field_ok_->show();
        field_cancel_->show();
    } else {
        field_ok_->hide();
        field_cancel_->hide();
        tools_->show();
        tools_->allowance(std::max(0, std::min(160, base_right(W) - 176)));
        int pw = tools_->preferred_width();
        tools_->resize(W - 12 - pw, 10, pw, 36);
        int omni_w = std::max(1, W - 12 - pw - 14 - 132);
        address_w_ = omni_w;
        if (field_mode_ == Field::Filter || field_mode_ == Field::Search) {
            crumbs_->hide();
            field_->resize(124, 10, omni_w, 38);
            field_->show();
        } else {
            field_->hide();
            crumbs_->resize(136, 12, std::max(1, omni_w - 24), 34);
            crumbs_->show();
        }
    }
    redraw();
}

void MainWindow::resize(int X, int Y, int W, int H) {
    bool narrow_changed = (W < 640) != (w() < 640);
    fo::Window::resize(X, Y, W, H);
    if (narrow_changed)
        update_header();
    layout();
}

void MainWindow::draw() {
    fo::Window::draw();
    // Between the two panes of the dual view: a card-colored line.
    if (dual_ && !top_layer()) {
        const fo::Theme &t = fo::theme();
        fo::paint::vline(dual_->x() - 1, kTop, h() - kTop - kStatus - opbar_height(), t.card());
    }
}

int MainWindow::handle(int event) { return fo::Window::handle(event); }

// --- Files' MC operation bar ------------------------------------------------------------

int MainWindow::opbar_height() const { return App::get().settings().operation_bar ? OperationBar::kHeight : 0; }

void MainWindow::toggle_operation_bar() {
    if (modal_open())
        return;
    Settings &s = App::get().settings();
    s.operation_bar = !s.operation_bar;
    App::get().save_settings();
    layout();
}

// Files' operation_bar::reason, for what Flo has.
std::string MainWindow::operation_reason(int i) {
    if (modal_open())
        return fo::tr("Close the dialog or tool first");
    if (field_mode_ != Field::None)
        return fo::tr("Enter: back to the list");
    if (i == 0 || i == 7)
        return {};
    Pane *p = active_pane();
    if (p->loading())
        return fo::tr("Wait for the load");
    if (i == 3 && p->view() == Pane::View::Space && !split())
        return {};
    if (i == 2) {
        if (!p->current_entry())
            return fo::tr("Focus an item first");
    } else if (i != 5 && p->selected_paths().empty()) {
        return fo::tr("Select at least one item");
    }
    return {};
}

void MainWindow::run_operation(int i) {
    std::string why = operation_reason(i);
    if (!why.empty()) {
        if (!modal_open())
            fo::Toast::show(this, "MC: " + why, 2.5);
        return;
    }
    Pane *p = active_pane();
    switch (i) {
    case 0: show_help(); break;
    case 1: p->rename(); break;
    case 2: p->preview(); break;
    case 3:
        if (p->view() == Pane::View::Space && !split())
            p->measure_listed();
        else
            p->transfer(false);
        break;
    case 4: p->transfer(true); break;
    case 5: p->new_folder(); break;
    case 6: p->delete_selected(); break;
    case 7: actions_menu(); break;
    }
    opbar_->redraw();
}

void MainWindow::hide() {
    if (session_owner_ && !tabs_.empty())
        session_state().save(); // Files saves the session when its window closes
    if (shown() && !maximize_active()) {
        App::get().settings().window_width = w();
        App::get().settings().window_height = h();
    }
    fo::Window::hide();
    App::get().window_closed(this);
}

// --- Following the panes -------------------------------------------------------------

void MainWindow::update_header() {
    Pane *p = active_pane();
    if (!p)
        return;
    nav_->items({
        {"arrow-left", fo::tr("Back"), key(Cmd::Back), false, p->can_back(), [this] { active_pane()->back(); }},
        {"arrow-right", fo::tr("Forward"), key(Cmd::Forward), false, p->can_forward(), [this] { active_pane()->forward(); }},
        {"arrow-up", fo::tr("Up"), key(Cmd::Parent), false, p->can_go_up(), [this] { active_pane()->go_up(); }},
    });
    using V = Pane::View;
    auto view = [this](V v) {
        return [this, v] {
            active_pane()->set_view(v);
            active_pane()->focus_view();
        };
    };
    V v = p->view();
    bool filtered = !p->filter().empty();
    std::vector<fo::ToolBar::Item> items = {
        {filtered ? "x" : "search", filtered ? fo::tr("Clear filter") : fo::tr("Search"), filtered ? std::string("Esc") : key(Cmd::Filter),
         filtered || field_mode_ == Field::Filter, true,
         [this, filtered] {
             if (filtered) {
                 active_pane()->set_filter("");
                 end_field();
             } else {
                 begin_filter();
             }
         }},
        {"pencil", fo::tr("Edit path"), key(Cmd::Location), false, true, [this] { edit_path(); }},
    };
    if (w() < 640) {
        items.push_back({"groups", fo::tr("Views"), "", false, true, [this] {
                             std::vector<fo::MenuItem> m;
                             const char *names[] = {"Groups", "List", "Columns", "Space", "Grid"};
                             const Cmd cmds[] = {Cmd::ViewGroups, Cmd::ViewList, Cmd::ViewColumns, Cmd::ViewSpace,
                                                 Cmd::ViewGrid};
                             for (int k = 0; k < 5; ++k)
                                 m.push_back(fo::MenuItem(fo::tr(names[k]), primary_chord(ShortcutProfile::Files, cmds[k]), [this, k] {
                                                 active_pane()->set_view(V(k));
                                             }).check(active_pane()->view() == V(k)));
                             open_menu_under(2, m);
                         }});
    } else {
        items.push_back({"groups", fo::tr("Groups"), key(Cmd::ViewGroups), v == V::Groups, true, view(V::Groups)});
        items.push_back({"list", fo::tr("List"), key(Cmd::ViewList), v == V::List, true, view(V::List)});
        items.push_back({"grid", fo::tr("Grid"), key(Cmd::ViewGrid), v == V::Grid, true, view(V::Grid)});
        items.push_back({"space", fo::tr("Space"), key(Cmd::ViewSpace), v == V::Space, true, view(V::Space)});
        items.push_back({"columns", fo::tr("Columns"), key(Cmd::ViewColumns), v == V::Columns, true, view(V::Columns)});
        items.push_back({"info", fo::tr("Inspector"), key(Cmd::Inspector), details_on_ || dual_, true,
                         [this] { toggle_details(); }});
    }
    items.push_back({"actions", fo::tr("Actions"), key(Cmd::Actions), false, true, [this] { actions_menu(); }});
    items.push_back({"settings", fo::tr("Preferences"), key(Cmd::Settings), false, true, [this] { preferences_menu(); }});
    tools_->items(items);
    std::vector<fo::Breadcrumbs::Segment> segs;
    for (const auto &c : p->crumbs())
        segs.push_back({c.first, c.second});
    crumbs_->segments(segs);
    layout();
}

void MainWindow::pane_moved(Pane *p) {
    update_tabs();
    if (p != active_pane())
        return;
    copy_label(("Flo \xC2\xB7 " + p->title()).c_str());
    rail_->set_active(place_for(p->folder()));
    details_->show_from(p);
    update_header();
    update_status();
}

void MainWindow::pane_selected(Pane *p) {
    if (!p || p != active_pane())
        return;
    if (p == active_pane())
        details_->show_from(p);
    update_status();
}

void MainWindow::pane_view_changed(Pane *p) {
    if (p == active_pane())
        update_header();
}

void MainWindow::sizes_changed() {
    for (Pane *p : tabs_)
        if (p->visible()) {
            p->sizes_arrived();
            p->redraw();
        }
    if (dual_)
        dual_->redraw();
    details_->show_from(active_pane());
}

void MainWindow::update_status() {
    Pane *p = active_pane();
    if (!p)
        return;
    status_->left(p->status_text());
    const Tip &tip = tips()[size_t(tip_) % tips().size()];
    std::string text = fo::tr(tip.text);
    for (Cmd c : tip.keys) {
        std::string k = key(c);
        text = fo::i18n::arg(text, k.empty() ? fo::tr("the command palette") : k);
    }
    status_->hint(text);
    status_->right(fs_summary(is_virtual_folder(p->folder()) ? xdg::home() : p->folder()));
    if (opbar_ && opbar_->visible()) {
        opbar_->space(p->view() == Pane::View::Space && !split());
        opbar_->redraw(); // the buttons follow the selection
    }
}

void MainWindow::pss_tick(void *data) {
    auto *w = static_cast<MainWindow *>(data);
    std::string s = pss_text();
    if (!s.empty())
        w->status_->badge(s, fo::theme().green());
    // Files moves on to the next tip now and then (every 15 s here).
    if (++w->tip_ticks_ % 5 == 0) {
        ++w->tip_;
        w->update_status();
    }
    Fl::repeat_timeout(3.0, pss_tick, data);
}

void MainWindow::language_changed() {
    for (Pane *p : tabs_)
        p->relabel();
    if (dual_)
        dual_->relabel();
    rebuild_sidebar();
    details_->relabel();
    pane_moved(active_pane());
    update_status(); // the tip and the counts in the new language
    redraw();
}

// --- The header's field ----------------------------------------------------------

bool MainWindow::typing() const { return field_mode_ != Field::None && Fl::focus() == field_; }

void MainWindow::edit_path() {
    field_mode_ = Field::Path;
    field_->placeholder(fo::tr("A path, >command or :place"));
    field_->set_text(active_pane()->typed_path());
    layout();
    field_->take_focus();
}

void MainWindow::connect_server() {
    // Files' connect_server_prompt (network_ui.rs): the location field
    // starts with smb://, the cursor at its end.
    const std::string start = "smb://";
    field_mode_ = Field::Path;
    field_->placeholder(fo::tr("A path, >command or :place"));
    field_->set_text(start, int(start.size()), int(start.size()));
    layout();
    field_->take_focus();
    status_->left(fo::tr("Type smb://, sftp://, ftp://, dav:// or nfs:// and the server \xC2\xB7 Enter connects"));
}

// --- Files' Tab completion in the location field (path_edit.rs) -------------------

bool MainWindow::path_key(int k) {
    const int st = Fl::event_state();
    const bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT, alt = st & FL_ALT;
    const bool suggesting = completion_ && !completion_->items.empty();
    if (k == FL_Tab && !ctrl && !alt) {
        complete_field(shift);
        return true;
    }
    if ((k == FL_Up || k == FL_Down) && suggesting) {
        complete_field(k == FL_Up);
        return true;
    }
    if (k == FL_Escape && completion_) {
        close_path_completion();
        return true;
    }
    if ((k == FL_Enter || k == FL_KP_Enter) && completion_ && completion_->index >= 0) {
        // Accept the highlighted candidate; a second Enter goes there.
        close_path_completion();
        return true;
    }
    switch (k) {
    case FL_Up: case FL_Down: case FL_Page_Up: case FL_Page_Down:
    case FL_Shift_L: case FL_Shift_R: case FL_Control_L: case FL_Control_R:
    case FL_Alt_L: case FL_Alt_R: case FL_Meta_L: case FL_Meta_R: case FL_Caps_Lock:
        break; // Files: these keep the candidates
    default:
        close_path_completion();
    }
    return false;
}

void MainWindow::set_field_text(const std::string &text) {
    // An edit, not value(): Ctrl+Z takes a completion back.
    if (text != field_->value())
        field_->replace(0, field_->size(), text.data(), int(text.size()));
    field_->insert_position(field_->size());
}

void MainWindow::show_completion(const std::string &notice) { status_->left(notice); }

void MainWindow::close_path_completion() {
    if (!completion_)
        return;
    completion_.reset();
    update_status();
}

void MainWindow::complete_field(bool backwards) {
    if (completion_ && completion_->items.size() > 1) {
        const int n = int(completion_->items.size()), i = completion_->index;
        completion_->index = i < 0 ? (backwards ? n - 1 : 0) : backwards ? (i + n - 1) % n : (i + 1) % n;
        set_field_text(completion_->text(completion_->index));
        show_completion(completion_->render());
        return;
    }
    close_path_completion();
    const std::string text = field_->value();
    if (text.empty() || text.find("://") != std::string::npos || text[0] == '>' || text[0] == ':' ||
        std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
        return;
    if (text == "~") {
        set_field_text("~/");
        return;
    }
    std::string base, partial;
    split_component(text, base, partial);
    const std::string cwd = active_pane()->folder();
    std::string dir = base.empty() ? cwd : expand_typed(base, cwd);
    if (dir.empty() || is_virtual_folder(dir))
        return;
    if (network_location(dir)) {
        completion_ = PathCompletion{};
        show_completion(fo::tr("Tab: no completion in network locations"));
        return;
    }
    std::vector<PathCandidate> found = completion_matches(read_candidates(dir, partial), partial);
    if (found.empty()) {
        completion_ = PathCompletion{};
        show_completion(fo::tr("Tab: no matches"));
        return;
    }
    if (found.size() == 1) {
        set_field_text(base + found[0].name + (found[0].dir ? "/" : ""));
        return;
    }
    bool insensitive = std::any_of(found.begin(), found.end(), [&](const PathCandidate &c) {
        return c.name.compare(0, partial.size(), partial) != 0;
    });
    std::string prefix = common_prefix(found, insensitive);
    if (prefix.size() >= partial.size() && prefix != partial)
        set_field_text(base + prefix);
    completion_ = PathCompletion{base, std::move(found), -1};
    show_completion(completion_->render());
}

void MainWindow::begin_filter(const std::string &text) {
    Pane *p = active_pane();
    if (p->in_recents() && text.empty())
        return;
    field_mode_ = Field::Filter;
    field_->placeholder(fo::tr("Type to filter: *.pdf, ext:md, type:dir, !draft"));
    std::string start = text.empty() ? p->filter() : p->filter() + text;
    field_->set_text(start, int(start.size()), int(start.size()));
    layout();
    field_->take_focus();
    if (!text.empty())
        p->set_filter(start);
    update_header();
}

void MainWindow::begin_search() {
    field_mode_ = Field::Search;
    field_->placeholder(fo::i18n::arg(fo::tr("Search %1 and below"), active_pane()->title()));
    field_->set_text(active_pane()->query());
    layout();
    field_->take_focus();
}

void MainWindow::end_field(bool refocus) {
    close_path_completion();
    if (field_mode_ == Field::None)
        return;
    bool keep = field_mode_ == Field::Filter && !active_pane()->filter().empty();
    field_mode_ = keep ? Field::Filter : Field::None;
    if (!keep)
        field_->value("");
    layout();
    update_header();
    if (refocus)
        active_pane()->focus_view();
}

void MainWindow::run_omnibar(const std::string &text) {
    if (!text.empty() && text[0] == '>' ) {
        std::string q = text.substr(1);
        end_field(false);
        std::vector<Command> cmds;
        auto add = [&](const std::vector<fo::MenuItem> &items, auto &self) -> void {
            for (const auto &it : items) {
                if (it.is_separator)
                    continue;
                if (!it.submenu.empty())
                    self(it.submenu, self);
                else if (it.action)
                    cmds.push_back({it.label, it.shortcut, it.icon, it.action, it.enabled});
            }
        };
        std::vector<fo::MenuItem> actions = actions_items(), prefs = preferences_items();
        remap_menu_keys(actions);
        remap_menu_keys(prefs);
        add(actions, add);
        add(prefs, add);
        CommandPalette::open(this, fo::tr("Commands"), fo::tr("Search command or shortcut\xE2\x80\xA6"), cmds, q);
        return;
    }
    if (!text.empty() && text[0] == ':') {
        std::string q = text.substr(1);
        end_field(false);
        std::vector<Command> places;
        for (const auto &r : rail_->rows()) {
            if (r.id.empty())
                continue;
            fo::SidebarRow row = r;
            places.push_back({r.label, r.id[0] == '/' ? tilde(r.id) : std::string(), r.icon,
                              [this, row] { activate_row(row); }, true});
        }
        CommandPalette::open(this, fo::tr("Places"), fo::tr("Search a place\xE2\x80\xA6"), places, q);
        return;
    }
    // "./>name" and "./:name" stay literal paths.
    if (active_pane()->go_typed(text))
        end_field();
}

// --- Menus --------------------------------------------------------------------------

void remap_menu_keys(std::vector<fo::MenuItem> &items) {
    const ShortcutProfile p = App::get().settings().shortcuts;
    if (p == ShortcutProfile::Files)
        return;
    for (auto &it : items) {
        it.shortcut = remap_chord(p, it.shortcut);
        remap_menu_keys(it.submenu);
    }
}

void MainWindow::open_menu_under(int item, std::vector<fo::MenuItem> items) {
    int x, y, w, h;
    if (!tools_->item_rect(item, x, y, w, h))
        return;
    remap_menu_keys(items);
    fo::PopupMenu::open_below(this, x, y, w, h, std::move(items), true);
}

std::vector<fo::MenuItem> MainWindow::actions_items() {
    using fo::MenuItem;
    Pane *p = active_pane();
    auto P = [this] { return active_pane(); };
    bool has = !p->selected_paths().empty();
    std::vector<MenuItem> create = {
        MenuItem(fo::tr("Create file"), "Ctrl+N", [P] { P()->new_file(); }).with_icon("file"),
        MenuItem(fo::tr("Create folder"), "F7", [P] { P()->new_folder(); }).with_icon("folder"),
        MenuItem(fo::tr("Rename"), "F2", [P] { P()->rename(); }).with_icon("edit").disabled(!has),
        MenuItem(fo::tr("Duplicate"), "", [P] { P()->duplicate(); }).with_icon("copy").disabled(!has),
        MenuItem(fo::tr("Send to Trash"), "Delete", [P] { P()->delete_selected(); }).with_icon("trash").disabled(!has),
        MenuItem(fo::tr("Compress\xE2\x80\xA6"), "", [P] { P()->compress(); }).with_icon("compress").disabled(!has),
    };
    std::vector<MenuItem> clip = {
        MenuItem(fo::tr("Copy"), "Ctrl+C", [P] { P()->copy(false); }).with_icon("copy").disabled(!has),
        MenuItem(fo::tr("Cut"), "Ctrl+X", [P] { P()->copy(true); }).with_icon("cut").disabled(!has),
        MenuItem(fo::tr("Paste"), "Ctrl+V", [P] { P()->paste(); }).with_icon("paste"),
        MenuItem(fo::tr("Copy path"), "Ctrl+Alt+C", [P] { P()->copy_path(); }).with_icon("link"),
        MenuItem(fo::tr("Copy to\xE2\x80\xA6"), "F5", [P] { P()->transfer(false); }).with_icon("copy").disabled(!has),
        MenuItem(fo::tr("Move to\xE2\x80\xA6"), "F6", [P] { P()->transfer(true); }).with_icon("arrow-right").disabled(!has),
    };
    std::vector<MenuItem> sel = {
        MenuItem(fo::tr("Select all"), "Ctrl+A", [P] { P()->select_all(); }).with_icon("square-check"),
        MenuItem(fo::tr("Invert selection"), "Ctrl+I", [P] { P()->invert_selection(); }).with_icon("select"),
        MenuItem(fo::tr("Clear selection"), "Ctrl+Shift+D", [P] { P()->clear_selection(); }).with_icon("x"),
        MenuItem(fo::tr("Mark filtered and show all"), "Ctrl+Shift+A", [P] { P()->select_filtered(); })
            .with_icon("filter")
            .disabled(p->filter().empty()),
        MenuItem(fo::tr("Mark and advance"), "Insert", [P] { P()->mark_and_advance(); }).with_icon("check"),
    };
    std::vector<MenuItem> find = {
        MenuItem(fo::tr("Search in subfolders"), "Ctrl+Shift+F", [this] { begin_search(); }).with_icon("search"),
        MenuItem(fo::tr("Filter this folder"), "Ctrl+F", [this] { begin_filter(); }).with_icon("filter"),
        MenuItem(fo::tr("Preview"), "Space", [P] { P()->preview(); }).with_icon("eye").disabled(!has),
        MenuItem(fo::tr("Open with\xE2\x80\xA6"), "Ctrl+Shift+O", [P] { P()->menu_from_keyboard(); })
            .with_icon("external-link")
            .disabled(!has),
        MenuItem(fo::tr("Properties and permissions"), "Alt+Enter", [P] { P()->show_properties(); })
            .with_icon("info")
            .disabled(!has),
        MenuItem(fo::tr("Pin this folder"), "Ctrl+D", [P] { P()->toggle_pin(); }).with_icon("pin"),
        MenuItem(fo::tr("Recents"), "Ctrl+Alt+R", [P] { P()->navigate(kRecents); }).with_icon("recent"),
        MenuItem(fo::tr("Star / unstar"), "Ctrl+E", [P] { P()->toggle_star(); }).with_icon("star"),
        MenuItem(fo::tr("Starred"), "Ctrl+Alt+E", [P] { P()->navigate(kStarred); }).with_icon("star-outline"),
        MenuItem(fo::tr("Favorites and places"), "Ctrl+Alt+D", [this] { FavoritesHub::open(this, active_pane()); })
            .with_icon("star"),
    };
    std::vector<MenuItem> sizes = {
        MenuItem(fo::tr("Calculate size"), "Ctrl+Space", [P] { P()->measure_focused(); }).with_icon("calculator"),
        MenuItem(fo::tr("Calculate listed folders"), "Ctrl+Shift+Space", [P] { P()->measure_listed(); })
            .with_icon("calculator"),
        MenuItem(fo::tr("Space map"), "Ctrl+4", [P] { P()->set_view(Pane::View::Space); }).with_icon("space"),
    };
    std::vector<MenuItem> tools = {
        MenuItem(fo::tr("Commands"), "Ctrl+Shift+P", [this] { commands(); }).with_icon("terminal"),
        MenuItem(fo::tr("Terminal here"), "Ctrl+.", [P] {
            if (!open_terminal(P()->folder()))
                P()->show_message(fo::tr("No terminal was found."));
        }).with_icon("terminal"),
        MenuItem(fo::tr("Reload"), "Ctrl+R", [P] {
            P()->reload(true);
            App::get().refresh_places();
        }).with_icon("refresh"),
    };
    std::vector<MenuItem> history = {
        MenuItem(fo::tr("Undo"), "Ctrl+Z", [P] { P()->undo(); }).with_icon("undo"),
        MenuItem(fo::tr("Redo"), "Ctrl+Shift+Z", [P] { P()->redo(); }).with_icon("redo"),
    };
    return {
        MenuItem(fo::tr("Create and organize"), "", nullptr, create).with_icon("folder-plus"),
        MenuItem(fo::tr("Clipboard"), "", nullptr, clip).with_icon("paste"),
        MenuItem(fo::tr("Selection"), "", nullptr, sel).with_icon("select"),
        MenuItem(fo::tr("Search and open"), "", nullptr, find).with_icon("search"),
        MenuItem(fo::tr("Space and sizes"), "", nullptr, sizes).with_icon("database"),
        MenuItem(fo::tr("Tools"), "", nullptr, tools).with_icon("terminal"),
        MenuItem(fo::tr("History and results"), "", nullptr, history).with_icon("history"),
    };
}

std::vector<fo::MenuItem> MainWindow::preferences_items() {
    using fo::MenuItem;
    Settings &s = App::get().settings();
    Pane *p = active_pane();
    auto toggle = [this](bool Settings::*field) {
        return [this, field] {
            Settings &st = App::get().settings();
            st.*field = !(st.*field);
            App::get().save_settings();
            if (field == &Settings::auto_sizes && st.auto_sizes)
                active_pane()->measure_listed();
            redraw();
        };
    };
    std::vector<MenuItem> folder = {
        MenuItem(fo::tr("Reload"), "Ctrl+R", [this] { active_pane()->reload(true); }).with_icon("refresh"),
        MenuItem(fo::tr("Sort by\xE2\x80\xA6"), "Ctrl+Shift+S", [this] { active_pane()->sort_menu(); }).with_icon("sort"),
        MenuItem(fo::tr("Save this folder's view"), "", [this] { active_pane()->save_folder_view(); })
            .with_icon("save")
            .disabled(p->in_recents()),
        MenuItem(fo::tr("Forget this folder's view"), "", [this] { active_pane()->forget_folder_view(); })
            .with_icon("x")
            .disabled(!p->has_folder_view()),
    };
    std::vector<MenuItem> view = {
        MenuItem(fo::tr("Show hidden files"), "Ctrl+H", [this] { active_pane()->toggle_hidden(); })
            .with_icon("eye")
            .check(p->show_hidden()),
        MenuItem(fo::tr("Dual pane"), "F9", [this] { toggle_dual(); }).with_icon("columns").check(dual_ != nullptr),
        MenuItem(fo::tr("Zoom in Grid"), "Ctrl++", [this] { active_pane()->zoom_step(1); }).with_icon("plus"),
        MenuItem(fo::tr("Zoom out Grid"), "Ctrl+-", [this] { active_pane()->zoom_step(-1); }).with_icon("minus"),
        MenuItem(fo::tr("Reset Grid zoom"), "Ctrl+0", [this] { active_pane()->zoom_step(0); }),
        MenuItem(fo::tr("Inspector"), "Ctrl+Alt+I", [this] { toggle_details(); }).with_icon("info").check(details_on_),
        MenuItem(fo::tr("Show thumbnails of pictures and videos"), "", toggle(&Settings::thumbnails))
            .check(s.thumbnails),
        MenuItem(fo::tr("Calculate sizes automatically"), "", toggle(&Settings::auto_sizes)).check(s.auto_sizes),
        MenuItem(fo::tr("MC operation bar"), "Ctrl+Alt+B", [this] { toggle_operation_bar(); })
            .with_icon("keyboard")
            .check(s.operation_bar),
        MenuItem(fo::tr("Keep the sidebar open"), "", [this] {
            rail_->pinned(!rail_->pinned());
            App::get().settings().rail_pinned = rail_->pinned();
            App::get().save_settings();
        }).check(rail_->pinned()),
    };
    std::vector<MenuItem> window = {
        MenuItem(fo::tr("Window: Normal"), "", [this] {
            if (fullscreen_active())
                fullscreen_off();
        }).check(!fullscreen_active()),
        MenuItem(fo::tr("Window: Fullscreen"), "", [this] {
            if (!fullscreen_active())
                fullscreen();
        }).check(fullscreen_active()),
        MenuItem(fo::tr("New window"), "", [this] {
            App::get().open_window(active_pane()->in_recents() ? xdg::home() : active_pane()->folder(), {});
        }).with_icon("external-link"),
    };
    std::vector<MenuItem> look = {
        MenuItem(fo::tr("Theme: System (live)"), "", nullptr).check(true),
    };
    std::vector<MenuItem> help = {
        MenuItem(dual_ ? fo::tr("Save only this group") : fo::tr("Save session"), "",
                 [this] { App::get().save_session(this); })
            .with_icon("save"),
        // Files: the two shortcut profiles, applied live to every window.
        MenuItem(fo::tr("Shortcuts: Files"), "", [this] { set_shortcut_profile(ShortcutProfile::Files); })
            .with_icon("keyboard")
            .check(s.shortcuts == ShortcutProfile::Files),
        MenuItem(fo::tr("Shortcuts: Nautilus"), "", [this] { set_shortcut_profile(ShortcutProfile::Nautilus); })
            .with_icon("keyboard")
            .check(s.shortcuts == ShortcutProfile::Nautilus),
        MenuItem(fo::tr("Shortcut help"), "F1", [this] { show_help(); }).with_icon("help"),
        MenuItem(fo::tr("Commands"), "Ctrl+Shift+P", [this] { commands(); }).with_icon("terminal"),
    };
    return {
        MenuItem(fo::tr("Folder"), "", nullptr, folder).with_icon("folder"),
        MenuItem(fo::tr("View"), "", nullptr, view).with_icon("eye"),
        MenuItem(fo::tr("Window"), "", nullptr, window).with_icon("monitor"),
        MenuItem(fo::tr("Appearance"), "", nullptr, look).with_icon("tag"),
        MenuItem(fo::tr("Session and help"), "", nullptr, help).with_icon("save"),
    };
}

void MainWindow::actions_menu() {
    int n = int(tools_->items().size());
    open_menu_under(n - 2, actions_items());
}

void MainWindow::preferences_menu() {
    int n = int(tools_->items().size());
    open_menu_under(n - 1, preferences_items());
}

void MainWindow::commands() { run_omnibar(">"); }

// --- Rail ------------------------------------------------------------------------------

std::string MainWindow::place_for(const std::string &folder) const {
    if (is_virtual_folder(folder))
        return folder;
    std::string best;
    for (const auto &r : rail_->rows()) {
        const std::string &id = r.id;
        if (id.empty() || id[0] != '/' || id == "/")
            continue;
        if (path_inside(folder, id) && id.size() > best.size())
            best = id;
    }
    return best;
}

// The rail's Favorites row opens the favorites hub (Files: Action::Bookmarks).
static const char *const kFavoritesRow = "favorites:";
// Files' "Connect…" row of the Network section.
static const char *const kConnectRow = "net:connect";

// Files' place glyphs (places_sidebar.rs place_glyph): by the place's
// position in the list, not by its folder: Home, then download, document,
// picture, music and film, and the database glyph for the rest.
static const char *place_glyph(std::size_t index) {
    static const char *const glyphs[] = {"home", "download", "file-text", "image", "music", "video"};
    return index < 6 ? glyphs[index] : "database";
}

// The rail key of a Places or Devices row (core/rail_order.h).
static std::string rail_key(const PlacesData &pd, const fo::SidebarRow &r, bool device) {
    if (device) {
        for (const auto &p : pd.drives)
            if (r.id == (p.mounted && !p.path.empty() ? p.path : "volume:" + p.id))
                return rail_order::device_key(p.name);
        return {};
    }
    for (const auto &p : pd.places)
        if (p.path == r.id)
            return rail_order::place_key(p.path, p.name);
    return {};
}

// The Network row of a connected location: "%1 on %2" with a share.
static std::string share_label(const GvfsShare &s) {
    return s.share.empty() ? s.title : fo::i18n::arg(fo::i18n::arg(fo::tr("%1 on %2"), s.share), s.title);
}

void MainWindow::rebuild_sidebar() {
    const PlacesData &pd = App::get().places();
    const auto &order = App::get().settings().rail_order;
    int cursor = rail_->cursor();
    rail_->clear();
    // Files' rail (places_sidebar.rs): Favorites, Recents and Starred,
    // then Places (Home and the XDG folders), Bookmarks, Devices, Network
    // (always, with "Connect…"), and the trash at the bottom.
    rail_->add_section("");
    rail_->add_row({fo::tr("Favorites"), "star", kFavoritesRow});
    for (const auto &p : pd.views)
        if (p.icon == "recent")
            rail_->add_row({fo::tr(p.name.c_str()), "recent", p.path});
    rail_->add_row({fo::tr("Starred"), "star-outline", kStarred});
    places_sec_ = rail_->add_section(fo::tr("Places"), pd.home);
    {
        std::vector<std::string> keys;
        for (const auto &p : pd.places)
            keys.push_back(rail_order::place_key(p.path, p.name));
        for (std::size_t i : rail_order::ordered_indices(keys, order))
            rail_->add_row({fo::tr(pd.places[i].name.c_str()), place_glyph(i), pd.places[i].path});
    }
    marks_sec_ = rail_->add_section(fo::tr("Bookmarks"));
    for (const auto &p : pd.pinned)
        rail_->add_row({p.name, "bookmark", p.path});
    rail_->section_visible(marks_sec_, !pd.pinned.empty());
    drives_sec_ = rail_->add_section(fo::tr("Devices"));
    {
        std::vector<std::string> keys;
        for (const auto &p : pd.drives)
            keys.push_back(rail_order::device_key(p.name));
        for (std::size_t i : rail_order::ordered_indices(keys, order)) {
            const Place &p = pd.drives[i];
            fo::SidebarRow r(p.name, place_icon(p.icon), p.mounted && !p.path.empty() ? p.path : "volume:" + p.id);
            r.ejectable = p.mounted && (p.can_eject || p.can_unmount);
            rail_->add_row(r);
        }
    }
    rail_->section_visible(drives_sec_, !pd.drives.empty());
    rail_->add_section(fo::tr("Network"));
    for (const auto &p : pd.remote)
        rail_->add_row({p.name, "globe", "mount:" + p.uri});
    std::vector<bool> shown(pd.shares.size(), false);
    for (const auto &p : pd.peers) {
        fo::SidebarRow r(p.name, p.os == "linux" ? "server" : "laptop", "tailnet:" + p.host);
        // A connected machine disconnects from its own row.
        for (std::size_t k = 0; k < pd.shares.size(); ++k)
            if (pd.shares[k].protocol == "SFTP" && pd.shares[k].title == p.host)
                shown[k] = r.ejectable = true;
        rail_->add_row(r);
    }
    for (std::size_t k = 0; k < pd.shares.size(); ++k) {
        if (shown[k])
            continue;
        const GvfsShare &s = pd.shares[k];
        // Files' second line (protocol · user) has no room in Flo's one-line
        // rows; the name keeps the space.
        fo::SidebarRow r(share_label(s), "server", s.path);
        r.ejectable = true;
        rail_->add_row(r);
    }
    fo::SidebarRow connect(fo::tr("Connect\xE2\x80\xA6"), "plus", kConnectRow);
    connect.busy = App::get().network_busy();
    rail_->add_row(connect);
    for (const auto &p : pd.views)
        if (p.icon == "trash") {
            fo::SidebarRow r(fo::tr(p.name.c_str()), "trash", p.path);
            r.footer = true;
            rail_->add_row(r);
        }
    rail_->set_cursor(std::min(cursor, int(rail_->rows().size()) - 1));
    if (Pane *p = active_pane())
        rail_->set_active(place_for(p->folder()));
    rail_->redraw();
}

std::vector<fo::SidebarRow> MainWindow::reorder_siblings(const fo::SidebarRow &row) const {
    std::vector<fo::SidebarRow> out;
    if (row.section != places_sec_ && row.section != marks_sec_ && row.section != drives_sec_)
        return out;
    for (const auto &r : rail_->rows())
        if (r.section == row.section && !r.footer)
            out.push_back(r);
    return out;
}

void MainWindow::reorder_row(const fo::SidebarRow &row, int index) {
    App &app = App::get();
    if (index < 0)
        return;
    if (row.section == marks_sec_) {
        // Bookmarks keep their order in the GTK bookmarks file.
        std::string error = places::move_bookmark(row.id, std::size_t(index));
        if (!error.empty())
            fo::Toast::show(this, fo::tr(error.c_str()), 3, fo::Toast::Error);
        app.refresh_places();
        return;
    }
    const bool device = row.section == drives_sec_;
    const PlacesData &pd = app.places();
    std::vector<std::string> visible;
    for (const auto &r : reorder_siblings(row))
        visible.push_back(rail_key(pd, r, device));
    std::vector<std::string> out;
    if (!rail_order::moved(app.settings().rail_order, visible, rail_key(pd, row, device), std::size_t(index), out))
        return;
    app.settings().rail_order = out;
    app.save_settings();
    app.refresh_places();
}

void MainWindow::disconnect_share(const std::string &path) {
    App &app = App::get();
    const GvfsShare *found = nullptr;
    for (const auto &s : app.places().shares)
        if (s.path == path)
            found = &s;
    if (!found)
        return;
    if (app.network_busy()) {
        fo::Toast::show(this, fo::tr("Wait for the network connection in progress"), 3);
        return;
    }
    const GvfsShare share = *found;
    const std::string label = share_label(share);
    // Files leaves the location first when it is open.
    for (MainWindow *w : app.windows()) {
        for (Pane *p : w->tabs_)
            if (path_inside(p->folder(), share.path))
                p->go_home();
        if (w->dual_ && path_inside(w->dual_->folder(), share.path))
            w->dual_->go_home();
    }
    status_->left(fo::i18n::arg(fo::tr("Disconnecting %1\xE2\x80\xA6"), label));
    app.network_job(+1);
    app.network().disconnect(share, [this, label](std::string error) {
        App &a = App::get();
        a.network_job(-1);
        a.refresh_shares();
        bool open = false;
        for (MainWindow *w : a.windows())
            open = open || w == this;
        if (!open)
            return;
        if (error.empty())
            fo::Toast::show(this, fo::i18n::arg(fo::tr("%1 disconnected"), label), 3);
        else
            fo::Toast::show(this, label + ": " + fo::tr(error.c_str()), 4, fo::Toast::Error);
    });
}

void MainWindow::activate_row(const fo::SidebarRow &r) {
    Pane *p = active_pane();
    const std::string &id = r.id;
    if (id == kFavoritesRow)
        FavoritesHub::open(this, p);
    else if (id == kConnectRow)
        connect_server();
    else if (id.compare(0, 8, "tailnet:") == 0)
        p->open_peer(id.substr(8));
    else if (id.compare(0, 6, "mount:") == 0)
        p->open_uri(id.substr(6));
    else if (id.compare(0, 7, "volume:") == 0)
        p->mount_volume(id.substr(7));
    else
        p->navigate(id);
}

void MainWindow::sidebar_context(const fo::SidebarRow &r, int x, int y) {
    std::vector<fo::MenuItem> m;
    fo::SidebarRow row = r;
    if (r.id == kConnectRow) {
        // Files: "Conectar a servidor…".
        m.push_back(fo::MenuItem(fo::tr("Connect to server\xE2\x80\xA6"), "", [this] { connect_server(); })
                        .with_icon("plus"));
        fo::PopupMenu::open_at(this, x, y, m);
        return;
    }
    m.push_back(fo::MenuItem(fo::tr("Open"), "Enter", [this, row] { activate_row(row); }));
    bool local = !r.id.empty() && r.id[0] == '/';
    const GvfsShare *share = nullptr;
    for (const auto &s : App::get().places().shares)
        if (s.path == r.id || (r.id.compare(0, 8, "tailnet:") == 0 && s.protocol == "SFTP" && s.title == r.id.substr(8)))
            share = &s;
    if (local) {
        std::string id = r.id;
        m.push_back(fo::MenuItem(fo::tr("Open in a new tab"), "Ctrl+Enter", [this, id] { new_tab(id); }));
        m.push_back(fo::MenuItem(fo::tr("Open in the other pane"), "F9", [this, id] {
            open_split();
            dual_->navigate(id);
        }));
        if (r.section == marks_sec_)
            m.push_back(fo::MenuItem(fo::tr("Unpin"), "Ctrl+D", [this, id] { active_pane()->toggle_pin(id); }));
        m.push_back(fo::MenuItem(fo::tr("Terminal here"), "Ctrl+.", [id] { open_terminal(id); }));
    }
    if (share) {
        std::string path = share->path;
        m.push_back(fo::MenuItem::separator());
        m.push_back(fo::MenuItem(fo::tr("Disconnect"), "", [this, path] { disconnect_share(path); })
                        .with_icon("eject"));
    } else if (r.ejectable) {
        m.push_back(fo::MenuItem::separator());
        m.push_back(fo::MenuItem(fo::tr("Eject"), "", [this, row] {
            App::get().devices().eject(row.id, [this, row](bool ok, const std::string &, const std::string &err) {
                fo::Toast::show(this, ok ? fo::i18n::arg(fo::tr("You can remove %1 now"), row.label)
                                         : (err.empty() ? fo::tr("The drive is busy") : err),
                                3, ok ? fo::Toast::Info : fo::Toast::Error);
            });
        }));
    }
    // Files' "Mover arriba" / "Mover abajo" for places, devices and bookmarks.
    std::vector<fo::SidebarRow> sibs = reorder_siblings(r);
    int at = -1;
    for (std::size_t i = 0; i < sibs.size(); ++i)
        if (sibs[i].id == r.id)
            at = int(i);
    if (at >= 0 && sibs.size() > 1) {
        m.push_back(fo::MenuItem::separator());
        if (at > 0)
            m.push_back(fo::MenuItem(fo::tr("Move up"), "", [this, row, at] { reorder_row(row, at - 1); }));
        if (at + 1 < int(sibs.size()))
            m.push_back(fo::MenuItem(fo::tr("Move down"), "", [this, row, at] { reorder_row(row, at + 1); }));
    }
    remap_menu_keys(m);
    fo::PopupMenu::open_at(this, x, y, m);
}

void MainWindow::focus_sidebar() {
    std::string here = place_for(active_pane()->folder());
    const auto &rows = rail_->rows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].id == here)
            rail_->set_cursor(int(i));
    rail_->take_focus();
}

void MainWindow::cycle_focus(bool back) {
    // Files: Tab switches between the panes of the dual view; with one
    // pane it goes to the rail and back.
    fo::set_focus_visible(true);
    bool in_rail = Fl::focus() == rail_;
    if (dual_ && !in_rail) {
        (active_ == 0 ? dual_ : tabs_[size_t(tab_)])->focus_view();
    } else if (in_rail) {
        active_pane()->focus_view();
    } else {
        (void)back;
        focus_sidebar();
    }
    redraw();
}

bool MainWindow::intercept(int event) {
    if (event == FL_KEYBOARD && Fl::event_key() == FL_Tab && !modal_open() &&
        !(Fl::event_state() & (FL_CTRL | FL_ALT | FL_META))) {
        if (dynamic_cast<Fl_Input_ *>(Fl::focus()))
            return false; // fields: Tab completes or moves on
        cycle_focus(Fl::event_state() & FL_SHIFT);
        return true;
    }
    if (event == FL_PUSH && (Fl::event_button() == FL_BACK_MOUSE || Fl::event_button() == FL_FORWARD_MOUSE) &&
        !modal_open()) {
        Pane *p = active_pane();
        for (Pane *q : {tabs_[size_t(tab_)], dual_})
            if (q && Fl::event_inside(q))
                p = q;
        set_active(p);
        Fl::event_button() == FL_BACK_MOUSE ? p->back() : p->forward();
        return true;
    }
    return false;
}

// --- Keys ---------------------------------------------------------------------------

void MainWindow::show_help() {
    // Files' help prints the active profile's chords; rows whose commands
    // have none in it are left out.
    // Two commands' first chords; "Ctrl+Shift+PgUp / PgDn" when they share
    // two modifiers.
    auto both = [this](Cmd a, Cmd b) {
        std::string x = key(a), y = key(b);
        if (x.empty() || y.empty())
            return x.empty() ? y : x;
        size_t cut = x.rfind('+');
        if (cut != std::string::npos && std::count(x.begin(), x.begin() + long(cut), '+') >= 1 &&
            y.compare(0, cut + 1, x, 0, cut + 1) == 0 && y.find('+', cut + 1) == std::string::npos)
            y = y.substr(cut + 1);
        return x + " / " + y;
    };
    const bool nautilus = App::get().settings().shortcuts == ShortcutProfile::Nautilus;
    // The five views in the profile's key order (Nautilus: List, Grid,
    // Groups, Columns, Space), "Ctrl+1 … 5" when they run in sequence.
    auto views = [this, nautilus] {
        const Cmd order_files[] = {Cmd::ViewGroups, Cmd::ViewList, Cmd::ViewColumns, Cmd::ViewSpace, Cmd::ViewGrid};
        const Cmd order_nautilus[] = {Cmd::ViewList, Cmd::ViewGrid, Cmd::ViewGroups, Cmd::ViewColumns, Cmd::ViewSpace};
        const Cmd *order = nautilus ? order_nautilus : order_files;
        bool sequence = true;
        std::string out;
        for (int i = 0; i < 5; ++i) {
            std::string k = key(order[i]);
            sequence = sequence && k == "Ctrl+" + std::to_string(i + 1);
            out += (out.empty() ? "" : " \xC2\xB7 ") + k;
        }
        return sequence ? std::string("Ctrl+1 \xE2\x80\xA6 5") : out;
    };
    std::vector<std::pair<std::string, std::string>> rows = {
        {fo::tr("Profile"), nautilus ? fo::tr("Shortcuts: Nautilus") : fo::tr("Shortcuts: Files")},
        {fo::tr("Type"), fo::tr("Filter this folder (Esc clears)")},
        {keys(Cmd::Filter), fo::tr("Edit the filter")},
        {keys(Cmd::Location), fo::tr("Edit the path; >command or :place opens a catalog")},
        {keys(Cmd::Commands), fo::tr("Commands")},
        {keys(Cmd::Search), fo::tr("Search in subfolders")},
        {keys(Cmd::Open), fo::tr("Open")},
        {keys(Cmd::Parent), fo::tr("Go to the parent folder")},
        {both(Cmd::Back, Cmd::Forward), fo::tr("Back / forward")},
        {keys(Cmd::Home), fo::tr("Go home")},
        {views(), nautilus ? fo::tr("List, Grid, Groups, Columns, Space") : fo::tr("Groups, List, Columns, Space, Grid")},
        {key(Cmd::ZoomIn) + " / " + key(Cmd::ZoomOut) + " / " + key(Cmd::ZoomReset),
         fo::tr("Zoom the Grid in / out / reset")},
        {keys(Cmd::Preview), fo::tr("Preview")},
        {both(Cmd::SelectAll, Cmd::InvertSelection), fo::tr("Select all / invert the selection")},
        {keys(Cmd::MarkAdvance), fo::tr("Mark and move on")},
        {keys(Cmd::MarkToggle), fo::tr("Mark without moving")},
        {keys(Cmd::ClearSelection), fo::tr("Clear the selection")},
        {keys(Cmd::Rename), fo::tr("Rename")},
        {both(Cmd::CopyFiles, Cmd::MoveFiles), fo::tr("Copy / move (to the other pane)")},
        {keys(Cmd::NewFolder), fo::tr("New folder")},
        {keys(Cmd::NewFile), fo::tr("New file")},
        {keys(Cmd::NewWindow), fo::tr("New window")},
        {keys(Cmd::Trash), fo::tr("Move to the trash")},
        {"Ctrl+C / X / V", fo::tr("Copy, cut, paste files")},
        {keys(Cmd::CopyPath), fo::tr("Copy the path")},
        {both(Cmd::Undo, Cmd::Redo), fo::tr("Undo / redo")},
        {both(Cmd::TabNew, Cmd::TabClose), fo::tr("New tab / close tab")},
        {both(Cmd::TabNext, Cmd::TabReopen), fo::tr("Next tab / reopen a closed tab")},
        {both(Cmd::TabMoveLeft, Cmd::TabMoveRight), fo::tr("Move the active tab left/right")},
        {keys(Cmd::DualPane) + " / Tab", fo::tr("Dual pane / switch pane")},
        {keys(Cmd::FocusCycle), fo::tr("Switch pane, or the sidebar and the list")},
        {keys(Cmd::RailToggle), fo::tr("Keep the sidebar open")},
        {keys(Cmd::SizeFocused), fo::tr("Calculate the folder's size")},
        {keys(Cmd::Hidden), fo::tr("Show hidden files")},
        {keys(Cmd::BookmarkAdd), fo::tr("Pin the folder")},
        {keys(Cmd::Bookmarks), fo::tr("Favorites and places")},
        {both(Cmd::StarToggle, Cmd::GoStarred), fo::tr("Star / open Starred")},
        {keys(Cmd::Inspector), fo::tr("Inspector")},
        {keys(Cmd::Properties), fo::tr("Properties and permissions")},
        {keys(Cmd::OperationBar), fo::tr("MC operation bar")},
        {both(Cmd::Actions, Cmd::Settings), fo::tr("Actions / Preferences")},
        {keys(Cmd::Refresh), fo::tr("Reload")},
        {keys(Cmd::ContextMenu), fo::tr("Context menu")},
        {keys(Cmd::Help), fo::tr("Show these shortcuts")},
    };
    std::vector<std::pair<std::string, std::string>> shown;
    for (auto &r : rows)
        if (!r.first.empty())
            shown.push_back(std::move(r));
    fo::HelpOverlay::show(this, shown);
}

void MainWindow::build_shortcuts() {
    for (const std::string &spec : shortcut_specs())
        remove_shortcut(spec);
    const ShortcutProfile profile = App::get().settings().shortcuts;
    auto P = [this] { return active_pane(); };
    // Files' table (core/shortcuts.h): every chord of the active profile,
    // plus its unprinted layout variants. "Enter" stays with the views.
    auto bind = [this, profile](Cmd c, std::function<void()> fn) {
        std::vector<std::string> specs = chords(profile, c);
        for (const auto &a : aliases(profile, c))
            specs.push_back(a);
        for (const auto &spec : specs)
            if (spec != "Enter")
                shortcut(spec, fn);
    };
    using V = Pane::View;
    bind(Cmd::ViewGroups, [P] { P()->set_view(V::Groups); });
    bind(Cmd::ViewList, [P] { P()->set_view(V::List); });
    bind(Cmd::ViewColumns, [P] { P()->set_view(V::Columns); });
    bind(Cmd::ViewSpace, [P] { P()->set_view(V::Space); });
    bind(Cmd::ViewGrid, [P] { P()->set_view(V::Grid); });
    bind(Cmd::ZoomIn, [P] { P()->zoom_step(1); });
    bind(Cmd::ZoomOut, [P] { P()->zoom_step(-1); });
    bind(Cmd::ZoomReset, [P] { P()->zoom_step(0); });
    bind(Cmd::Commands, [this] { commands(); });
    bind(Cmd::Help, [this] { show_help(); });
    bind(Cmd::TabNew, [this] { new_tab(active_pane()->in_recents() ? xdg::home() : active_pane()->folder()); });
    bind(Cmd::TabClose, [this] { defer([this] { close_tab(tab_); }); });
    bind(Cmd::TabReopen, [this] { reopen_tab(); });
    bind(Cmd::TabNext, [this] { select_tab((tab_ + 1) % int(tabs_.size())); });
    bind(Cmd::TabPrev, [this] { select_tab((tab_ + int(tabs_.size()) - 1) % int(tabs_.size())); });
    bind(Cmd::TabMoveLeft, [this] { move_tab(tab_, tab_ - 1); });
    bind(Cmd::TabMoveRight, [this] { move_tab(tab_, tab_ + 1); });
    bind(Cmd::FocusRail, [this] { focus_sidebar(); });
    // Nautilus' F6: the other pane of the dual view, else the rail and back.
    bind(Cmd::FocusCycle, [this] { cycle_focus(false); });
    // Nautilus' F9: the rail stays open, or not.
    bind(Cmd::RailToggle, [this] { toggle_rail_pin(); });
    // Files: the favorites hub (favorites, XDG places and history, labels).
    bind(Cmd::Bookmarks, [this] { FavoritesHub::open(this, active_pane()); });
    bind(Cmd::StarToggle, [P] { P()->toggle_star(); });
    bind(Cmd::GoStarred, [P] { P()->navigate(kStarred); });
    bind(Cmd::DualPane, [this] { toggle_dual(); });
    bind(Cmd::OperationBar, [this] { toggle_operation_bar(); });
    bind(Cmd::Search, [this] { begin_search(); });
    bind(Cmd::Filter, [this] { begin_filter(); });
    bind(Cmd::SizeFocused, [P] { P()->measure_focused(); });
    bind(Cmd::SizesListed, [P] { P()->measure_listed(); });
    bind(Cmd::BookmarkAdd, [P] { P()->toggle_pin(); });
    bind(Cmd::MarkAdvance, [P] { P()->mark_and_advance(); });
    bind(Cmd::MarkToggle, [P] { P()->toggle_mark(); });
    bind(Cmd::OpenWith, [P] { P()->menu_from_keyboard(); });
    bind(Cmd::Terminal, [P] {
        if (!open_terminal(P()->folder()))
            P()->show_message(fo::tr("No terminal was found."));
    });
    bind(Cmd::CopyPath, [P] { P()->copy_path(); });
    bind(Cmd::Preview, [P] { P()->preview(); });
    bind(Cmd::Trash, [P] { P()->delete_selected(); });
    bind(Cmd::DeletePermanently, [P] {
        if (P()->in_trash())
            P()->confirm_delete(P()->selected_paths());
        else
            P()->delete_selected();
    });
    bind(Cmd::Breadcrumbs, [this] { crumbs_->open_fold(); });
    // Nautilus' Alt+Down opens the focused item (Enter stays with the views).
    bind(Cmd::Open, [P] {
        if (P()->current() >= 0)
            P()->open_path(P()->current_path());
    });
    bind(Cmd::OpenInTab, [P, this] {
        if (const Entry *e = P()->current_entry(); e && e->is_dir)
            new_tab(P()->current_path());
    });
    bind(Cmd::OpenInBackground, [P, this] {
        if (const Entry *e = P()->current_entry(); e && e->is_dir)
            new_tab(P()->current_path(), true);
    });
    bind(Cmd::Back, [P] { P()->back(); });
    bind(Cmd::Forward, [P] { P()->forward(); });
    bind(Cmd::Parent, [P] { P()->go_up(); });
    bind(Cmd::Home, [P] { P()->go_home(); });
    bind(Cmd::Properties, [P] { P()->show_properties(); });
    bind(Cmd::ClipboardCopy, [P] { P()->copy(false); });
    bind(Cmd::ClipboardCut, [P] { P()->copy(true); });
    bind(Cmd::ClipboardPaste, [P] { P()->paste(); });
    bind(Cmd::NativePaste, [P] { P()->paste(); });
    bind(Cmd::Hidden, [P] { P()->toggle_hidden(); });
    bind(Cmd::Refresh, [P] {
        P()->reload(true);
        App::get().refresh_places();
    });
    bind(Cmd::ContextMenu, [P] { P()->menu_from_keyboard(); });
    bind(Cmd::Inspector, [this] { toggle_details(); });
    bind(Cmd::Actions, [this] { actions_menu(); });
    bind(Cmd::Settings, [this] { preferences_menu(); });
    // Files: F5 recalculates in the Space view, copies everywhere else.
    bind(Cmd::CopyFiles, [P, this] {
        if (P()->view() == Pane::View::Space && !split())
            P()->measure_listed();
        else
            P()->transfer(false);
    });
    bind(Cmd::MoveFiles, [P] { P()->transfer(true); });
    bind(Cmd::Rename, [P] { P()->rename(); });
    bind(Cmd::NewFolder, [P] { P()->new_folder(); });
    bind(Cmd::NewFile, [P] { P()->new_file(); });
    bind(Cmd::NewWindow, [this] {
        App::get().open_window(active_pane()->in_recents() ? xdg::home() : active_pane()->folder(), {});
    });
    bind(Cmd::Undo, [P] { P()->undo(); });
    bind(Cmd::Redo, [P] { P()->redo(); });
    bind(Cmd::Location, [this] { edit_path(); });
    bind(Cmd::SelectAll, [P] { P()->select_all(); });
    bind(Cmd::SelectFiltered, [P] { P()->select_filtered(); });
    bind(Cmd::ClearSelection, [P] { P()->clear_selection(); });
    bind(Cmd::InvertSelection, [P] { P()->invert_selection(); });
    bind(Cmd::SortMenu, [P] { P()->sort_menu(); });
    bind(Cmd::GoRecent, [P] { P()->navigate(kRecents); });
    bind(Cmd::OpenContaining, [P] {
        if (P()->current() >= 0 && (!P()->query().empty() || P()->in_recents()))
            P()->show_item(P()->current_path());
    });
    shortcut("Escape", [this, P] {
        if (field_mode_ != Field::None) {
            if (field_mode_ == Field::Filter)
                P()->set_filter("");
            end_field();
            return;
        }
        P()->escape();
    });
}

std::string MainWindow::keys(Cmd c) const { return chord_label(App::get().settings().shortcuts, c); }
std::string MainWindow::key(Cmd c) const { return primary_chord(App::get().settings().shortcuts, c); }

void MainWindow::set_shortcut_profile(ShortcutProfile p) {
    // Files: applied live and kept with the other preferences.
    Settings &s = App::get().settings();
    if (s.shortcuts == p)
        return;
    s.shortcuts = p;
    App::get().save_settings();
    App::get().shortcuts_changed();
}

void MainWindow::shortcuts_changed() {
    build_shortcuts();
    update_header();
    if (opbar_)
        opbar_->redraw();
    details_->show_from(active_pane());
}

void MainWindow::toggle_rail_pin() {
    rail_->pinned(!rail_->pinned());
    App::get().settings().rail_pinned = rail_->pinned();
    App::get().save_settings();
    layout();
}

} // namespace flo::ui

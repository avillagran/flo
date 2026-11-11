#include "ui/main_window.h"

#include "core/fsutil.h"
#include "core/settings.h"
#include "core/tree_size.h"
#include "core/xdg.h"
#include "ui/app.h"
#include "ui/commands.h"
#include "ui/details.h"
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

const char *kTips[] = {
    "Ctrl+L: type >command or :place and press Enter to open the catalog",
    "Type to filter: *.pdf, ext:rs,md, type:dir, !draft",
    "Home/End go to the ends. Shift extends the range; Ctrl moves the focus without replacing marks",
    "Insert marks and moves on; Ctrl+I inverts the selection",
    "F5 copies and F6 moves to the other pane (F9)",
    "Ctrl+Space measures the focused folder; Ctrl+Shift+Space all of them",
    "Ctrl+Shift+P: every command, with its shortcut",
};

} // namespace

// --- Construction ---------------------------------------------------------------

MainWindow::MainWindow(int W, int H, const std::string &folder, const std::string &select)
    : fo::Window(W, H, "Flo") {
    size_range(560, 400);
    const Settings &s = App::get().settings();
    details_on_ = s.details_pane;
    build_header();
    tab_strip_ = new fo::TabStrip(0, kHeader, W, kTabs);
    rail_ = new fo::Sidebar(0, kTop, kRail, H - kTop - kStatus);
    rail_->rail(true, kRail, kRailWide);
    rail_->pinned(s.rail_pinned);
    details_ = new DetailsPane(W - kInspector, kTop, kInspector, H - kTop - kStatus);
    status_ = new fo::StatusBar(0, H - kStatus, W, kStatus);
    end();
    resizable(nullptr);
    tabs_.push_back(make_pane(folder, int(view_from_settings(s.view))));

    tab_strip_->on_select([this](int i) { select_tab(i); });
    tab_strip_->on_close([this](int i) { defer([this, i] { close_tab(i); }); });
    tab_strip_->on_new([this] { new_tab(active_pane()->folder()); });
    tab_strip_->on_context([this](int i, int x, int y) {
        std::vector<fo::MenuItem> m;
        m.push_back(fo::MenuItem(fo::tr("New tab"), "Ctrl+T", [this] { new_tab(active_pane()->folder()); }));
        m.push_back(fo::MenuItem(fo::tr("Close tab"), "Ctrl+W", [this, i] { defer([this, i] { close_tab(i); }); }));
        m.push_back(fo::MenuItem(fo::tr("Reopen closed tab"), "Ctrl+Shift+T", [this] { reopen_tab(); })
                        .disabled(closed_tabs_.empty()));
        fo::PopupMenu::open_at(this, x, y, m);
    });
    status_->on_hint([this] { edit_path(); });

    rail_->on_width([this] { layout(); });
    rail_->on_pin([](bool on) {
        App::get().settings().rail_pinned = on;
        App::get().save_settings();
    });
    rail_->on_activate([this](const fo::SidebarRow &r) { activate_row(r); });
    rail_->on_finished([this] { active_pane()->focus_view(); });
    rail_->on_eject([this](const fo::SidebarRow &r) {
        std::string path = r.id;
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
    field_->on_key([this](int k) {
        if ((k == FL_Down || k == FL_Tab) && field_mode_ != Field::Path) {
            active_pane()->focus_view();
            return true;
        }
        return false;
    });
    field_->on_complete([this](const std::string &t) {
        if (field_mode_ != Field::Path)
            return t;
        return complete_path(t, active_pane()->folder(), active_pane()->show_hidden());
    });
    field_ok_->on_click([this] { run_omnibar(field_->value()); });
    field_cancel_->on_click([this] { end_field(); });
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

void MainWindow::reopen_tab() {
    if (closed_tabs_.empty())
        return;
    std::string f = closed_tabs_.back();
    closed_tabs_.pop_back();
    new_tab(f);
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
    int body = std::max(40, H - kTop - kStatus);
    rail_->resize(0, kTop, rw, body);
    tab_strip_->resize(0, kHeader, W, kTabs);
    status_->resize(0, H - kStatus, W, kStatus);
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
        fo::paint::vline(dual_->x() - 1, kTop, h() - kTop - kStatus, t.card());
    }
}

int MainWindow::handle(int event) { return fo::Window::handle(event); }

void MainWindow::hide() {
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
        {"arrow-left", fo::tr("Back"), "Alt+Left", false, p->can_back(), [this] { active_pane()->back(); }},
        {"arrow-right", fo::tr("Forward"), "Alt+Right", false, p->can_forward(), [this] { active_pane()->forward(); }},
        {"arrow-up", fo::tr("Up"), "Backspace", false, p->can_go_up(), [this] { active_pane()->go_up(); }},
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
        {filtered ? "x" : "search", filtered ? fo::tr("Clear filter") : fo::tr("Search"), filtered ? "Esc" : "Ctrl+F",
         filtered || field_mode_ == Field::Filter, true,
         [this, filtered] {
             if (filtered) {
                 active_pane()->set_filter("");
                 end_field();
             } else {
                 begin_filter();
             }
         }},
        {"pencil", fo::tr("Edit path"), "Ctrl+L", false, true, [this] { edit_path(); }},
    };
    if (w() < 640) {
        items.push_back({"groups", fo::tr("Views"), "", false, true, [this] {
                             std::vector<fo::MenuItem> m;
                             const char *names[] = {"Groups", "List", "Columns", "Space", "Grid"};
                             const char *keys[] = {"Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5"};
                             for (int k = 0; k < 5; ++k)
                                 m.push_back(fo::MenuItem(fo::tr(names[k]), keys[k], [this, k] {
                                                 active_pane()->set_view(V(k));
                                             }).check(active_pane()->view() == V(k)));
                             open_menu_under(2, m);
                         }});
    } else {
        items.push_back({"groups", fo::tr("Groups"), "Ctrl+1", v == V::Groups, true, view(V::Groups)});
        items.push_back({"list", fo::tr("List"), "Ctrl+2", v == V::List, true, view(V::List)});
        items.push_back({"grid", fo::tr("Grid"), "Ctrl+5", v == V::Grid, true, view(V::Grid)});
        items.push_back({"space", fo::tr("Space"), "Ctrl+4", v == V::Space, true, view(V::Space)});
        items.push_back({"columns", fo::tr("Columns"), "Ctrl+3", v == V::Columns, true, view(V::Columns)});
        items.push_back({"info", fo::tr("Inspector"), "Ctrl+Alt+I", details_on_ || dual_, true,
                         [this] { toggle_details(); }});
    }
    items.push_back({"actions", fo::tr("Actions"), "F10", false, true, [this] { actions_menu(); }});
    items.push_back({"settings", fo::tr("Preferences"), "Ctrl+,", false, true, [this] { preferences_menu(); }});
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
    static int tip = 0;
    status_->hint(fo::tr(kTips[size_t(tip) % (sizeof kTips / sizeof kTips[0])]));
    status_->right(fs_summary(p->folder() == kRecents ? xdg::home() : p->folder()));
}

void MainWindow::pss_tick(void *data) {
    auto *w = static_cast<MainWindow *>(data);
    std::string s = pss_text();
    if (!s.empty())
        w->status_->badge(s, fo::theme().green());
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
        add(actions_items(), add);
        add(preferences_items(), add);
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

void MainWindow::open_menu_under(int item, std::vector<fo::MenuItem> items) {
    int x, y, w, h;
    if (!tools_->item_rect(item, x, y, w, h))
        return;
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
        MenuItem(fo::tr("Pin this folder"), "Ctrl+D", [P] { P()->toggle_pin(); }).with_icon("pin"),
        MenuItem(fo::tr("Recents"), "Ctrl+Alt+R", [P] { P()->navigate(kRecents); }).with_icon("recent"),
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
    if (folder == kRecents)
        return kRecents;
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

void MainWindow::rebuild_sidebar() {
    const PlacesData &pd = App::get().places();
    const fo::Theme &t = fo::theme();
    int cursor = rail_->cursor();
    rail_->clear();
    // Files' rail: pinned folders and Recents first, then the places, the
    // network and drives, and the trash at the bottom.
    rail_->add_section("");
    for (const auto &p : pd.views)
        if (p.icon == "recent")
            rail_->add_row({fo::tr(p.name.c_str()), "recent", p.path});
    for (const auto &p : pd.pinned)
        rail_->add_row({p.name, "star", p.path});
    for (const auto &p : pd.remote)
        rail_->add_row({p.name, "globe", "mount:" + p.uri});
    rail_->add_section(fo::tr("Places"), pd.home);
    rail_->add_row({fo::tr("Home"), "home", pd.home});
    // Files' order: Downloads, Documents, Pictures, Music, Videos, then the rest.
    std::vector<Place> home_places = pd.home_places;
    auto rank = [](const Place &p) {
        static const char *order[] = {"downloads", "documents", "pictures", "music", "videos"};
        for (int k = 0; k < 5; ++k)
            if (p.icon == order[k])
                return k;
        return 5;
    };
    std::stable_sort(home_places.begin(), home_places.end(),
                     [&](const Place &a, const Place &b) { return rank(a) < rank(b); });
    for (const auto &p : home_places) {
        fo::SidebarRow r(p.name, place_icon(p.icon), p.path);
        if (p.icon == "dropbox" && !pd.dropbox.empty() && pd.dropbox != "synced") {
            r.status = pd.dropbox == "syncing" ? fo::tr("syncing") : fo::tr("stopped");
            r.status_color = pd.dropbox == "syncing" ? t.yellow() : t.red();
        }
        rail_->add_row(r);
    }
    int drives = rail_->add_section(fo::tr("Drives"));
    for (const auto &p : pd.drives) {
        fo::SidebarRow r(p.name, place_icon(p.icon), p.mounted && !p.path.empty() ? p.path : "volume:" + p.id);
        r.ejectable = p.mounted && (p.can_eject || p.can_unmount);
        rail_->add_row(r);
    }
    rail_->section_visible(drives, !pd.drives.empty());
    int net = rail_->add_section(fo::tr("Network"));
    for (const auto &p : pd.peers)
        rail_->add_row({p.name, p.os == "linux" ? "server" : "laptop", "tailnet:" + p.host});
    rail_->section_visible(net, !pd.peers.empty());
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

void MainWindow::activate_row(const fo::SidebarRow &r) {
    Pane *p = active_pane();
    const std::string &id = r.id;
    if (id.compare(0, 8, "tailnet:") == 0)
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
    m.push_back(fo::MenuItem(fo::tr("Open"), "Enter", [this, row] { activate_row(row); }));
    bool local = !r.id.empty() && r.id[0] == '/';
    if (local) {
        std::string id = r.id;
        m.push_back(fo::MenuItem(fo::tr("Open in a new tab"), "Ctrl+Enter", [this, id] { new_tab(id); }));
        m.push_back(fo::MenuItem(fo::tr("Open in the other pane"), "F9", [this, id] {
            open_split();
            dual_->navigate(id);
        }));
        bool pinned = false;
        for (const auto &p : App::get().places().pinned)
            pinned = pinned || p.path == id;
        if (pinned)
            m.push_back(fo::MenuItem(fo::tr("Unpin"), "Ctrl+D", [this, id] { active_pane()->toggle_pin(id); }));
        m.push_back(fo::MenuItem(fo::tr("Terminal here"), "Ctrl+.", [id] { open_terminal(id); }));
    }
    if (r.ejectable) {
        m.push_back(fo::MenuItem::separator());
        m.push_back(fo::MenuItem(fo::tr("Eject"), "", [this, row] {
            App::get().devices().eject(row.id, [this, row](bool ok, const std::string &, const std::string &err) {
                fo::Toast::show(this, ok ? fo::i18n::arg(fo::tr("You can remove %1 now"), row.label)
                                         : (err.empty() ? fo::tr("The drive is busy") : err),
                                3, ok ? fo::Toast::Info : fo::Toast::Error);
            });
        }));
    }
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
    fo::HelpOverlay::show(
        this, {
                  {fo::tr("Type"), fo::tr("Filter this folder (Esc clears)")},
                  {"Ctrl+F", fo::tr("Edit the filter")},
                  {"Ctrl+L", fo::tr("Edit the path; >command or :place opens a catalog")},
                  {"Ctrl+Shift+P", fo::tr("Commands")},
                  {"Ctrl+Shift+F", fo::tr("Search in subfolders")},
                  {"Enter", fo::tr("Open")},
                  {"Backspace / Alt+Up", fo::tr("Go to the parent folder")},
                  {"Alt+Left / Alt+Right", fo::tr("Back / forward")},
                  {"Alt+Home", fo::tr("Go home")},
                  {"Ctrl+1 \xE2\x80\xA6 5", fo::tr("Groups, List, Columns, Space, Grid")},
                  {"Ctrl++ / Ctrl+- / Ctrl+0", fo::tr("Zoom the Grid in / out / reset")},
                  {"Space / F3", fo::tr("Preview")},
                  {"Ctrl+A / Ctrl+I", fo::tr("Select all / invert the selection")},
                  {"Insert", fo::tr("Mark and move on")},
                  {"Ctrl+Shift+D", fo::tr("Clear the selection")},
                  {"F2", fo::tr("Rename")},
                  {"F5 / F6", fo::tr("Copy / move (to the other pane)")},
                  {"F7 / Ctrl+Shift+N", fo::tr("New folder")},
                  {"Ctrl+N", fo::tr("New file")},
                  {"Delete / F8", fo::tr("Move to the trash")},
                  {"Ctrl+C / X / V", fo::tr("Copy, cut, paste files")},
                  {"Ctrl+Alt+C", fo::tr("Copy the path")},
                  {"Ctrl+Z / Ctrl+Shift+Z", fo::tr("Undo / redo")},
                  {"Ctrl+T / Ctrl+W", fo::tr("New tab / close tab")},
                  {"Ctrl+Tab / Ctrl+Shift+T", fo::tr("Next tab / reopen a closed tab")},
                  {"F9 / Tab", fo::tr("Dual pane / switch pane")},
                  {"Ctrl+Space", fo::tr("Calculate the folder's size")},
                  {"Ctrl+H", fo::tr("Show hidden files")},
                  {"Ctrl+D", fo::tr("Pin the folder")},
                  {"Ctrl+Alt+I", fo::tr("Inspector")},
                  {"F10 / Ctrl+,", fo::tr("Actions / Preferences")},
                  {"Ctrl+R", fo::tr("Reload")},
                  {"Menu / Shift+F10", fo::tr("Context menu")},
                  {"F1", fo::tr("Show these shortcuts")},
              });
}

void MainWindow::build_shortcuts() {
    auto P = [this] { return active_pane(); };
    auto on = [this](const char *spec, std::function<void()> fn) { shortcut(spec, std::move(fn)); };
    using V = Pane::View;
    // Files' default shortcut profile (crates/ui/src/shortcuts.rs).
    on("Ctrl+1", [P] { P()->set_view(V::Groups); });
    on("Ctrl+2", [P] { P()->set_view(V::List); });
    on("Ctrl+3", [P] { P()->set_view(V::Columns); });
    on("Ctrl+4", [P] { P()->set_view(V::Space); });
    on("Ctrl+5", [P] { P()->set_view(V::Grid); });
    on("Ctrl++", [P] { P()->zoom_step(1); });
    on("Ctrl+=", [P] { P()->zoom_step(1); });
    on("Ctrl+-", [P] { P()->zoom_step(-1); });
    on("Ctrl+0", [P] { P()->zoom_step(0); });
    on("Ctrl+Shift+P", [this] { commands(); });
    on("F1", [this] { show_help(); });
    on("Ctrl+?", [this] { show_help(); });
    on("Ctrl+T", [this] { new_tab(active_pane()->in_recents() ? xdg::home() : active_pane()->folder()); });
    on("Ctrl+W", [this] { defer([this] { close_tab(tab_); }); });
    on("Ctrl+Shift+T", [this] { reopen_tab(); });
    on("Ctrl+Tab", [this] { select_tab((tab_ + 1) % int(tabs_.size())); });
    on("Ctrl+PageDown", [this] { select_tab((tab_ + 1) % int(tabs_.size())); });
    on("Ctrl+Shift+Tab", [this] { select_tab((tab_ + int(tabs_.size()) - 1) % int(tabs_.size())); });
    on("Ctrl+PageUp", [this] { select_tab((tab_ + int(tabs_.size()) - 1) % int(tabs_.size())); });
    on("Ctrl+Alt+S", [this] { focus_sidebar(); });
    on("Ctrl+Alt+D", [this] { focus_sidebar(); });
    on("F9", [this] { toggle_dual(); });
    on("Ctrl+Shift+F", [this] { begin_search(); });
    on("Ctrl+F", [this] { begin_filter(); });
    on("Ctrl+Space", [P] { P()->measure_focused(); });
    on("Ctrl+Shift+Space", [P] { P()->measure_listed(); });
    on("Ctrl+D", [P] { P()->toggle_pin(); });
    on("Insert", [P] { P()->mark_and_advance(); });
    on("Ctrl+Shift+O", [P] { P()->menu_from_keyboard(); });
    on("Ctrl+.", [P] {
        if (!open_terminal(P()->folder()))
            P()->show_message(fo::tr("No terminal was found."));
    });
    on("Ctrl+Alt+C", [P] { P()->copy_path(); });
    on("Space", [P] { P()->preview(); });
    on("F3", [P] { P()->preview(); });
    on("Delete", [P] { P()->delete_selected(); });
    on("F8", [P] { P()->delete_selected(); });
    on("Shift+Delete", [P] {
        if (P()->in_trash())
            P()->confirm_delete(P()->selected_paths());
        else
            P()->delete_selected();
    });
    on("Alt+Down", [this] { crumbs_->open_fold(); });
    on("Ctrl+Enter", [P, this] {
        if (const Entry *e = P()->current_entry(); e && e->is_dir)
            new_tab(P()->current_path());
    });
    on("Shift+Enter", [P, this] {
        if (const Entry *e = P()->current_entry(); e && e->is_dir)
            new_tab(P()->current_path(), true);
    });
    on("Alt+Left", [P] { P()->back(); });
    on("Alt+Right", [P] { P()->forward(); });
    on("Backspace", [P] { P()->go_up(); });
    on("Alt+Up", [P] { P()->go_up(); });
    on("Alt+Home", [P] { P()->go_home(); });
    on("Alt+Enter", [this] {
        if (!details_on_)
            toggle_details();
    });
    on("Ctrl+C", [P] { P()->copy(false); });
    on("Ctrl+X", [P] { P()->copy(true); });
    on("Ctrl+V", [P] { P()->paste(); });
    on("Ctrl+Shift+V", [P] { P()->paste(); });
    on("Ctrl+H", [P] { P()->toggle_hidden(); });
    on("Ctrl+R", [P] {
        P()->reload(true);
        App::get().refresh_places();
    });
    on("Menu", [P] { P()->menu_from_keyboard(); });
    on("Shift+F10", [P] { P()->menu_from_keyboard(); });
    on("Ctrl+Alt+I", [this] { toggle_details(); });
    on("F10", [this] { actions_menu(); });
    on("Ctrl+,", [this] { preferences_menu(); });
    // Files: F5 recalculates in the Space view, copies everywhere else.
    on("F5", [P, this] {
        if (P()->view() == Pane::View::Space && !split())
            P()->measure_listed();
        else
            P()->transfer(false);
    });
    on("F6", [P] { P()->transfer(true); });
    on("F2", [P] { P()->rename(); });
    on("F7", [P] { P()->new_folder(); });
    on("Ctrl+Shift+N", [P] { P()->new_folder(); });
    on("Ctrl+N", [P] { P()->new_file(); });
    on("Ctrl+Z", [P] { P()->undo(); });
    on("Ctrl+Shift+Z", [P] { P()->redo(); });
    on("Ctrl+L", [this] { edit_path(); });
    on("Ctrl+A", [P] { P()->select_all(); });
    on("Ctrl+Shift+A", [P] { P()->select_filtered(); });
    on("Ctrl+Shift+D", [P] { P()->clear_selection(); });
    on("Ctrl+I", [P] { P()->invert_selection(); });
    on("Ctrl+Shift+S", [P] { P()->sort_menu(); });
    on("Ctrl+Alt+R", [P] { P()->navigate(kRecents); });
    on("Ctrl+Alt+O", [P] {
        if (P()->current() >= 0 && !P()->query().empty())
            P()->show_item(P()->current_path());
    });
    on("Escape", [this, P] {
        if (field_mode_ != Field::None) {
            if (field_mode_ == Field::Filter)
                P()->set_filter("");
            end_field();
            return;
        }
        P()->escape();
    });
}

} // namespace flo::ui

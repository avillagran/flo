#include "ui/main_window.h"

#include "core/fsutil.h"
#include "core/xdg.h"
#include "ui/app.h"
#include "ui/details.h"
#include "ui/file_view.h"
#include "ui/pane.h"

#include "fltk-omarchy/app.h"
#include "fltk-omarchy/dialog.h"
#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/menu.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/sidebar.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>
#include <FL/Fl_Input_.H>

#include <algorithm>

namespace flo::ui {

namespace {

Pane::View view_from_settings(ViewMode m) {
    return m == ViewMode::List ? Pane::View::List : m == ViewMode::Columns ? Pane::View::Columns : Pane::View::Grid;
}

bool wide_enough(int w, bool split) { return w >= (split ? 1400 : 960); }

} // namespace

MainWindow::MainWindow(int W, int H, const std::string &folder, const std::string &select)
    : fo::Window(W, H, "Flo") {
    size_range(720, 460);
    const Settings &s = App::get().settings();
    details_on_ = s.details_pane;
    sidebar_ = new fo::Sidebar(0, kBar, kSidebar, H - kBar);
    new fo::Rule(kSidebar, kBar, 1, H - kBar);
    details_ = new DetailsPane(W - kDetails, kBar, kDetails, H - kBar);
    end();
    details_w_.jump(details_on_ && wide_enough(W, false) ? kDetails : 0);
    split_t_.jump(0);
    panes_[0] = new Pane(this, kSidebar + 1, kBar, W - kSidebar - 1, H - kBar, folder, view_from_settings(s.view));
    insert(*panes_[0], 0);
    resizable(nullptr);

    sidebar_->on_activate([this](const fo::SidebarRow &r) { activate_row(r); });
    sidebar_->on_finished([this] { active_pane()->focus_view(); });
    sidebar_->on_header([this](const std::string &id) {
        active_pane()->navigate(id);
        active_pane()->focus_view();
    });
    sidebar_->on_eject([this](const fo::SidebarRow &r) {
        std::string path = r.id;
        auto done = [this, path](bool ok, const std::string &, const std::string &error) {
            fo::Toast::show(this, ok ? fo::i18n::arg(fo::tr("You can remove %1 now"), path_basename(path))
                                     : (error.empty() ? fo::tr("The drive is busy") : error),
                            3, ok ? fo::Toast::Info : fo::Toast::Error);
            for (int i = 0; i < 2; ++i)
                if (panes_[i] && path_inside(panes_[i]->folder(), path))
                    panes_[i]->go_home();
        };
        App::get().devices().eject(path, done);
    });
    sidebar_->on_context([this](const fo::SidebarRow &r, int x, int y) { sidebar_context(r, x, y); });
    sidebar_->accepts_drop([](const fo::SidebarRow &r) { return !r.id.empty() && r.id[0] == '/'; });
    sidebar_->on_drop([this](const fo::SidebarRow &r, const std::string &text) {
        bool cut = false;
        active_pane()->drop_into(App::get().paths_from_clipboard(text, cut), r.id);
    });

    build_shortcuts();
    rebuild_sidebar();
    layout();
    if (!select.empty())
        panes_[0]->select_path(path_join(folder, select));
}

MainWindow::~MainWindow() {
    Fl::remove_timeout(nullptr, this);
    // Panes first: each takes its path bar (a child of this window) with it,
    // before Fl_Group's teardown would delete the bars on their own.
    for (auto *&p : panes_)
        if (p) {
            remove(p);
            delete p;
            p = nullptr;
        }
}

void MainWindow::hide() {
    if (shown() && !maximize_active()) {
        App::get().settings().window_width = w();
        App::get().settings().window_height = h();
    }
    fo::Window::hide();
    App::get().window_closed(this);
}

// --- Panes ------------------------------------------------------------------------

void MainWindow::set_active(Pane *p) {
    int i = index_of(p);
    if (i < 0)
        return;
    bool changed = i != active_;
    active_ = i;
    for (int k = 0; k < 2; ++k)
        if (panes_[k])
            panes_[k]->bar()->set_active_marker(split() && k == active_);
    if (changed) {
        pane_moved(p);
        pane_selected(p);
    }
}

void MainWindow::open_split() {
    if (split()) {
        panes_[1]->focus_view();
        return;
    }
    Pane *from = active_pane();
    Fl_Group::current(nullptr);
    panes_[1] = new Pane(this, w() / 2, kBar, w() / 2, h() - kBar, from->folder(), from->view());
    insert(*panes_[1], 0);
    panes_[1]->bar()->closable(true);
    split_t_.jump(0);
    split_t_.set(1, fo::anim::ms::LAYOUT, fo::anim::Ease::OutCubic);
    if (details_on_ && !wide_enough(w(), true))
        details_w_.set(0, fo::anim::ms::LAYOUT, fo::anim::Ease::OutCubic);
    layout();
    fo::anim::request_frame(relayout_frame, this);
    panes_[1]->focus_view();
}

void MainWindow::close_split() {
    if (!split())
        return;
    Pane *p = panes_[1];
    panes_[1] = nullptr;
    active_ = 0;
    if (App::get().preview().owner() == p)
        App::get().preview().close();
    remove(p);
    p->bar()->hide();
    Fl::delete_widget(p);
    panes_[0]->bar()->set_active_marker(false);
    if (details_on_ && wide_enough(w(), false))
        details_w_.set(kDetails, fo::anim::ms::LAYOUT, fo::anim::Ease::OutCubic);
    layout();
    fo::anim::request_frame(relayout_frame, this);
    panes_[0]->focus_view();
    pane_moved(panes_[0]);
}

void MainWindow::toggle_details() {
    details_on_ = !details_on_;
    App::get().settings().details_pane = details_on_;
    App::get().save_settings();
    bool fits = wide_enough(w(), split());
    details_w_.set(details_on_ && fits ? kDetails : 0, fo::anim::ms::LAYOUT, fo::anim::Ease::OutCubic);
    fo::anim::request_frame(relayout_frame, this);
    for (int i = 0; i < 2; ++i)
        if (panes_[i])
            panes_[i]->bar()->update();
    if (details_on_)
        details_->show();
    details_->show_from(active_pane());
}

void MainWindow::relayout_frame(void *data) {
    auto *w = static_cast<MainWindow *>(data);
    w->layout();
    if (w->details_w_.running() || w->split_t_.running())
        fo::anim::request_frame(relayout_frame, w);
}

void MainWindow::layout() {
    const int W = w(), H = h();
    int dw = int(details_w_.value() + 0.5);
    if (dw <= 0 && !details_w_.running())
        details_->hide();
    else
        details_->show();
    details_->resize(W - dw, kBar, kDetails, H - kBar);
    sidebar_->resize(0, kBar, kSidebar, H - kBar);
    for (int i = children(); i-- > 0;)
        if (auto *r = dynamic_cast<fo::Rule *>(child(i)))
            r->resize(kSidebar, kBar, 1, H - kBar);
    int x0 = kSidebar + 1, x1 = W - dw;
    if (!split()) {
        panes_[0]->resize(x0, kBar, std::max(100, x1 - x0), H - kBar);
        panes_[0]->bar()->resize(0, 0, W, kBar);
    } else {
        double t = split_t_.value();
        int full = x1 - x0;
        int left = int(full - (full / 2.0) * t);
        int mid = x0 + left;
        panes_[0]->resize(x0, kBar, std::max(100, left), H - kBar);
        panes_[0]->bar()->resize(0, 0, mid, kBar);
        panes_[1]->resize(mid + 1, kBar, std::max(100, x1 - mid - 1), H - kBar);
        panes_[1]->bar()->resize(mid + 1, 0, W - mid - 1, kBar);
    }
    redraw();
}

void MainWindow::resize(int X, int Y, int W, int H) {
    fo::Window::resize(X, Y, W, H);
    bool fits = wide_enough(W, split());
    if (!details_w_.running())
        details_w_.jump(details_on_ && fits ? kDetails : 0);
    layout();
}

void MainWindow::draw() {
    fo::Window::draw();
    // Between two panes the divider runs up through the top bar.
    if (split() && panes_[1]) {
        const fo::Theme &t = fo::theme();
        int mx = panes_[1]->x() - 1;
        if (!top_layer())
            fo::paint::vline(mx, 0, h(), t.hairline());
    }
}

int MainWindow::handle(int event) { return fo::Window::handle(event); }

void MainWindow::pane_moved(Pane *p) {
    if (p != active_pane())
        return;
    copy_label(p->title().c_str());
    sidebar_->set_active(place_for(p->folder()));
    details_->show_from(p);
}

void MainWindow::pane_selected(Pane *p) {
    if (p == active_pane())
        details_->show_from(p);
}

// --- Sidebar -----------------------------------------------------------------------

std::string MainWindow::place_for(const std::string &folder) const {
    if (folder == kRecents)
        return kRecents;
    std::string best;
    for (const auto &r : sidebar_->rows()) {
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
    int cursor = sidebar_->cursor();
    sidebar_->clear();
    sidebar_->add_section(fo::tr("Home"), pd.home);
    for (const auto &p : pd.home_places) {
        fo::SidebarRow r(p.name, place_icon(p.icon), p.path);
        if (p.icon == "dropbox" && !pd.dropbox.empty() && pd.dropbox != "synced") {
            r.status = pd.dropbox == "syncing" ? fo::tr("syncing") : fo::tr("stopped");
            r.status_color = pd.dropbox == "syncing" ? t.yellow() : t.red();
        }
        sidebar_->add_row(r);
    }
    int pinned = sidebar_->add_section(fo::tr("Pinned"));
    for (const auto &p : pd.pinned)
        sidebar_->add_row({p.name, "folder", p.path});
    for (const auto &p : pd.remote)
        sidebar_->add_row({p.name, "globe", "mount:" + p.uri});
    sidebar_->section_visible(pinned, !pd.pinned.empty() || !pd.remote.empty());
    sidebar_->add_section(fo::tr("Views"));
    for (const auto &p : pd.views)
        sidebar_->add_row({fo::tr(p.name.c_str()), place_icon(p.icon), p.path});
    int drives = sidebar_->add_section(fo::tr("Drives"));
    for (const auto &p : pd.drives) {
        fo::SidebarRow r(p.name, place_icon(p.icon), p.mounted && !p.path.empty() ? p.path : "volume:" + p.id);
        r.ejectable = p.mounted && (p.can_eject || p.can_unmount);
        sidebar_->add_row(r);
    }
    sidebar_->section_visible(drives, !pd.drives.empty());
    int tailnet = sidebar_->add_section(fo::tr("Tailnet"));
    for (const auto &p : pd.peers)
        sidebar_->add_row({p.name, p.os == "linux" ? "server" : "laptop", "tailnet:" + p.host});
    sidebar_->section_visible(tailnet, !pd.peers.empty());
    sidebar_->set_cursor(std::min(cursor, int(sidebar_->rows().size()) - 1));
    if (Pane *p = active_pane())
        sidebar_->set_active(place_for(p->folder()));
    sidebar_->redraw();
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
    m.push_back(fo::MenuItem(fo::tr("Open"), "\xE2\x8F\x8E", [this, row] { activate_row(row); }));
    bool local = !r.id.empty() && r.id[0] == '/';
    if (local) {
        std::string id = r.id;
        m.push_back(fo::MenuItem(fo::tr("Open in Split Pane"), "", [this, id] {
            open_split();
            panes_[1]->navigate(id);
        }));
        m.push_back(fo::MenuItem(fo::tr("Open in New Window"), "Ctrl+N", [id] { App::get().open_window(id, {}); }));
        bool pinned = false;
        for (const auto &p : App::get().places().pinned)
            pinned = pinned || p.path == id;
        if (pinned)
            m.push_back(fo::MenuItem(fo::tr("Unpin"), "Ctrl+D", [this, id] { active_pane()->toggle_pin(id); }));
        m.push_back(fo::MenuItem(fo::tr("Open Terminal Here"), "", [id] { open_terminal(id); }));
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
    const auto &rows = sidebar_->rows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].id == here)
            sidebar_->set_cursor(int(i));
    sidebar_->take_focus();
}

void MainWindow::cycle_focus(bool back) {
    fo::set_focus_visible(true);
    bool in_sidebar = Fl::focus() == sidebar_;
    if (!back) {
        if (in_sidebar)
            panes_[0]->focus_view();
        else if (split() && active_ == 0)
            panes_[1]->focus_view();
        else
            focus_sidebar();
    } else {
        if (in_sidebar)
            (split() ? panes_[1] : panes_[0])->focus_view();
        else if (split() && active_ == 1)
            panes_[0]->focus_view();
        else
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
        for (int i = 0; i < 2; ++i)
            if (panes_[i] && Fl::event_inside(panes_[i]))
                p = panes_[i];
        set_active(p);
        Fl::event_button() == FL_BACK_MOUSE ? p->back() : p->forward();
        return true;
    }
    return false;
}

// --- Keys ----------------------------------------------------------------------------

void MainWindow::show_help() {
    fo::HelpOverlay::show(
        this, {
                  {"\xE2\x86\x90 \xE2\x86\x91 \xE2\x86\x92 \xE2\x86\x93", fo::tr("Move the selection")},
                  {"\xE2\x86\x92 / \xE2\x86\x90", fo::tr("In list and columns: into a folder / back out")},
                  {"Shift+click / Shift+arrows", fo::tr("Select a range")},
                  {"Alt+click", fo::tr("Add or remove one from the selection")},
                  {fo::tr("Drag on empty space"), fo::tr("Select with a rectangle (Alt adds)")},
                  {"Ctrl+A", fo::tr("Select everything")},
                  {"Tab", fo::tr("Move between the sidebar and the panes")},
                  {"\xE2\x8F\x8E", fo::tr("Open")},
                  {"Backspace", fo::tr("Go to the parent folder")},
                  {"Alt+\xE2\x86\x90 / Alt+\xE2\x86\x92", fo::tr("Back / forward (or the mouse's thumb buttons)")},
                  {"Alt+Home", fo::tr("Go home")},
                  {"Ctrl+L", fo::tr("Type a path (Tab completes)")},
                  {"/", fo::tr("Search this folder and below")},
                  {"Ctrl+1 / 2 / 3", fo::tr("Grid, list or columns")},
                  {"Space", fo::tr("Preview the selected file")},
                  {"Ctrl+I", fo::tr("Show or hide the details pane")},
                  {"Ctrl+D", fo::tr("Pin or unpin the folder")},
                  {"Ctrl+C / X / V", fo::tr("Copy, cut, paste files")},
                  {"Ctrl+Shift+C", fo::tr("Copy the path as text")},
                  {"F2 / Ctrl+Shift+D", fo::tr("Rename / duplicate")},
                  {"Ctrl+N", fo::tr("New window")},
                  {"Ctrl+Shift+N", fo::tr("New folder")},
                  {"Ctrl+Alt+N / Ctrl+W", fo::tr("Split into two panes / close the right one")},
                  {"Delete", fo::tr("Move to the trash")},
                  {"Ctrl+Z / Ctrl+Shift+Z", fo::tr("Undo / redo")},
                  {"Right-click / Alt+\xE2\x8F\x8E", fo::tr("Everything else")},
                  {"S / Shift+S", fo::tr("Sort: name, kind, newest, oldest, size")},
                  {"Ctrl+H", fo::tr("Show hidden files")},
                  {"F5", fo::tr("Reload")},
                  {"?", fo::tr("Show these shortcuts")},
                  {"Q", fo::tr("Close the window")},
              });
}

void MainWindow::build_shortcuts() {
    auto P = [this] { return active_pane(); };
    auto on = [this](const char *spec, std::function<void()> fn) { shortcut(spec, std::move(fn)); };
    on("Ctrl+1", [P] { P()->set_view(Pane::View::Grid); });
    on("Ctrl+2", [P] { P()->set_view(Pane::View::List); });
    on("Ctrl+3", [P] { P()->set_view(Pane::View::Columns); });
    on("Ctrl+I", [this] { toggle_details(); });
    on("Ctrl+Alt+N", [this] { open_split(); });
    on("Ctrl+W", [this] { close_split(); });
    on("Ctrl+N", [P] { App::get().open_window(P()->in_recents() ? xdg::home() : P()->folder(), {}); });
    on("Ctrl+L", [P] { P()->bar()->edit_path(); });
    on("Alt+D", [P] { P()->bar()->edit_path(); });
    on("/", [P] {
        if (!P()->in_recents())
            P()->bar()->search();
    });
    on("Space", [P] { P()->preview(); });
    on("Ctrl+D", [P] { P()->toggle_pin(); });
    on("Ctrl+C", [P] { P()->copy(false); });
    on("Ctrl+X", [P] { P()->copy(true); });
    on("Ctrl+V", [P] { P()->paste(); });
    on("Ctrl+Shift+C", [P] { P()->copy_path(); });
    on("F2", [P] { P()->rename(); });
    on("Ctrl+Shift+D", [P] { P()->duplicate(); });
    on("Ctrl+Shift+N", [P] { P()->new_folder(); });
    on("Delete", [P] { P()->delete_selected(); });
    on("Ctrl+Z", [P] { P()->undo(); });
    on("Ctrl+Shift+Z", [P] { P()->redo(); });
    on("s", [P] { P()->step_sort(1); });
    on("Shift+S", [P] { P()->step_sort(-1); });
    on("Ctrl+H", [P] { P()->toggle_hidden(); });
    on("F5", [P] {
        P()->reload(true);
        App::get().refresh_places();
    });
    on("?", [this] { show_help(); });
    on("q", [this] { defer([this] { hide(); }); });
    on("Backspace", [P] { P()->go_up(); });
    on("Alt+Up", [P] { P()->go_up(); });
    on("Alt+Left", [P] { P()->back(); });
    on("Alt+Right", [P] { P()->forward(); });
    on("Alt+Home", [P] { P()->go_home(); });
    on("Alt+Enter", [P] { P()->menu_from_keyboard(); });
    on("Menu", [P] { P()->menu_from_keyboard(); });
    on("Shift+F10", [P] { P()->menu_from_keyboard(); });
    on("Escape", [P] { P()->escape(); });
}

} // namespace flo::ui

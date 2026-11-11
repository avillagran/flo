// A Flo window, laid out as Files lays its window out: a header (back,
// forward and up; the path chips or the omnibar; compact view and action
// buttons with Preferences at the far right), the tab strip under it, the
// 52 px rail on the left, the folder (one pane, or two with F9), the
// inspector on the right and the status bar at the bottom. The header, the
// rail and the inspector follow the pane used last.
#pragma once

#include "core/path_completion.h"
#include "core/places.h"
#include "core/shortcuts.h"
#include "core/session.h"

#include "fltk-omarchy/anim.h"
#include "fltk-omarchy/window.h"

#include <optional>
#include <string>
#include <vector>

namespace fo {
class Breadcrumbs;
class IconButton;
class Sidebar;
class StatusBar;
class TabStrip;
class TextField;
class ToolBar;
struct MenuItem;
struct SidebarRow;
} // namespace fo

namespace flo::ui {

class Pane;
class DetailsPane;

// Files' shortcut profiles: menu accelerators written with Files' chords,
// printed as the active profile binds the same commands.
void remap_menu_keys(std::vector<fo::MenuItem> &items);

class MainWindow : public fo::Window {
public:
    // Files' metrics: a 58 px header, 32 px tabs and status bar, a 52 px
    // rail (170 px expanded) and a 240 px inspector.
    static constexpr int kHeader = 58, kTabs = 32, kTop = kHeader + kTabs, kStatus = 32;
    static constexpr int kRail = 52, kRailWide = 170, kInspector = 240, kMaxTabs = 8;

    MainWindow(int w, int h, const std::string &folder, const std::string &select);
    ~MainWindow() override;

    // pane(0) is the current tab, pane(1) the dual view's second pane.
    // Null while the first pane is being made.
    Pane *active_pane() const { return active_ == 1 && dual_ ? dual_ : tabs_.empty() ? nullptr : tabs_[size_t(tab_)]; }
    Pane *pane(int i) const { return i == 0 ? (tabs_.empty() ? nullptr : tabs_[size_t(tab_)]) : i == 1 ? dual_ : nullptr; }
    const std::vector<Pane *> &tab_panes() const { return tabs_; }
    bool split() const { return dual_ != nullptr; }
    int index_of(const Pane *p) const;
    void set_active(Pane *p);
    Pane *other_pane(const Pane *p) const;

    // Tabs (Ctrl+T, Ctrl+W, Ctrl+Shift+T, Ctrl+Tab).
    void new_tab(const std::string &folder, bool background = false);
    void close_tab(int i);
    // Files' tab menu: "Close other tabs" and "Close tabs to the right".
    void close_other_tabs(int i);
    void close_tabs_right(int i);
    void reopen_tab();
    void select_tab(int i);
    // Files' shortcut profiles: the active profile's chords for a command
    // (all of them, "F1 / Ctrl+?", or the first), for menus and the help.
    std::string keys(Cmd c) const;
    std::string key(Cmd c) const;
    // Switches the profile for every window (saved with the settings).
    void set_shortcut_profile(ShortcutProfile p);
    // The profile changed: the window keys and what prints them follow.
    void shortcuts_changed();

    // Files: moves tab `from` to position `to` (Ctrl+Shift+PgUp/PgDn, drag).
    void move_tab(int from, int to);

    // The session: this window's tabs (not the dual pane), and showing a
    // saved one. The owner window saves it again when it closes.
    Session session_state() const;
    void restore_session(const Session &s);
    void session_owner(bool on) { session_owner_ = on; }
    bool session_owner() const { return session_owner_; }

    // F9: the dual view.
    void toggle_dual();
    void open_split();
    void close_split();
    bool details_visible() const { return details_on_; }
    void toggle_details();

    // The header's field: the omnibar (Ctrl+L: a path, >command or :place),
    // the filter (typing in the files, Ctrl+F) or a recursive search
    // (Ctrl+Shift+F).
    enum class Field { None, Path, Filter, Search };
    void edit_path();
    void begin_filter(const std::string &text = {});
    void begin_search();
    void end_field(bool refocus = true);
    Field field() const { return field_mode_; }
    bool typing() const;

    // Menus: Actions (F10), Preferences (Ctrl+,) and the commands palette
    // (Ctrl+Shift+P).
    void actions_menu();
    void preferences_menu();
    void commands();
    std::vector<fo::MenuItem> actions_items();
    std::vector<fo::MenuItem> preferences_items();

    // The header, rail, tabs, status bar and inspector follow the panes.
    void pane_moved(Pane *p);    // folder changed
    void pane_selected(Pane *p); // selection changed
    void pane_view_changed(Pane *p);
    void update_status();
    void sizes_changed();
    void rebuild_sidebar();
    void language_changed(); // relabels everything set once
    void focus_sidebar();
    void cycle_focus(bool back); // Tab / Shift+Tab
    void show_help();
    // A key the event dispatch hands over before widgets see it (Tab,
    // mouse thumb buttons). True when used.
    bool intercept(int event);

    void resize(int x, int y, int w, int h) override;
    void draw() override;
    int handle(int event) override;
    void hide() override;

private:
    void build_header();
    // Files' free header (window_chrome.rs): the 58 px header outside its
    // controls (back/forward/up, the address card, the OK/x buttons and each
    // toolbar button) moves the window.
    bool free_header(int x, int y) const;
    // Files' location field: Tab completes the last component and walks the
    // candidates (Shift+Tab, Up/Down), Enter accepts one, Esc closes them.
    bool path_key(int key);
    void complete_field(bool backwards);
    void close_path_completion();
    void set_field_text(const std::string &text);
    void show_completion(const std::string &notice);
    void build_shortcuts();
    void toggle_rail_pin();
    void update_header();
    void update_tabs();
    void layout();
    static void relayout_frame(void *);
    static void pss_tick(void *);
    void activate_row(const fo::SidebarRow &row);
    void sidebar_context(const fo::SidebarRow &row, int x, int y);
    std::string place_for(const std::string &folder) const;
    Pane *make_pane(const std::string &folder, int view);
    void run_omnibar(const std::string &text);
    void open_menu_under(int item, std::vector<fo::MenuItem> items);

    fo::ToolBar *nav_ = nullptr, *tools_ = nullptr;
    fo::Breadcrumbs *crumbs_ = nullptr;
    int address_x_ = 124, address_w_ = 0; // Files' address card (y 10..48)
    std::optional<PathCompletion> completion_;
    int tip_ = 0, tip_ticks_ = 0; // Files' status tips take turns
    fo::TextField *field_ = nullptr;
    fo::IconButton *field_ok_ = nullptr, *field_cancel_ = nullptr;
    fo::TabStrip *tab_strip_ = nullptr;
    fo::Sidebar *rail_ = nullptr;
    fo::StatusBar *status_ = nullptr;
    class OperationBar *opbar_ = nullptr;
    // Files' MC bar (Ctrl+Alt+B): shown from the setting; its height when shown.
    void toggle_operation_bar();
    std::string operation_reason(int i);
    void run_operation(int i);
    int opbar_height() const;
    DetailsPane *details_ = nullptr;

    std::vector<Pane *> tabs_;
    std::vector<std::string> closed_tabs_;
    int tab_ = 0;
    Pane *dual_ = nullptr;
    int active_ = 0; // 0: the tab, 1: the dual pane
    bool details_on_ = true;
    Field field_mode_ = Field::None;
    bool session_owner_ = false;
    std::string pss_;
};

} // namespace flo::ui

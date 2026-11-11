// One folder pane: a tab of a Files window, or the second pane of the dual
// view (F9). It shows a folder in Groups, List, Grid, Columns or Space,
// with a banner for problems and slow work. Each pane has its own folder,
// history, selection, filter, search, file operations (with undo) and
// mounts. The window's header (path chips, the omnibar, the view buttons)
// and the inspector follow the pane used last.
#pragma once

#include "core/entry.h"
#include "core/event_queue.h"
#include "core/fileops.h"
#include "core/lister.h"
#include "core/mounter.h"
#include "core/process.h"
#include "core/search.h"
#include "core/session.h"
#include "core/settings.h"
#include "ui/util.h"

#include <FL/Fl_Group.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fltk-omarchy/shell.h"

namespace fo {
class Button;
class IconButton;
struct MenuItem;
} // namespace fo

namespace flo::ui {

class MainWindow;
class FileView;
class ColumnsView;
class SpaceView;
class Pane;

class Pane : public Fl_Group {
public:
    // Files' views: Ctrl+1 Groups, 2 List, 3 Columns, 4 Space, 5 Grid.
    enum class View { Groups, List, Columns, Space, Grid };

    Pane(MainWindow *win, int x, int y, int w, int h, const std::string &folder, View view);
    ~Pane() override;

    MainWindow *win() const { return win_; }
    const std::string &folder() const { return folder_; }
    FolderModel &model() { return model_; }
    const FolderModel &model() const { return model_; }
    View view() const { return view_; }
    // `remember`: the first pane's choice becomes the default view.
    void set_view(View v, bool remember = true);
    static View from_mode(ViewMode m);
    static ViewMode to_mode(View v);
    // Files' saved folder views: this folder's view, sort and zoom kept
    // (applied whenever it is opened again) or dropped.
    void save_folder_view();
    void forget_folder_view();
    bool has_folder_view() const;
    // The session (Files' session_store.rs): what this tab shows, and
    // showing it again.
    TabState tab_state() const;
    void apply_tab_state(const TabState &t);
    // The second pane of the dual view: compact rows and its own small
    // header (folder · path), whatever its view.
    void dual(bool on);
    bool is_dual() const { return dual_; }
    FileView *files() const; // the view holding the selection
    const std::vector<std::pair<std::string, std::string>> &crumbs() const { return crumbs_; }
    std::string title() const;
    // Files' empty-state line for this listing (Starred and Recents have
    // their own).
    std::string empty_label() const;
    bool can_back() const { return !back_.empty(); }
    bool can_forward() const { return !forward_.empty(); }
    bool can_go_up() const;

    // Going places.
    void navigate(const std::string &path, const std::string &select = {}, bool history = true);
    void reload(bool keep_selection = true);
    void go_up();
    void back();
    void forward();
    void go_home();
    bool go_typed(const std::string &text);
    std::string typed_path() const;
    void open_index(int i);
    void open_path(const std::string &path);
    void show_item(const std::string &path); // its folder, selected
    void open_peer(const std::string &host);
    void open_uri(const std::string &uri);
    void mount_volume(const std::string &id);

    // Selection.
    int current() const;
    const Entry *current_entry() const;
    std::string current_path() const;
    std::vector<std::string> selected_paths() const; // or the cursor's item
    void select_path(const std::string &path);
    void focus_view();

    // What the keys and menus do.
    void copy(bool cut);
    void paste(const std::string &into = {});
    void copy_path();
    void rename();
    void duplicate();
    void new_folder();
    void delete_selected();
    void confirm_delete(const std::vector<std::string> &paths);
    void empty_trash();
    void restore(const std::vector<std::string> &paths);
    void undo();
    void redo();
    void toggle_hidden();
    void set_sort(SortMode m, bool remember = true);
    // Files' Grid zoom (Ctrl++ / Ctrl+- / Ctrl+0, Ctrl+wheel): cells and
    // icons scale by `grid_zoom` in [0.65, 1.8], 1.15 per step.
    static constexpr double kZoomMin = 0.65, kZoomMax = 1.8, kZoomStep = 1.15;
    double grid_zoom() const { return grid_zoom_; }
    void set_grid_zoom(double z);
    void zoom_by(double factor);
    // +1 in, -1 out, 0 reset; outside the Grid it says where zoom exists.
    void zoom_step(int step);
    void sort_menu();
    // Files' selection tools.
    void invert_selection();
    void clear_selection();
    void select_all();
    void mark_and_advance(); // Insert
    void toggle_mark();      // Ctrl+Insert (Nautilus: Ctrl+Space): marks without moving
    void select_filtered();  // Ctrl+Shift+A: mark the matches, show everything
    // Ctrl+N: an empty file (never overwriting).
    void new_file();
    // F5 / F6: copy or move what is selected to `folder` (asked when empty;
    // the other pane's folder in the dual view).
    void transfer(bool move, const std::string &folder = {});
    // Ctrl+Space: measure the focused folder again; Ctrl+Shift+Space: all
    // the listed folders.
    void measure_focused();
    void measure_listed();
    // A folder size arrived (or measuring stopped): the Space map relays.
    void sizes_arrived();
    // Type-to-filter (Files): only the names matching `text` show.
    void set_filter(const std::string &text);
    const std::string &filter() const { return filter_; }
    int unfiltered_count() const { return filter_.empty() ? model_.count() : int(all_.size()); }
    void toggle_pin(const std::string &path = {});
    void preview();
    void context_menu(int index, int x, int y, FolderModel *where = nullptr, const std::string &folder = {});
    void menu_from_keyboard();
    void open_with(const std::string &path, const std::string &app);
    void set_default(const std::string &mime, const std::string &app, const std::string &name);
    // Share (as Folio): a Dropbox link, LocalSend, Taildrop.
    void dropbox_link(const std::string &path);
    void share_localsend(const std::vector<std::string> &paths);
    void share_taildrop(const std::vector<std::string> &paths, const std::string &host, const std::string &name);
    void compress();
    void extract(const std::string &path);
    void drop_into(const std::vector<std::string> &paths, const std::string &folder);
    void start_drag();
    bool escape();
    // A recursive search below the folder (Ctrl+Shift+F); "" ends it.
    void set_query(const std::string &q);
    const std::string &query() const { return query_; }
    bool searching() const { return searching_; }

    bool in_trash() const;
    // Recents or Starred: a list of items from many folders.
    bool in_recents() const { return is_virtual_folder(folder_); }
    bool in_starred() const { return folder_ == kStarred; }
    // Ctrl+E: star the marked items (or the focused one), or unstar them
    // when they are all starred.
    void toggle_star();
    // Alt+Enter: Files' properties and permissions of the marked items (or
    // the focused one), up to 32.
    void show_properties();
    const std::vector<std::string> &history_back() const { return back_; }
    const std::vector<std::string> &history_forward() const { return forward_; }
    bool typing() const;
    bool show_hidden() const { return hidden_; }
    SortMode sort() const { return sort_; }

    // What the status bar says about this pane ("2 marked · 54 items").
    std::string status_text() const;
    bool loading() const { return model_.loading; }

    void show_message(const std::string &text, bool error = true);
    void dismiss_message();
    // The language changed: labels set once are set again.
    void relabel();

    void resize(int x, int y, int w, int h) override;
    void draw() override;
    int handle(int event) override;

private:
    void list_now(bool keep_selection);
    // Moves what is shown into the App's listing cache (leaving a folder,
    // starting a search) and back out (arriving, ending the search).
    void remember_listing();
    bool recall_listing();
    void landed(ListingPtr listing, bool keep);
    void count_folders();
    void watch();
    void unwatch();
    void build_crumbs();
    void after_folder_change();
    void selection_changed();
    void op_done(const OpResult &r);
    void op_progress(const OpProgress &p);
    void op_conflict(const OpConflict &c);
    void ask_mount(const MountRequest &r);
    void place_children();
    void order_for_view(); // Groups: folders, then files by age section
    void request_sizes();
    void sort_clicked(int column);
    std::vector<fo::SortHeader::Column> list_header_columns() const;
    int banner_height() const;
    int header_height() const;
    // The Grid header (36 px): caption, zoom percentage, − and +.
    bool grid_header() const { return view_ == View::Grid && !dual_; }
    void zoom_rects(int &minus_x, int &plus_x, int &y) const;
    int grid_header_y_ = 0;
    std::string banner_text(bool &error, bool &progress) const;
    void wire(FileView *v);
    std::vector<fo::MenuItem> menu_for(int index, FolderModel *where, const std::string &folder);
    std::vector<fo::MenuItem> share_menu(const std::vector<std::string> &paths, const std::string &target);
    // Runs a tool off the UI thread with a busy banner, then `done` here
    // (never after the pane is gone).
    void run_tool(const std::vector<std::string> &argv, const std::string &busy, int timeout_ms,
                  std::function<void(const ProcessResult &)> done);

    MainWindow *win_;
    FileView *grid_ = nullptr; // groups, list, grid
    ColumnsView *columns_ = nullptr;
    SpaceView *space_ = nullptr;
    fo::SortHeader *list_header_ = nullptr;
    fo::IconButton *dismiss_ = nullptr;
    fo::Button *cancel_ = nullptr, *empty_trash_ = nullptr;
    // Files' copy card: Pause / Resume, Cancel job, Clear pending.
    fo::Button *pause_ = nullptr, *cancel_job_ = nullptr, *clear_pending_ = nullptr;
    int card_y_ = 0, card_h_ = 0;
    bool copy_card() const;
    void draw_copy_card();
    void relabel_card();

    std::string folder_;
    FolderModel model_;
    View view_ = View::Groups;
    bool dual_ = false;
    bool hidden_ = false;
    std::string filter_;
    std::vector<Entry> all_; // the whole listing while a filter shows part of it
    SortMode sort_ = SortMode::Name;
    double grid_zoom_ = 1.0;
    std::vector<std::string> back_, forward_;
    std::vector<std::pair<std::string, std::string>> crumbs_; // label, path
    std::string pending_select_;
    // A restored tab's marks and focus, applied once its listing is complete.
    std::vector<std::string> restore_marks_;
    std::string restore_current_;
    std::vector<std::string> keep_paths_;
    std::string keep_current_;

    std::unique_ptr<Lister> lister_;
    std::unique_ptr<Searcher> searcher_;
    std::unique_ptr<FileOps> ops_;
    std::unique_ptr<Mounter> mounter_;
    Executor recents_exec_;
    AliveGuard alive_;
    int watch_id_ = 0;
    std::string query_;
    bool searching_ = false;
    bool showing_cached_ = false; // a remembered listing, being re-read
    bool password_given_ = false; // a mount password was just answered

    std::string message_;
    bool message_error_ = true;
    std::string busy_; // "Connecting to laptop…"
    OpProgress progress_;
    bool progress_on_ = false;
    std::string undo_label_, redo_label_;
    bool paste_pending_ = false;
    std::string paste_into_;
};

} // namespace flo::ui

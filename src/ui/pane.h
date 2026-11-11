// One folder pane (Folio's FolderPane): its path bar, a banner for problems
// and slow work, and the files in a grid, a list or columns. Each pane has
// its own folder, history, selection, search, file operations (with undo)
// and mounts. A window has one, or two side by side.
#pragma once

#include "core/entry.h"
#include "core/event_queue.h"
#include "core/fileops.h"
#include "core/lister.h"
#include "core/mounter.h"
#include "core/search.h"
#include "core/settings.h"
#include "ui/util.h"

#include <FL/Fl_Group.H>

#include <memory>
#include <string>
#include <vector>

namespace fo {
class Breadcrumbs;
class Button;
class IconButton;
class TextField;
struct MenuItem;
} // namespace fo

namespace flo::ui {

class MainWindow;
class FileView;
class ColumnsView;
class Pane;

// The pane's part of the top bar: breadcrumbs (or the path being typed, or
// the search), then the view, sort, hidden-files and details buttons.
class PathBar : public Fl_Group {
public:
    PathBar(int x, int y, int w, int h, Pane *pane);
    void update(); // folder, view, sort, toggles
    void edit_path();
    void stop_editing(bool refocus = true);
    void search();
    void end_search(bool refocus = true);
    bool editing() const { return editing_; }
    bool searching() const;
    bool search_focused() const;
    void closable(bool on);
    void resize(int x, int y, int w, int h) override;
    void draw() override;
    void set_active_marker(bool on) { marker_ = on; redraw(); }

    fo::Breadcrumbs *crumbs = nullptr;
    fo::TextField *path_field = nullptr, *search_field = nullptr;
    fo::IconButton *view_b = nullptr, *sort_b = nullptr, *hidden_b = nullptr, *details_b = nullptr, *close_b = nullptr;

private:
    static void search_timer(void *);
    void place();
    Pane *pane_;
    bool editing_ = false, closable_ = false, marker_ = false;
};

class Pane : public Fl_Group {
public:
    enum class View { Grid, List, Columns };

    Pane(MainWindow *win, int x, int y, int w, int h, const std::string &folder, View view);
    ~Pane() override;

    MainWindow *win() const { return win_; }
    PathBar *bar() const { return bar_; }
    const std::string &folder() const { return folder_; }
    FolderModel &model() { return model_; }
    const FolderModel &model() const { return model_; }
    View view() const { return view_; }
    void set_view(View v);
    void cycle_view();
    FileView *files() const; // the view holding the selection
    const std::vector<std::pair<std::string, std::string>> &crumbs() const { return crumbs_; }
    std::string title() const;

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
    void step_sort(int by);
    void set_sort(SortMode m);
    void sort_menu();
    void toggle_pin(const std::string &path = {});
    void preview();
    void context_menu(int index, int x, int y, FolderModel *where = nullptr, const std::string &folder = {});
    void menu_from_keyboard();
    void open_with(const std::string &path, const std::string &app);
    void compress();
    void extract(const std::string &path);
    void drop_into(const std::vector<std::string> &paths, const std::string &folder);
    void start_drag();
    bool escape();
    void set_query(const std::string &q);
    const std::string &query() const { return query_; }

    bool in_trash() const;
    bool in_recents() const { return folder_ == kRecents; }
    bool typing() const;
    bool show_hidden() const { return hidden_; }
    SortMode sort() const { return sort_; }

    void show_message(const std::string &text, bool error = true);
    void dismiss_message();

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
    int banner_height() const;
    std::string banner_text(bool &error, bool &progress) const;
    void wire(FileView *v);
    std::vector<fo::MenuItem> menu_for(int index, FolderModel *where, const std::string &folder);

    MainWindow *win_;
    PathBar *bar_;
    FileView *grid_ = nullptr; // grid and list
    ColumnsView *columns_ = nullptr;
    fo::IconButton *dismiss_ = nullptr;
    fo::Button *cancel_ = nullptr, *empty_trash_ = nullptr;

    std::string folder_;
    FolderModel model_;
    View view_ = View::Grid;
    bool hidden_ = false;
    SortMode sort_ = SortMode::Name;
    std::vector<std::string> back_, forward_;
    std::vector<std::pair<std::string, std::string>> crumbs_; // label, path
    std::string pending_select_;
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

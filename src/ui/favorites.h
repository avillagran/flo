// Files' favorites hub (Ctrl+Alt+D, bookmarks_ui.rs): a panel over the
// window listing the favorites (Flo's GTK bookmarks, with their labels),
// the XDG places and the tab's Back and Forward history. Typing filters by
// label; Enter opens the highlighted row; F2 edits a favorite's label (the
// folder is never renamed); Delete (with the list focused) removes a
// favorite (the folder stays); Ctrl+D adds the current folder; F5 reloads;
// Esc cancels the label edit or closes.
#pragma once

#include "fltk-omarchy/window.h"

#include <string>
#include <vector>

namespace fo {
class TextField;
}

namespace flo::ui {

class Pane;

class FavoritesHub : public fo::Layer {
public:
    static FavoritesHub *open(fo::Window *win, Pane *pane);
    void draw() override;
    int handle(int event) override;
    void relayout() override;

private:
    struct Row {
        std::string label, path;
        int kind; // 0 favorite, 1 place, 2 back, 3 forward
    };
    enum { kEnter, kAdd, kLabel, kRemove, kReload, kButtons };
    FavoritesHub() = default;
    void reload();
    void refilter();
    void place();
    void jump();
    void add_current();
    void begin_label();
    void save_label();
    void remove_favorite();
    void press(int button);
    void move(int to);
    bool key(int k);
    bool button_enabled(int b) const;
    const Row *current() const;
    void notice(const std::string &text, bool error = false);

    Pane *pane_ = nullptr;
    fo::TextField *field_ = nullptr;
    std::vector<Row> all_;
    std::vector<int> shown_;
    int hi_ = 0, top_ = 0, rows_ = 1;
    bool list_focus_ = false, editing_ = false;
    std::string edit_path_, saved_query_, notice_;
    bool notice_error_ = false;
    int rx_ = 0, ry_ = 0, rw_ = 0, rh_ = 0, list_y_ = 0, list_h_ = 0;
};

} // namespace flo::ui

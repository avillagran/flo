// A Flo window, laid out like Folio's: one bar across the top (each pane's
// path bar), the sidebar, one folder pane or two side by side (Ctrl+Alt+N,
// Ctrl+W), and the details pane (Ctrl+I). The sidebar and the details
// follow the pane used last.
#pragma once

#include "core/places.h"

#include "fltk-omarchy/anim.h"
#include "fltk-omarchy/window.h"

#include <string>
#include <vector>

namespace fo {
class Sidebar;
struct SidebarRow;
} // namespace fo

namespace flo::ui {

class Pane;
class DetailsPane;

class MainWindow : public fo::Window {
public:
    static constexpr int kBar = 60, kSidebar = 236, kDetails = 300;

    MainWindow(int w, int h, const std::string &folder, const std::string &select);
    ~MainWindow() override;

    Pane *active_pane() const { return panes_[active_]; }
    Pane *pane(int i) const { return i >= 0 && i < 2 ? panes_[i] : nullptr; }
    bool split() const { return panes_[1] != nullptr; }
    int index_of(const Pane *p) const { return p == panes_[0] ? 0 : p == panes_[1] ? 1 : -1; }
    void set_active(Pane *p);

    void open_split();
    void close_split();
    bool details_visible() const { return details_on_; }
    void toggle_details();

    // The sidebar and the details pane follow the active pane.
    void pane_moved(Pane *p);     // folder changed
    void pane_selected(Pane *p);  // selection changed
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
    void build_shortcuts();
    void layout();
    static void relayout_frame(void *);
    void activate_row(const fo::SidebarRow &row);
    void sidebar_context(const fo::SidebarRow &row, int x, int y);
    std::string place_for(const std::string &folder) const;

    fo::Sidebar *sidebar_ = nullptr;
    Pane *panes_[2] = {nullptr, nullptr};
    DetailsPane *details_ = nullptr;
    int active_ = 0;
    bool details_on_ = true;
    fo::Anim<double> details_w_, split_t_;
    bool closing_split_ = false;
};

} // namespace flo::ui

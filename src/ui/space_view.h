// Files' Space view (Ctrl+4): a treemap of the folder, each file and
// folder a block whose area is its apparent size (folders as measured by
// FolderSizes). Pointing at a block shows its name, kind, size and share;
// arrows move to the nearest block in that direction, Enter opens a folder
// (zooms in), Backspace goes back up. Blocks too small for a label still
// show their tooltip when focused.
//
// Above the map sits Files' Space toolbar (108 px): the title and the
// hidden-files toggle, Back / Recalculate / Cancel with the apparent total,
// the watch notes and Marks (the selection tools). Under the map, Files'
// "No area/label" list names what has no area or is too small for a label
// (three rows, scrolled with the wheel or the arrows).
#pragma once

#include <FL/Fl_Widget.H>

#include <cstdint>
#include <string>
#include <vector>

namespace flo::ui {

class Pane;

class SpaceView : public Fl_Widget {
public:
    SpaceView(int x, int y, int w, int h, Pane *pane);
    // The folder, its listing or its sizes changed.
    void sync();
    void draw() override;
    int handle(int event) override;
    void resize(int x, int y, int w, int h) override;

    struct Block {
        int index;      // into the pane's model
        uint64_t bytes;
        int x, y, w, h; // relative to the map's origin
    };
    // Squarified treemap of `sizes` (descending) into w x h.
    static std::vector<Block> squarify(const std::vector<std::pair<int, uint64_t>> &sizes, int w, int h);

    // Files' metrics: the toolbar above the map, the smallest labelled
    // block, the list's rows.
    static constexpr int kToolbar = 108, kLabelW = 65, kLabelH = 45, kRow = 28, kListRows = 3;
    // What the "No area/label" list holds (model indices), for tests.
    int list_count() const { return int(rows_.size()); }

private:
    struct Row {
        int index;
        std::string state; // "0 B · no area", "4 kB · label in list", ...
    };
    struct Btn {
        int x, y, w, h;
        std::string label, icon, tip;
        int id;
    };
    enum { kHidden, kBack, kRecalc, kCancel, kMarks, kListUp, kListDown };
    void layout();
    void layout_buttons();
    int block_at(int ex, int ey) const;
    int button_at(int ex, int ey) const;
    int row_at(int ex, int ey) const;
    int nearest(int from, int dx, int dy) const;
    void focus_block(int b);
    void focus_row(int r);
    void press(int id);
    bool scanning() const;
    Pane *pane_;
    std::vector<Block> blocks_;
    std::vector<Row> rows_;
    std::vector<Btn> buttons_;
    uint64_t total_ = 0;
    bool partial_ = false;
    int hover_ = -1, focus_ = -1, row_focus_ = -1, row_top_ = 0, hover_button_ = -1;
    int map_x_ = 0, map_y_ = 0, map_w_ = 0, map_h_ = 0, list_y_ = 0;
};

} // namespace flo::ui

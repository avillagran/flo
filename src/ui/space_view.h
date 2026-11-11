// Files' Space view (Ctrl+4): a treemap of the folder, each file and
// folder a block whose area is its apparent size (folders as measured by
// FolderSizes). Pointing at a block shows its name, kind, size and share;
// arrows move to the nearest block in that direction, Enter opens a folder
// (zooms in), Backspace goes back up. Blocks too small for a label still
// show their tooltip when focused. Items without a size are counted under
// the map.
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

private:
    void layout();
    int block_at(int ex, int ey) const;
    int nearest(int from, int dx, int dy) const;
    void focus_block(int b);
    Pane *pane_;
    std::vector<Block> blocks_;
    uint64_t total_ = 0;
    int unsized_ = 0, hover_ = -1, focus_ = -1;
    int map_x_ = 0, map_y_ = 0, map_w_ = 0, map_h_ = 0;
};

} // namespace flo::ui

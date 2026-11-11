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
//
// Every block takes Files' stable colour of its name (tile_fill) with a
// readable label colour; marks and hover are a light band, keyboard focus
// an accent ring that glides between blocks. Opening a folder zooms its
// block to the whole map and going up shrinks the map back into its block
// (space_map_zoom.rs): paint-only, the pane's folder and focus change at
// once, and nothing asks for frames once the motion ends.
#pragma once

#include "fltk-omarchy/anim.h"
#include "fltk-omarchy/theme.h"

#include <FL/Fl_Widget.H>

#include <cstdint>
#include <deque>
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
    // Files' weighted binary treemap of `sizes` into w x h, larger first.
    static std::vector<Block> treemap(const std::vector<std::pair<int, uint64_t>> &sizes, int w, int h);

    // Files' metrics: the toolbar above the map, the smallest labelled
    // block, the list's rows.
    static constexpr int kToolbar = 108, kLabelW = 65, kLabelH = 45, kRow = 28, kListRows = 3;
    // What the "No area/label" list holds (model indices), for tests.
    int list_count() const { return int(rows_.size()); }

    // Files' motion: the block <-> map zoom, the children growing in when
    // the sizes arrive after it, the focus ring's glide.
    static constexpr int kZoomMs = 220, kRevealMs = 140, kRingMs = 80;
    // How long the old map holds still while the new folder's listing
    // arrives, before the zoom starts without it.
    static constexpr int kHoldMs = 120;
    // Folder layouts kept for zooming back out, and tiles kept per layout.
    static constexpr size_t kSnapshots = 8, kSnapshotTiles = 512;
    // True while the last paint left a zoom, reveal or glide running.
    bool animating() const { return animating_; }
    // Files' block colour of a name and the label colour that reads on it
    // (ui/tile_colors.h) in the current theme.
    static fo::Rgb tile_fill(const std::string &name);
    static fo::Rgb readable(fo::Rgb fill);

private:
    // One painted block, relative to the map's origin.
    struct Tile {
        int x = 0, y = 0, w = 0, h = 0;
        std::string name;
        bool dir = false;
        int kind = 0; // flo::Kind
        uint64_t bytes = 0, files = 0;
    };
    struct Snapshot {
        std::string path;
        unsigned gen = 0;
        fo::RectF map;
        std::vector<Tile> tiles;
        const Tile *find(const std::string &name) const;
    };
    // p' = p * s + t per axis.
    struct Affine {
        double sx = 1, sy = 1, tx = 0, ty = 0;
        bool identity() const { return sx == 1 && sy == 1 && tx == 0 && ty == 0; }
        static Affine between(const fo::RectF &from, const fo::RectF &to);
        fo::RectF apply(const fo::RectF &r) const;
    };
    // What a frame paints while a zoom or a reveal runs.
    struct Plan {
        Affine live;        // the current layout's tiles
        double reveal = 1;  // children growing from their centres
        bool has_frame = false, has_back = false, has_front = false;
        fo::RectF frame;    // the moving block
        Affine back, front; // the layout behind (old parent) / in front (old child)
        bool moving = false, outline = false;
        bool transforms() const { return !live.identity() || reveal < 1; }
        fo::RectF place(const fo::RectF &r) const;
    };
    struct Zoom {
        bool active = false, in = true;
        bool has_from = false, has_parent = false, has_block = false;
        Snapshot from, parent;
        fo::RectF block; // the folder's block in the parent (absolute)
        Tile tile;       // the folder's own tile, for the moving block
        fo::Anim<double> t;
    };
    void remember(const std::string &path);
    const Snapshot *snapshot(const std::string &path) const;
    bool plan(const std::string &path, const fo::RectF &full, bool ready, Plan &out);
    void start(const std::string &previous, const std::string &path, bool ready);
    void paint_back(const Plan &plan, bool ready);
    void paint_front(const Plan &plan);
    void leave();
    void draw_ring(const fo::RectF *target, int radius);
    void draw_tip(int tip, int px, int py, bool pointer);
    bool label_fits(const Tile &t) const;
    Tile tile_of(const Block &b) const;
    static void draw_tile(int x, int y, int w, int h, const Tile &t, bool selected, bool hovered, bool focused);

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
    std::string layout_path_; // the folder blocks_ belongs to
    unsigned layout_gen_ = 0;
    // Motion state.
    std::string shown_; // the folder the map last painted
    std::deque<Snapshot> cache_;
    Zoom zoom_;
    std::string reveal_path_, hold_path_;
    double hold_since_ = 0;
    bool reveal_pending_ = false, came_in_ = false, animating_ = false;
    fo::Anim<double> reveal_;
    fo::Anim<fo::RectF> ring_;
    std::string ring_key_;
    bool ring_valid_ = false;
};

} // namespace flo::ui

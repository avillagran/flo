// Files' inspector: a preview tile, the name and kind, the total size of a
// folder with "Recalculate · Ctrl+Space", "Rename · F2", Open (or Restore
// in the trash) and Preview, and copy-path / F5 / F6 at the bottom. Facts
// that need reading the file (a picture's size, a text's first lines, a
// trashed item's origin) are read off the UI thread, and only while the
// inspector is shown.
#pragma once

#include "core/entry.h"

#include <FL/Fl_Group.H>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fo {
class Button;
}

namespace flo::ui {

class Pane;

class DetailsPane : public Fl_Group {
public:
    DetailsPane(int x, int y, int w, int h);
    ~DetailsPane() override;
    // Shows the cursor's item of `pane` (nullptr: nothing).
    void show_from(Pane *pane);
    void relabel(); // the language changed: describe the item again
    void resize(int x, int y, int w, int h) override;
    void draw() override;
    int handle(int event) override;

private:
    enum class Act { Measure, Rename, Open, Preview, CopyPath, Copy, Move };
    struct Btn {
        int x, y, w, h;
        std::string icon, label;
        bool primary;
        Act act;
    };
    void buttons(std::vector<Btn> &out) const;
    int hover_ = -1;
    struct Facts {
        std::string text;       // opening lines of a text file
        std::string dimensions; // "1600 × 1200"
        std::string origin;     // trashed: where it came from
        int64_t deleted_at = 0;
        bool ready = false;
    };
    void describe();
    Pane *pane_ = nullptr;
    std::string path_;
    Entry entry_;
    bool has_ = false, trashed_ = false;
    std::shared_ptr<Facts> facts_;
    uint64_t generation_ = 0;
};

// The size of a picture from its header (PNG, JPEG, GIF, WebP, BMP), or "".
std::string image_dimensions(const std::string &path);

} // namespace flo::ui

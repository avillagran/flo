// The details pane (Folio's PreviewPane): a larger look at the selected
// item, the facts about it, and the one action that matters (Open, or
// Restore in the trash). Facts that need reading the file (a picture's
// size, a text's first lines, a trashed item's origin) are read off the UI
// thread, and only while the pane is shown.
#pragma once

#include "core/entry.h"

#include <FL/Fl_Group.H>

#include <cstdint>
#include <memory>
#include <string>

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

private:
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
    fo::Button *open_ = nullptr;
};

// The size of a picture from its header (PNG, JPEG, GIF, WebP, BMP), or "".
std::string image_dimensions(const std::string &path);

} // namespace flo::ui

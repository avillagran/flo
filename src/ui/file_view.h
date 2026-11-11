// The files of a folder in a fo::Collection, drawn as Files draws them:
// Groups (folder cards under "FOLDERS · n", then the files as rows under
// date sections), List (38 px rows with size, date and kind columns),
// Grid (cards with thumbnails), the narrow rows of a column and the
// compact rows of a dual pane. Thumbnails come from the shared cache (the
// 256 px bucket for cards, 128 px for rows).
#pragma once

#include "ui/util.h"

#include "fltk-omarchy/collection.h"

#include <functional>
#include <string>
#include <vector>

namespace flo::ui {

// Files' date sections in Groups.
enum class AgeSection { Today, ThisWeek, Earlier, Undated };
AgeSection age_section(int64_t mtime_ns, int64_t now_s);

class FileView : public fo::Collection {
public:
    enum class Mode { Groups, List, Grid, Column, Dual };

    FileView(int x, int y, int w, int h, FolderModel *model);

    void mode(Mode m);
    Mode mode() const { return mode_; }
    FolderModel *model() const { return model_; }
    void model(FolderModel *m) { model_ = m; }

    // Groups: the model is ordered folders first, then the files by age
    // section; `ends` are the five boundaries (folders | today | week |
    // earlier | undated), as Files keeps them.
    void groups(const int ends[5]);
    // The new-folder button beside "FOLDERS".
    void on_new_folder(std::function<void()> fn) { new_folder_ = std::move(fn); }

    // Columns: the entry leading to the next column is lit.
    void trail(const std::string &name) { trail_ = name; redraw(); }
    // What an empty folder says ("Empty", "No matches").
    void empty_text(const std::string &t) { empty_ = t; redraw(); }
    // Search results show where they are.
    void show_folders(bool on) { show_folders_ = on; }

    int count() const override { return model_ ? model_->count() : 0; }
    std::string item_text(int i) const override;
    void draw_item(int i, int x, int y, int w, int h, const ItemState &s) override;
    void draw_band_header(int band, int x, int y, int w, int h) override;
    void band_header_clicked(int band, int x, int y) override;
    void draw() override;

    // Requested thumbnail sizes (large for cards, normal for rows).
    static constexpr int kGridPx = 256, kListPx = 128;
    // Files' list row and grid cell heights.
    static constexpr int kRowH = 38, kCellW = 156, kCellH = 132;

private:
    void draw_card_item(int i, int x, int y, int w, int h, const ItemState &s);
    void draw_tile_item(int i, int x, int y, int w, int h, const ItemState &s);
    void draw_row_item(int i, int x, int y, int w, int h, const ItemState &s);
    void draw_column_item(int i, int x, int y, int w, int h, const ItemState &s);
    void draw_badge(const Entry &e, const std::string &path, int x, int y, int size, fo::Rgb matte);
    FolderModel *model_;
    Mode mode_ = Mode::Groups;
    std::string trail_, empty_;
    bool show_folders_ = false;
    int ends_[5] = {0, 0, 0, 0, 0};
    int new_folder_x_ = 0, new_folder_y_ = 0;
    std::function<void()> new_folder_;
};

// Files' "now", "2 min ago", "3 h ago", "yesterday", "4 d ago".
std::string relative_when(int64_t mtime_ns);
// Files' kind words for the List view ("Folder", "Image", "PDF", ...).
std::string kind_word(const Entry &e);
// Files' badge color by type (images magenta, archives yellow, ...).
fo::Rgb badge_color(const Entry &e);
// A folder in a large box (the preview window): Files' folder glyph.
void draw_folder_card(int x, int y, int w, int h, fo::Rgb fill, fo::Rgb edge, int edge_px);
// "∑ 4.2 kB" when the folder was measured, its item count otherwise.
std::string folder_summary(const Entry &e, const std::string &path);

} // namespace flo::ui

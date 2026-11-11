// The files of a folder in a fo::Collection: Folio's grid tiles with real
// thumbnails (256 px bucket), list rows with small thumbnails (128 px
// bucket) and the size and date, or the narrow rows of a column.
#pragma once

#include "ui/util.h"

#include "fltk-omarchy/collection.h"

#include <string>

namespace flo::ui {

class FileView : public fo::Collection {
public:
    enum class Mode { Grid, List, Column };

    FileView(int x, int y, int w, int h, FolderModel *model);

    void mode(Mode m);
    Mode mode() const { return mode_; }
    FolderModel *model() const { return model_; }
    void model(FolderModel *m) { model_ = m; }

    // Columns: the entry leading to the next column is lit.
    void trail(const std::string &name) { trail_ = name; redraw(); }
    // What an empty folder says ("Empty", "No matches").
    void empty_text(const std::string &t) { empty_ = t; redraw(); }
    // Search results show where they are.
    void show_folders(bool on) { show_folders_ = on; }

    int count() const override { return model_ ? model_->count() : 0; }
    std::string item_text(int i) const override;
    void draw_item(int i, int x, int y, int w, int h, const ItemState &s) override;
    void draw() override;

    // Requested thumbnail sizes (Folio: large for the grid, normal for rows).
    static constexpr int kGridPx = 256, kListPx = 128;

private:
    void draw_tile_item(int i, int x, int y, int w, int h, const ItemState &s);
    void draw_row_item(int i, int x, int y, int w, int h, const ItemState &s);
    void draw_stand_in(const Entry &e, int x, int y, int w, int h, int &dx, int &dy, int &dw, int &dh);
    FolderModel *model_;
    Mode mode_ = Mode::Grid;
    std::string trail_, empty_;
    bool show_folders_ = false;
};

// Draws a folder card (Folio's silhouette: a tab on the top left).
void draw_folder_card(int x, int y, int w, int h, fo::Rgb fill, fo::Rgb edge, int edge_px);

} // namespace flo::ui

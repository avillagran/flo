// Columns, as in the macOS Finder (Files' Columns view): the folders on
// the way here from the root (~, a mounted machine, or /), the current
// folder, and what's inside the selected subfolder. Built from FileViews
// side by side; the newest columns stay in view as the trail grows.
#pragma once

#include "core/lister.h"
#include "ui/util.h"

#include <FL/Fl_Group.H>

#include <memory>
#include <string>
#include <vector>

namespace flo::ui {

class FileView;
class Pane;

class ColumnsView : public Fl_Group {
public:
    static constexpr int kColumn = 230; // Files' column width

    ColumnsView(int x, int y, int w, int h, Pane *pane);
    ~ColumnsView() override;

    FileView *here() const { return here_; }
    // The pane's folder changed: trim and extend the trail.
    void sync();
    // The selection moved: show inside the selected folder.
    void selection_changed();
    // Lists again (hidden files or the sort changed).
    void refresh();

    void resize(int x, int y, int w, int h) override;
    void draw() override;
    int handle(int event) override;

private:
    struct Column {
        std::string path, next;
        FolderModel model;
        std::unique_ptr<Lister> lister;
        FileView *view = nullptr;
    };
    void list(Column &c);
    void place();
    Column *make_column(const std::string &path);
    std::string root_of(const std::string &folder) const;

    Pane *pane_;
    std::vector<std::unique_ptr<Column>> trail_;
    FileView *here_ = nullptr;
    std::unique_ptr<Column> inside_;
    int scroll_x_ = 0;
};

} // namespace flo::ui

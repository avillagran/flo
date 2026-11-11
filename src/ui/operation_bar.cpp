#include "ui/operation_bar.h"

#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/text.h"
#include "fltk-omarchy/theme.h"

#include <FL/Fl.H>

namespace flo::ui {

namespace {
struct Item {
    const char *key, *icon, *text, *tip;
};
// Files' ITEMS: key, icon, short text, accessible name.
const Item kItems[OperationBar::kButtons] = {
    {"F1", "help", "Help", "Help and shortcuts"},
    {"F2", "pencil", "Rename", "Rename the selection"},
    {"F3", "eye", "Preview", "Preview the focused item"},
    {"F5", "copy", "Copy", "Copy the selection: review the destination"},
    {"F6", "arrow-right", "Move", "Move the selection: review the destination"},
    {"F7", "folder-plus", "Folder", "Create a folder"},
    {"F8", "trash", "Trash", "Send the selection to the trash"},
    {"F10", "zap", "Actions", "Actions on files"},
};
} // namespace

OperationBar::OperationBar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}

int OperationBar::at(int ex) const {
    if (w() <= 0)
        return -1;
    int i = (ex - x()) * kButtons / w();
    return i >= 0 && i < kButtons ? i : -1;
}

void OperationBar::draw() {
    const fo::Theme &t = fo::theme();
    fo::paint::fill(x(), y(), w(), h(), t.dark_background());
    fo::paint::hline(x(), y(), w(), t.lighter_background());
    const int r = t.radius() > 0 ? 4 : 0;
    for (int i = 0; i < kButtons; ++i) {
        const int bx = x() + i * w() / kButtons, end = x() + (i + 1) * w() / kButtons;
        const int ax = bx + 2, ay = y() + 3, aw = end - bx - 4, ah = 30;
        const bool on = !reason_ || reason_(i).empty();
        fo::paint::round_fill(ax, ay, aw, ah, r, on ? (i == hover_ ? t.selection() : t.lighter_background())
                                                    : t.dark_background());
        const fo::Rgb fg = on ? t.foreground() : t.muted();
        const char *icon = i == 3 && space_ ? "refresh" : kItems[i].icon;
        const std::string text = fo::tr(i == 3 && space_ ? "Recalculate" : kItems[i].text);
        // Files: "F5 icon Copy" from 960 px, "F5 Copy" from 720, else "F5 icon".
        const int W = w();
        int tx = ax + 6;
        fo::text::draw(kItems[i].key, tx, ay, aw - 12, ah, fg, fo::text::REGULAR, 11, FL_ALIGN_LEFT,
                       fo::text::Elide::None);
        tx += int(fo::text::width(kItems[i].key, fo::text::REGULAR, 11)) + 6;
        if (W >= 960 || W < 720) {
            fo::icon::draw(icon, tx, ay + 9, 12, fg);
            tx += 18;
        }
        if (W >= 720)
            fo::text::draw(text, tx, ay, ax + aw - 4 - tx, ah, fg, fo::text::REGULAR, 11);
    }
}

int OperationBar::handle(int event) {
    switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
        int i = at(Fl::event_x());
        if (i != hover_) {
            hover_ = i;
            tooltip(i >= 0 ? fo::tr(kItems[i].tip) : nullptr);
            redraw();
        }
        return 1;
    }
    case FL_LEAVE:
        hover_ = -1;
        redraw();
        return 1;
    case FL_PUSH: return 1;
    case FL_RELEASE: {
        int i = at(Fl::event_x());
        if (i >= 0 && Fl::event_inside(this) && run_)
            run_(i);
        return 1;
    }
    }
    return Fl_Widget::handle(event);
}

} // namespace flo::ui

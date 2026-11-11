#include "ui/favorites.h"

#include "core/fsutil.h"
#include "core/places.h"
#include "core/xdg.h"
#include "ui/app.h"
#include "ui/commands.h"
#include "ui/main_window.h"
#include "ui/pane.h"
#include "ui/util.h"

#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/keys.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/text.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

namespace flo::ui {

namespace {
const int kRowPitch = 44, kRowH = 42;
}

FavoritesHub *FavoritesHub::open(fo::Window *win, Pane *pane) {
    if (!win || !pane || win->modal_open())
        return nullptr;
    auto *h = new FavoritesHub();
    h->pane_ = pane;
    h->begin();
    h->field_ = new fo::TextField(0, 0, 100, 32);
    h->end();
    h->field_->placeholder(fo::tr("Filter by label"));
    h->field_->on_change([h] {
        if (!h->editing_) {
            h->list_focus_ = false;
            h->refilter();
        }
    });
    h->field_->on_escape([h] {
        if (h->editing_) {
            h->editing_ = false;
            h->field_->set_text(h->saved_query_, int(h->saved_query_.size()), int(h->saved_query_.size()));
            h->refilter();
            return;
        }
        h->close();
    });
    h->field_->on_enter([h] { h->editing_ ? h->save_label() : h->jump(); });
    h->field_->on_key([h](int k) { return h->key(k); });
    h->fo::Layer::open(win);
    h->reload();
    h->place();
    h->field_->take_focus();
    h->fade_in();
    return h;
}

void FavoritesHub::reload() {
    all_.clear();
    for (const auto &b : places::read_bookmarks())
        if (!b.path.empty())
            all_.push_back({b.label.empty() ? path_basename(b.path) : b.label, b.path, 0});
    // Files: the XDG places (Home first).
    for (const auto &p : App::get().places().places)
        all_.push_back({fo::tr(p.name.c_str()), p.path, 1});
    const auto &back = pane_->history_back();
    for (auto it = back.rbegin(); it != back.rend(); ++it)
        if (!is_virtual_folder(*it))
            all_.push_back({path_basename(*it).empty() ? *it : path_basename(*it), *it, 2});
    const auto &fwd = pane_->history_forward();
    for (auto it = fwd.rbegin(); it != fwd.rend(); ++it)
        if (!is_virtual_folder(*it))
            all_.push_back({path_basename(*it).empty() ? *it : path_basename(*it), *it, 3});
    refilter();
}

void FavoritesHub::refilter() {
    std::string keep = current() ? current()->path : std::string();
    shown_.clear();
    std::string q = editing_ ? saved_query_ : std::string(field_->value());
    for (size_t i = 0; i < all_.size(); ++i)
        if (command_matches(q, all_[i].label))
            shown_.push_back(int(i));
    hi_ = 0;
    for (size_t i = 0; i < shown_.size(); ++i)
        if (all_[size_t(shown_[i])].path == keep)
            hi_ = int(i);
    move(hi_);
    redraw();
}

const FavoritesHub::Row *FavoritesHub::current() const {
    if (hi_ < 0 || hi_ >= int(shown_.size()))
        return nullptr;
    return &all_[size_t(shown_[size_t(hi_)])];
}

void FavoritesHub::move(int to) {
    int n = int(shown_.size());
    hi_ = n ? std::clamp(to, 0, n - 1) : 0;
    if (hi_ < top_)
        top_ = hi_;
    if (hi_ >= top_ + rows_)
        top_ = hi_ - rows_ + 1;
    top_ = std::clamp(top_, 0, std::max(0, n - rows_));
    redraw();
}

void FavoritesHub::notice(const std::string &text, bool error) {
    notice_ = text;
    notice_error_ = error;
    redraw();
}

void FavoritesHub::jump() {
    const Row *r = current();
    if (!r)
        return;
    std::string to = r->path;
    Pane *p = pane_;
    close();
    defer([p, to] { p->navigate(to); });
}

void FavoritesHub::add_current() {
    std::string here = pane_->folder();
    if (here.empty() || here[0] != '/')
        return;
    if (places::is_pinned(here)) {
        notice(fo::tr("The folder is already a favorite"));
        return;
    }
    std::string why = places::pin(here);
    if (!why.empty()) {
        notice(fo::tr(why.c_str()), true);
        return;
    }
    App::get().refresh_places();
    reload();
    notice(fo::tr("Favorite added"));
}

void FavoritesHub::begin_label() {
    const Row *r = current();
    if (!r || r->kind != 0)
        return;
    editing_ = true;
    edit_path_ = r->path;
    saved_query_ = field_->value();
    field_->set_text(r->label, 0, int(r->label.size()));
    field_->take_focus();
    redraw();
}

void FavoritesHub::save_label() {
    std::string label = field_->value();
    std::string trimmed = label;
    while (!trimmed.empty() && trimmed.back() == ' ')
        trimmed.pop_back();
    if (trimmed.empty() || trimmed.size() > 96) {
        notice(fo::tr("A label holds 1 to 96 bytes."), true);
        return;
    }
    std::string why = places::relabel(edit_path_, label);
    if (!why.empty()) {
        notice(fo::tr(why.c_str()), true);
        return;
    }
    editing_ = false;
    field_->set_text(saved_query_, int(saved_query_.size()), int(saved_query_.size()));
    App::get().refresh_places();
    reload();
    notice(fo::tr("Label updated"));
}

void FavoritesHub::remove_favorite() {
    const Row *r = current();
    if (!r || r->kind != 0)
        return;
    places::unpin(r->path);
    App::get().refresh_places();
    reload();
    notice(fo::tr("Favorite removed; the folder is untouched"));
}

bool FavoritesHub::button_enabled(int b) const {
    const Row *r = current();
    switch (b) {
    case kEnter: return editing_ || r;
    case kAdd: return !editing_;
    case kLabel:
    case kRemove: return !editing_ && r && r->kind == 0;
    default: return !editing_;
    }
}

void FavoritesHub::press(int b) {
    if (!button_enabled(b))
        return;
    switch (b) {
    case kEnter: editing_ ? save_label() : jump(); break;
    case kAdd: add_current(); break;
    case kLabel: begin_label(); break;
    case kRemove: remove_favorite(); break;
    case kReload:
        reload();
        notice(fo::tr("Favorites loaded"));
        break;
    }
}

bool FavoritesHub::key(int k) {
    int st = Fl::event_state() & (FL_CTRL | FL_ALT | FL_SHIFT | FL_META);
    if (fo::event_is("Ctrl+W") || fo::event_is("Ctrl+Alt+D")) {
        close();
        return true;
    }
    if (editing_)
        return false; // the field edits the label; Enter saves, Esc cancels
    if (k == FL_F + 2) {
        begin_label();
        return true;
    }
    if (k == FL_F + 5 || fo::event_is("Ctrl+R")) {
        press(kReload);
        return true;
    }
    if (fo::event_is("Ctrl+D")) {
        add_current();
        return true;
    }
    if (k == FL_Delete && list_focus_ && !st) {
        remove_favorite();
        return true;
    }
    if (!st || st == FL_SHIFT) {
        int step = 0;
        if (k == FL_Down) step = 1;
        else if (k == FL_Up) step = -1;
        else if (k == FL_Page_Down) step = rows_;
        else if (k == FL_Page_Up) step = -rows_;
        else if (k == FL_Home && list_focus_) step = -int(shown_.size());
        else if (k == FL_End && list_focus_) step = int(shown_.size());
        if (step) {
            list_focus_ = true;
            move(hi_ + step);
            return true;
        }
    }
    if (k != FL_Delete && k != FL_BackSpace && Fl::event_length() > 0)
        list_focus_ = false;
    return false;
}

void FavoritesHub::place() {
    // Files: (12, 66, w - 24, h - 106) over the window.
    rx_ = 12, ry_ = 66, rw_ = std::max(200, w() - 24), rh_ = std::max(260, h() - 106);
    field_->resize(rx_ + 14, ry_ + 60, rw_ - 28, 32);
    list_y_ = ry_ + 104;
    list_h_ = std::max(0, rh_ - 230);
    rows_ = std::max(1, list_h_ / kRowPitch);
    move(hi_);
}

void FavoritesHub::relayout() { place(); }

int FavoritesHub::handle(int event) {
    switch (event) {
    case FL_PUSH: {
        int ex = Fl::event_x(), ey = Fl::event_y();
        if (ex < rx_ || ex >= rx_ + rw_ || ey < ry_ || ey >= ry_ + rh_ ||
            (ey < ry_ + 34 && ex >= rx_ + rw_ - 84)) { // outside, or "× Esc"
            close();
            return 1;
        }
        int bw = (rw_ - 36) / 3;
        for (int b = 0; b < kButtons; ++b) {
            int bx = rx_ + 14 + (b % 3) * (bw + 4), by = ry_ + rh_ - 84 + (b / 3) * 36;
            if (ex >= bx && ex < bx + bw && ey >= by && ey < by + 30) {
                press(b);
                return 1;
            }
        }
        if (ey >= list_y_ && ey < list_y_ + rows_ * kRowPitch) {
            int i = top_ + (ey - list_y_) / kRowPitch;
            if (i < int(shown_.size())) {
                list_focus_ = true;
                move(i);
                if (Fl::event_clicks())
                    jump();
            }
            return 1;
        }
        break;
    }
    case FL_MOUSEWHEEL:
        list_focus_ = true;
        move(hi_ + (Fl::event_dy() > 0 ? 1 : -1));
        return 1;
    }
    int r = fo::Layer::handle(event);
    return r ? r : 1;
}

void FavoritesHub::draw() {
    const fo::Theme &t = fo::theme();
    const int rad = t.radius() > 0 ? 8 : 0, r4 = t.radius() > 0 ? 4 : 0;
    fo::paint::round_fill(rx_, ry_, rw_, rh_, rad, t.card());
    fo::paint::round_fill(rx_ + 1, ry_ + 1, rw_ - 2, rh_ - 2, rad, t.background());
    fo::icon::draw("star", rx_ + 14, ry_ + 11, 15, t.yellow());
    fo::text::draw(fo::tr("Favorites and places"), rx_ + 36, ry_ + 4, rw_ - 130, 28, t.foreground(),
                   fo::text::REGULAR, 16);
    fo::text::draw("\xC3\x97 Esc", rx_ + rw_ - 84, ry_ + 6, 70, 28, t.accent(), fo::text::REGULAR, 11, FL_ALIGN_RIGHT);
    fo::text::draw(editing_ ? fo::tr("Favorite label \xC2\xB7 does not rename the folder")
                            : fo::tr("Filter by label \xC2\xB7 favorites / XDG / history"),
                   rx_ + 14, ry_ + 36, rw_ - 28, 20, t.dim(), fo::text::REGULAR, 11);
    draw_children();
    static const char *kinds[] = {"star", "folder", "arrow-left", "arrow-right"};
    fl_push_clip(rx_ + 14, list_y_, rw_ - 28, rows_ * kRowPitch);
    for (int k = 0; k < rows_ && top_ + k < int(shown_.size()); ++k) {
        int i = top_ + k;
        const Row &r = all_[size_t(shown_[size_t(i)])];
        int x = rx_ + 14, y = list_y_ + k * kRowPitch, w = rw_ - 28;
        if (i == hi_) {
            fo::paint::round_fill(x, y, w, kRowH, r4, t.selection());
            if (list_focus_)
                fo::paint::fill(x + 1, y + 6, 2, kRowH - 12, t.accent());
        }
        fo::icon::draw(kinds[r.kind], x + 8, y + 5, 12, r.kind == 0 ? t.yellow() : t.dim());
        fo::text::draw(r.label, x + 26, y, w - 32, 22, t.foreground(), fo::text::REGULAR, 12);
        fo::text::draw(tilde(r.path), x + 26, y + 22, w - 32, 18, t.dim(), fo::text::REGULAR, 10, FL_ALIGN_LEFT,
                       fo::text::Elide::Middle);
    }
    fl_pop_clip();
    if (shown_.empty())
        fo::text::draw(fo::tr("No matches"), rx_ + 14, list_y_ + 8, rw_ - 28, 24, t.dim(), fo::text::REGULAR, 12);
    int favs = int(std::count_if(all_.begin(), all_.end(), [](const Row &r) { return r.kind == 0; }));
    std::string status = !notice_.empty() ? notice_
                                          : fo::trn("%n favorites", favs) + " \xC2\xB7 " +
                                                fo::tr("F2 label \xC2\xB7 Delete removes");
    fo::text::draw(status, rx_ + 14, ry_ + rh_ - 120, rw_ - 28, 30, notice_error_ ? t.red() : t.dim(),
                   fo::text::REGULAR, 11);
    struct B {
        const char *icon, *label;
    };
    const B bs[kButtons] = {{"folder-open", editing_ ? "Save" : "Enter"},
                            {"star", "Current"},
                            {"pencil", "Label"},
                            {"x", "Remove"},
                            {"refresh", "F5"}};
    int bw = (rw_ - 36) / 3;
    for (int b = 0; b < kButtons; ++b) {
        int bx = rx_ + 14 + (b % 3) * (bw + 4), by = ry_ + rh_ - 84 + (b / 3) * 36;
        bool on = button_enabled(b);
        fo::paint::round_fill(bx, by, bw, 30, r4, on ? t.card() : t.background());
        fo::icon::draw(bs[b].icon, bx + 10, by + 9, 12, on ? t.accent() : t.muted());
        fo::text::draw(fo::tr(bs[b].label), bx + 28, by, bw - 34, 30, on ? t.accent() : t.muted(), fo::text::REGULAR,
                       11);
    }
}

} // namespace flo::ui

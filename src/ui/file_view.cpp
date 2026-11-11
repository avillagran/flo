#include "ui/file_view.h"

#include "ui/app.h"
#include "ui/thumbs.h"

#include "core/fsutil.h"

#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

namespace flo::ui {

void draw_folder_card(int x, int y, int w, int h, fo::Rgb fill, fo::Rgb edge, int edge_px) {
    const fo::Theme &t = fo::theme();
    int r = std::min(3, t.radius_small());
    int tab_w = w * 42 / 128, tab_h = std::max(5, h * 6 / 92);
    int bx = x + w * 4 / 128, bw = w - 2 * (w * 4 / 128);
    fo::paint::round_fill(bx, y, tab_w, tab_h + r + 1, r, fill);
    fo::paint::round_fill(bx, y + tab_h, bw, h - tab_h, r, fill);
    if (edge_px > 0) {
        fo::paint::round_frame(bx, y + tab_h, bw, h - tab_h, r, edge_px, edge);
        fo::paint::fill(bx, y, edge_px, tab_h + edge_px, edge);
        fo::paint::fill(bx, y, tab_w, edge_px, edge);
        fo::paint::line(bx + tab_w - 0.5, y + 0.5, bx + tab_w + tab_h * 0.6, y + tab_h, edge_px, edge);
    }
}

FileView::FileView(int x, int y, int w, int h, FolderModel *model) : fo::Collection(x, y, w, h), model_(model) {
    focus_ring_on_selected(false); // the selection is drawn on the item itself
    mode(Mode::Grid);
}

void FileView::mode(Mode m) {
    mode_ = m;
    if (m == Mode::Grid) {
        layout(Layout::Grid);
        tile_size(172, 192);
        padding(16);
    } else {
        layout(Layout::List);
        row_height(32);
        padding(m == Mode::List ? 12 : 8);
    }
    redraw();
}

std::string FileView::item_text(int i) const {
    return model_ && i >= 0 && i < model_->count() ? display_name(model_->entries[size_t(i)]) : std::string();
}

void FileView::draw() {
    fo::Collection::draw();
    if (count() == 0 && model_ && !model_->loading && model_->error.empty() && !empty_.empty()) {
        const fo::Theme &t = fo::theme();
        fl_push_clip(x(), y(), w(), h());
        if (mode_ == Mode::Column) {
            fo::text::draw(empty_, x() + 8 + 14 + 20 + 12, y() + 8, w() - 60, 32, t.secondary_foreground());
        } else {
            int cy = y() + std::max(40, h() / 2 - 60);
            fo::icon::draw("folder-open", x() + w() / 2 - 24, cy, 48, t.tint(t.foreground(), 0.25));
            fo::text::draw(empty_, x(), cy + 60, w(), 24, t.secondary_foreground(), fo::text::REGULAR,
                           fo::text::size::BODY, FL_ALIGN_CENTER);
        }
        fl_pop_clip();
    }
}

void FileView::draw_item(int i, int x, int y, int w, int h, const ItemState &s) {
    if (!model_ || i < 0 || i >= model_->count())
        return;
    if (mode_ == Mode::Grid)
        draw_tile_item(i, x, y, w, h, s);
    else
        draw_row_item(i, x, y, w, h, s);
}

// Stand-ins while a thumbnail loads or when none can be made: a page with
// the extension for documents, a waveform for audio, a frame for video.
void FileView::draw_stand_in(const Entry &e, int x, int y, int w, int h, int &dx, int &dy, int &dw, int &dh) {
    const fo::Theme &t = fo::theme();
    fo::Rgb kc = kind_color(e);
    if (e.kind == Kind::Audio) {
        dx = x, dy = y, dw = w, dh = h;
        fo::paint::fill(x, y, w, h, t.lighter_background());
        int bars = 18, bw = 3, gap = (w - 24 - bars * bw) / (bars - 1);
        for (int b = 0; b < bars; ++b) {
            int bh = 10 + ((b * 37 + 11) % 41);
            fo::paint::fill(x + 12 + b * (bw + gap), y + (h - bh) / 2, bw, bh, kc, 0.85);
        }
    } else if (e.kind == Kind::Video) {
        dx = x, dy = y, dw = w, dh = h;
        fo::paint::fill(x, y, w, h, t.lighter_background());
        fo::paint::fill(x + w / 2 - 15, y + h / 2 - 15, 30, 30, t.dark_background(), 0.8);
        fo::icon::draw("play", x + w / 2 - 8, y + h / 2 - 8, 16, kc);
    } else {
        // A page, as tall as the box.
        dh = h, dw = h * 72 / 92;
        dx = x + (w - dw) / 2, dy = y;
        bool light_page = e.kind == Kind::Pdf;
        fo::paint::fill(dx, dy, dw, dh, light_page ? fo::Rgb(0xf4, 0xf4, 0xf0) : t.lighter_background());
        fo::paint::round_frame(dx, dy, dw, dh, 0, 1, t.hairline());
        fo::Rgb line = light_page ? fo::Rgb(0x9a, 0x9a, 0x9a) : t.tint(t.foreground(), 0.25);
        fo::paint::fill(dx + 8, dy + 10, dw * 45 / 100, 3, kc);
        for (int l = 0; l < 5; ++l)
            fo::paint::fill(dx + 8, dy + 20 + l * 8, dw - 16 - (l % 2) * 10, 1, line);
        std::string ext = e.extension();
        if (!ext.empty() && ext.size() <= 5) {
            int ew = int(fo::text::width(ext, fo::text::BOLD, 9)) + 8;
            fo::paint::fill(dx, dy + dh - 16, ew, 14, kc);
            fo::text::draw(ext, dx, dy + dh - 16, ew, 14, t.dark_background(), fo::text::BOLD, 9, FL_ALIGN_CENTER,
                           fo::text::Elide::None);
        }
    }
}

void FileView::draw_tile_item(int i, int X, int Y, int W, int H, const ItemState &s) {
    const fo::Theme &t = fo::theme();
    const Entry &e = model_->entries[size_t(i)];
    const std::string path = model_->path(i);
    const int aw = std::min(128, W - 28), ah = aw * 92 / 128;
    const int ax = X + (W - aw) / 2, ay = Y + 18;
    bool lit = s.selected || s.drop_target;
    if (s.hover > 0 && !lit)
        fo::paint::round_fill(X + 8, Y + 4, W - 16, H - 8, t.radius_small(), t.foreground(), 0.03 * s.hover);
    int dx = ax, dy = ay, dw = aw, dh = ah;
    if (e.is_dir) {
        fo::Rgb fill = lit ? fo::mix(t.lighter_background(), t.accent(), 0.3)
                           : s.hover > 0 ? fo::mix(t.lighter_background(), t.foreground(), 0.06 * s.hover)
                                         : t.lighter_background();
        draw_folder_card(ax, ay, aw, ah, fill, t.accent(), lit ? (s.drop_target ? 2 : 1) : 0);
        draw_peek(path, ax, ay, aw, ah);
    } else {
        bool crop = e.kind == Kind::Image || e.kind == Kind::Video;
        Fl_RGB_Image *img = App::get().thumbs().get(path, e, kGridPx, aw, ah, crop, this);
        if (img) {
            dw = img->w(), dh = img->h();
            dx = ax + (aw - dw) / 2, dy = ay + (ah - dh) / 2;
            img->draw(dx, dy);
            if (e.kind == Kind::Video) {
                fo::paint::fill(ax + aw / 2 - 15, ay + ah / 2 - 15, 30, 30, t.dark_background(), 0.75);
                fo::icon::draw("play", ax + aw / 2 - 8, ay + ah / 2 - 8, 16, t.bright_foreground());
            }
        } else {
            draw_stand_in(e, ax, ay, aw, ah, dx, dy, dw, dh);
        }
        // Selection hugs what is drawn.
        if (lit)
            fo::paint::round_frame(dx - 3, dy - 3, dw + 6, dh + 6, t.radius_small(), 2, t.accent());
    }
    // The name in up to two lines, then size and date.
    const std::string name = display_name(e);
    auto lines = fo::text::wrap(name, W - 28, fo::text::fltk_measure(fo::text::REGULAR, fo::text::size::LABEL), 2);
    int ly = ay + ah + 12;
    for (const auto &l : lines) {
        fo::text::draw(l, X + 14, ly, W - 28, 16, lit ? t.accent() : t.foreground(),
                       fo::text::REGULAR, fo::text::size::LABEL, FL_ALIGN_CENTER, fo::text::Elide::None);
        ly += 16;
    }
    std::string sub = size_text(e);
    std::string when = when_text(e.mtime_ns);
    if (!when.empty())
        sub = sub.empty() ? when : sub + " \xC2\xB7 " + when;
    if (!sub.empty())
        fo::text::draw(sub, X + 14, ly + 3, W - 28, 14, t.secondary_foreground(), fo::text::REGULAR,
                       fo::text::size::SMALL - 1, FL_ALIGN_CENTER);
}

// A folder's peek: its first one or two pictures side by side on the card,
// from the shared thumbnail cache (the grid's 256 px bucket, so opening the
// folder finds them made), scaled to a few KB each.
void FileView::draw_peek(const std::string &folder, int ax, int ay, int aw, int ah) {
    auto it = model_->peeks.find(folder);
    if (it == model_->peeks.end() || it->second.empty())
        return;
    const auto &pics = it->second;
    const int tab_h = std::max(5, ah * 6 / 92), inset = std::max(5, aw * 7 / 128);
    const int bx = ax + aw * 4 / 128 + inset, bw = aw - 2 * (aw * 4 / 128) - 2 * inset;
    const int by = ay + tab_h + inset, bh = ah - tab_h - 2 * inset;
    const int n = int(std::min<std::size_t>(2, pics.size())), gap = 4;
    const int pw = n == 1 ? bw : (bw - gap) / 2;
    if (pw < 8 || bh < 8)
        return;
    for (int k = 0; k < n; ++k) {
        const Entry &pe = pics[size_t(k)];
        Fl_RGB_Image *img = App::get().thumbs().get(path_join(folder, pe.name), pe, kGridPx, pw, bh, true, this);
        if (img)
            img->draw(bx + k * (pw + gap) + (pw - img->w()) / 2, by + (bh - img->h()) / 2);
    }
}

void FileView::draw_row_item(int i, int X, int Y, int W, int H, const ItemState &s) {
    const fo::Theme &t = fo::theme();
    const Entry &e = model_->entries[size_t(i)];
    const std::string path = model_->path(i);
    const std::string name = display_name(e);
    bool on_trail = mode_ == Mode::Column && !trail_.empty() && name == trail_;
    int r = std::min(4, t.radius_small());
    if (s.selected || s.drop_target)
        fo::paint::round_fill(X + 4, Y, W - 8, H, r, t.accent(), 0.18);
    else if (on_trail)
        fo::paint::round_fill(X + 4, Y, W - 8, H, r, t.foreground(), 0.08);
    else if (s.hover > 0)
        fo::paint::round_fill(X + 4, Y, W - 8, H, r, t.foreground(), 0.05 * s.hover);
    if (s.drop_target)
        fo::paint::round_frame(X + 4, Y, W - 8, H, r, 2, t.accent());
    int ix = X + 14, iy = Y + (H - 20) / 2;
    Fl_RGB_Image *img = nullptr;
    if (!e.is_dir && (e.kind == Kind::Image || e.kind == Kind::Video))
        img = App::get().thumbs().get(path, e, kListPx, 20, 20, true, this);
    if (img)
        img->draw(ix + (20 - img->w()) / 2, iy + (20 - img->h()) / 2);
    else
        fo::icon::draw(kind_icon(e), ix + 2, iy + 2, 16, e.is_dir ? t.accent() : t.secondary_foreground());
    int lx = ix + 20 + 12, right = X + W - 14;
    if (mode_ == Mode::Column) {
        if (e.is_dir) {
            fo::icon::draw("chevron-right", right - 14, Y + (H - 14) / 2, 14,
                           s.selected ? t.accent() : t.secondary_foreground());
            right -= 22;
        }
    } else {
        // The name keeps its room: the date steps aside first, then the size.
        if (W >= 480) {
            std::string when = when_text(e.mtime_ns);
            fo::text::draw(when, right - 96, Y, 96, H, t.secondary_foreground(), fo::text::REGULAR,
                           fo::text::size::LABEL);
            right -= 96 + 12;
        }
        if (W >= 380) {
            fo::text::draw(size_text(e), right - 96, Y, 96, H, t.secondary_foreground(), fo::text::REGULAR,
                           fo::text::size::LABEL, FL_ALIGN_RIGHT);
            right -= 96 + 12;
        }
    }
    std::string label = name;
    if (show_folders_ && e.name.find('/') != std::string::npos) {
        std::string where = e.name.substr(0, e.name.rfind('/'));
        int nw = int(fo::text::width(name, fo::text::REGULAR, fo::text::size::BODY)) + 1;
        int avail = right - lx;
        fo::text::draw(name, lx, Y, std::min(nw, avail), H, s.selected ? t.accent() : t.foreground(), fo::text::REGULAR,
                       fo::text::size::BODY, FL_ALIGN_LEFT, fo::text::Elide::Middle);
        if (nw + 16 < avail)
            fo::text::draw(where, lx + nw + 12, Y, avail - nw - 12, H, t.secondary_foreground(), fo::text::REGULAR,
                           fo::text::size::LABEL, FL_ALIGN_LEFT, fo::text::Elide::Middle);
        return;
    }
    fo::text::draw(label, lx, Y, right - lx, H, s.selected ? t.accent() : t.foreground(), fo::text::REGULAR,
                   fo::text::size::BODY, FL_ALIGN_LEFT, fo::text::Elide::Middle);
}

} // namespace flo::ui

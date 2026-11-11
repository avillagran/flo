#include "ui/space_view.h"

#include "core/fsutil.h"
#include "core/tree_size.h"
#include "ui/app.h"
#include "ui/file_view.h"
#include "ui/main_window.h"
#include "ui/pane.h"
#include "ui/sizes.h"

#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

namespace flo::ui {

namespace {
const int kHeader = 30, kFooter = 24;

fo::Rgb block_color(const std::string &name, bool dir) {
    const fo::Theme &t = fo::theme();
    if (dir)
        return t.blue();
    fo::Rgb cycle[] = {t.magenta(), t.cyan(), t.yellow(), t.green(), t.blue(), t.orange()};
    unsigned h = 2166136261u;
    for (char c : name)
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return cycle[h % 6];
}
} // namespace

SpaceView::SpaceView(int x, int y, int w, int h, Pane *pane) : Fl_Widget(x, y, w, h), pane_(pane) {
    box(FL_NO_BOX);
    set_visible_focus();
}

std::vector<SpaceView::Block> SpaceView::squarify(const std::vector<std::pair<int, uint64_t>> &sizes, int W, int H) {
    std::vector<Block> out;
    double total = 0;
    for (const auto &s : sizes)
        total += double(s.second);
    if (total <= 0 || W <= 0 || H <= 0)
        return out;
    double scale = double(W) * double(H) / total;
    double rx = 0, ry = 0, rw = W, rh = H;
    size_t i = 0;
    auto worst = [](const std::vector<double> &row, double side) {
        double sum = 0, mx = 0, mn = 1e300;
        for (double a : row) {
            sum += a;
            mx = std::max(mx, a);
            mn = std::min(mn, a);
        }
        if (sum <= 0 || mn <= 0)
            return 1e300;
        double s2 = side * side, sum2 = sum * sum;
        return std::max(s2 * mx / sum2, sum2 / (s2 * mn));
    };
    while (i < sizes.size()) {
        double side = std::min(rw, rh);
        std::vector<double> row;
        size_t j = i;
        while (j < sizes.size()) {
            std::vector<double> next = row;
            next.push_back(double(sizes[j].second) * scale);
            if (!row.empty() && worst(next, side) > worst(row, side))
                break;
            row = std::move(next);
            ++j;
        }
        double sum = 0;
        for (double a : row)
            sum += a;
        bool horizontal = rw >= rh; // lay the row along the short side
        double thick = side > 0 ? sum / side : 0;
        double pos = 0;
        for (size_t k = 0; k < row.size(); ++k) {
            double len = thick > 0 ? row[k] / thick : 0;
            Block b;
            b.index = sizes[i + k].first;
            b.bytes = sizes[i + k].second;
            if (horizontal) {
                b.x = int(std::lround(rx)), b.y = int(std::lround(ry + pos));
                b.w = int(std::lround(rx + thick)) - b.x;
                b.h = int(std::lround(ry + pos + len)) - b.y;
            } else {
                b.x = int(std::lround(rx + pos)), b.y = int(std::lround(ry));
                b.w = int(std::lround(rx + pos + len)) - b.x;
                b.h = int(std::lround(ry + thick)) - b.y;
            }
            pos += len;
            out.push_back(b);
        }
        if (horizontal) {
            rx += thick;
            rw -= thick;
        } else {
            ry += thick;
            rh -= thick;
        }
        i = j;
    }
    return out;
}

void SpaceView::sync() {
    layout();
    redraw();
}

void SpaceView::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    layout();
}

void SpaceView::layout() {
    const FolderModel &m = pane_->model();
    std::vector<std::pair<int, uint64_t>> sizes;
    total_ = 0;
    unsized_ = 0;
    for (int i = 0; i < m.count(); ++i) {
        const Entry &e = m.entries[size_t(i)];
        uint64_t b = 0;
        if (e.is_dir) {
            if (const FolderSizes::Result *r = App::get().sizes().get(m.path(i)))
                b = r->bytes;
        } else {
            b = uint64_t(std::max<int64_t>(0, e.size));
        }
        if (b == 0) {
            ++unsized_;
            continue;
        }
        total_ += b;
        sizes.push_back({i, b});
    }
    std::stable_sort(sizes.begin(), sizes.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    map_x_ = x(), map_y_ = y() + kHeader, map_w_ = w() - 4, map_h_ = std::max(0, h() - kHeader - kFooter);
    blocks_ = squarify(sizes, map_w_, map_h_);
    int cur = pane_->current();
    focus_ = -1;
    for (size_t b = 0; b < blocks_.size(); ++b)
        if (blocks_[b].index == cur)
            focus_ = int(b);
    hover_ = -1;
}

int SpaceView::block_at(int ex, int ey) const {
    for (size_t b = 0; b < blocks_.size(); ++b) {
        const Block &k = blocks_[b];
        if (ex >= map_x_ + k.x && ex < map_x_ + k.x + k.w && ey >= map_y_ + k.y && ey < map_y_ + k.y + k.h)
            return int(b);
    }
    return -1;
}

int SpaceView::nearest(int from, int dx, int dy) const {
    if (from < 0)
        return blocks_.empty() ? -1 : 0;
    const Block &f = blocks_[size_t(from)];
    double cx = f.x + f.w / 2.0, cy = f.y + f.h / 2.0;
    int best = -1;
    double best_d = 1e300;
    for (size_t b = 0; b < blocks_.size(); ++b) {
        if (int(b) == from)
            continue;
        const Block &k = blocks_[b];
        // Only blocks whose facing edge lies in that direction.
        if (dx > 0 && k.x < f.x + f.w) continue;
        if (dx < 0 && k.x + k.w > f.x) continue;
        if (dy > 0 && k.y < f.y + f.h) continue;
        if (dy < 0 && k.y + k.h > f.y) continue;
        double kx = k.x + k.w / 2.0, ky = k.y + k.h / 2.0;
        // Prefer blocks sharing the row or column span.
        bool shares = dx ? (k.y < f.y + f.h && k.y + k.h > f.y) : (k.x < f.x + f.w && k.x + k.w > f.x);
        double d = std::hypot(kx - cx, ky - cy) + (shares ? 0 : 10000);
        if (d < best_d) {
            best_d = d;
            best = int(b);
        }
    }
    return best < 0 ? from : best;
}

void SpaceView::focus_block(int b) {
    if (b < 0 || b >= int(blocks_.size()))
        return;
    focus_ = b;
    pane_->files()->set_current(blocks_[size_t(b)].index, true);
    redraw();
}

int SpaceView::handle(int event) {
    switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
        redraw();
        return 1;
    case FL_ENTER:
    case FL_MOVE: {
        int b = block_at(Fl::event_x(), Fl::event_y());
        if (b != hover_) {
            hover_ = b;
            redraw();
        }
        return 1;
    }
    case FL_LEAVE:
        hover_ = -1;
        redraw();
        return 1;
    case FL_PUSH: {
        take_focus();
        pane_->win()->set_active(pane_);
        int b = block_at(Fl::event_x(), Fl::event_y());
        if (b >= 0) {
            focus_block(b);
            if (Fl::event_button() == FL_RIGHT_MOUSE)
                pane_->context_menu(blocks_[size_t(b)].index, Fl::event_x(), Fl::event_y());
            else if (Fl::event_clicks())
                pane_->open_index(blocks_[size_t(b)].index);
        }
        return 1;
    }
    case FL_KEYBOARD: {
        if (Fl::event_state() & (FL_CTRL | FL_ALT | FL_META))
            return 0;
        int k = Fl::event_key();
        switch (k) {
        case FL_Left: focus_block(nearest(focus_, -1, 0)); return 1;
        case FL_Right: focus_block(nearest(focus_, 1, 0)); return 1;
        case FL_Up: focus_block(nearest(focus_, 0, -1)); return 1;
        case FL_Down: focus_block(nearest(focus_, 0, 1)); return 1;
        case FL_Home: focus_block(0); return 1;
        case FL_End: focus_block(int(blocks_.size()) - 1); return 1;
        case FL_Enter:
        case FL_KP_Enter:
            if (focus_ >= 0)
                pane_->open_index(blocks_[size_t(focus_)].index);
            return 1;
        }
        return 0;
    }
    }
    return 0;
}

void SpaceView::draw() {
    const fo::Theme &t = fo::theme();
    fl_push_clip(x(), y(), w(), h());
    fo::paint::fill(x(), y(), w(), h(), t.background());
    const FolderModel &m = pane_->model();
    std::string head = std::string(fo::tr("Space")) + " \xC2\xB7 " + fo::i18n::arg(fo::tr("%1 shown"), si_size(total_));
    fo::text::draw(head, x(), y(), w() - 8, 24, t.bright_foreground(), fo::text::REGULAR, 13);
    for (size_t b = 0; b < blocks_.size(); ++b) {
        const Block &k = blocks_[b];
        if (k.w < 1 || k.h < 1)
            continue;
        const Entry &e = m.entries[size_t(k.index)];
        int bx = map_x_ + k.x, by = map_y_ + k.y;
        fo::Rgb c = block_color(e.name, e.is_dir);
        if (int(b) == hover_)
            c = fo::mix(c, t.bright_foreground(), 0.15);
        fo::paint::fill(bx, by, k.w, k.h, c);
        fo::paint::round_frame(bx, by, k.w, k.h, 0, 1, t.background());
        if (int(b) == focus_) {
            fo::paint::round_frame(bx + 1, by + 1, k.w - 2, k.h - 2, 0, 2, t.bright_foreground());
            fo::paint::round_frame(bx + 3, by + 3, k.w - 6, k.h - 6, 0, 1, t.background());
        }
        if (k.w > 56 && k.h > 36) {
            fl_push_clip(bx + 4, by + 2, k.w - 8, k.h - 4);
            fo::icon::draw(e.is_dir ? "folder" : "image", bx + 8, by + 9, 11, t.background());
            fo::text::draw(display_name(e), bx + 24, by + 4, k.w - 28, 20, t.background(), fo::text::REGULAR, 11);
            fo::text::draw(si_size(k.bytes), bx + 8, by + 24, k.w - 12, 18, t.background(), fo::text::REGULAR, 11);
            fl_pop_clip();
        }
    }
    std::string foot = fo::i18n::arg(fo::tr("Without area: %1"), std::to_string(unsized_)) + " \xC2\xB7 " +
                       std::string(fo::tr("Enter opens a folder \xC2\xB7 Backspace goes up"));
    fo::text::draw(foot, x(), y() + h() - kFooter, w(), kFooter, t.dim(), fo::text::REGULAR, 11);
    // The tooltip of the pointed block, or of a focused one too small for
    // its own label (Files' Space tooltips).
    int tip = hover_ >= 0 ? hover_ : -1;
    if (tip < 0 && focus_ >= 0 && Fl::focus() == this) {
        const Block &k = blocks_[size_t(focus_)];
        if (!(k.w > 56 && k.h > 36))
            tip = focus_;
    }
    if (tip >= 0) {
        const Block &k = blocks_[size_t(tip)];
        const Entry &e = m.entries[size_t(k.index)];
        std::string l1 = display_name(e);
        std::string l2 = kind_word(e) + " \xC2\xB7 " + si_size(k.bytes);
        char share[32];
        std::snprintf(share, sizeof share, "%.1f %%", total_ ? 100.0 * double(k.bytes) / double(total_) : 0.0);
        std::string l3 = fo::i18n::arg(fo::tr("%1 of this folder"), share);
        if (e.is_dir)
            l3 += std::string(" \xC2\xB7 ") + fo::tr("Enter opens it");
        int tw = int(std::max({fo::text::width(l1, fo::text::REGULAR, 11), fo::text::width(l2, fo::text::REGULAR, 11),
                               fo::text::width(l3, fo::text::REGULAR, 11)})) + 24;
        int th = 64;
        int tx = std::min(x() + w() - tw - 4, map_x_ + k.x + k.w / 2);
        int ty = std::min(y() + h() - th - 4, map_y_ + k.y + k.h / 2);
        int r = t.radius() > 0 ? 5 : 0;
        fo::paint::round_fill(tx, ty, tw, th, r, t.card());
        fo::paint::round_fill(tx + 1, ty + 1, tw - 2, th - 2, r, t.background());
        fo::text::draw(l1, tx + 12, ty + 4, tw - 24, 18, t.bright_foreground(), fo::text::REGULAR, 11);
        fo::text::draw(l2, tx + 12, ty + 22, tw - 24, 18, t.foreground(), fo::text::REGULAR, 11);
        fo::text::draw(l3, tx + 12, ty + 40, tw - 24, 18, t.foreground(), fo::text::REGULAR, 11);
    }
    fl_pop_clip();
}

} // namespace flo::ui

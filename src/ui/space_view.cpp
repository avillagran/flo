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
#include "fltk-omarchy/menu.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

namespace flo::ui {

namespace {

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
    std::vector<Row> zero;
    total_ = 0;
    partial_ = false;
    for (int i = 0; i < m.count(); ++i) {
        const Entry &e = m.entries[size_t(i)];
        uint64_t b = 0;
        bool known = true;
        if (e.is_dir) {
            const FolderSizes::Result *r = App::get().sizes().get(m.path(i));
            if (r)
                b = r->bytes;
            known = r != nullptr;
            if (!r || !r->complete)
                partial_ = true;
        } else {
            b = uint64_t(std::max<int64_t>(0, e.size));
        }
        if (b == 0) {
            zero.push_back({i, known ? "0 B \xC2\xB7 " + std::string(fo::tr("no area")) : std::string(fo::tr("Unknown size"))});
            continue;
        }
        total_ += b;
        sizes.push_back({i, b});
    }
    std::stable_sort(sizes.begin(), sizes.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    // First pass with the list as big as it may get, to know which blocks
    // are too small for a label (they get a row too, as in Files).
    auto place = [&](size_t rows) {
        int reserve = rows == 0 ? 26 : 56 + kRow * int(std::min<size_t>(rows, kListRows));
        map_x_ = x(), map_y_ = y() + kToolbar, map_w_ = std::max(0, w() - 4);
        map_h_ = std::max(0, h() - kToolbar - reserve);
        blocks_ = squarify(sizes, map_w_, map_h_);
    };
    auto small_rows = [&] {
        std::vector<Row> out;
        for (const auto &b : blocks_)
            if (b.w < kLabelW || b.h < kLabelH)
                out.push_back({b.index, si_size(b.bytes) + " \xC2\xB7 " + fo::tr("label in list")});
        return out;
    };
    place(zero.size() + sizes.size());
    std::vector<Row> small = small_rows();
    place(zero.size() + small.size());
    small = small_rows();
    rows_ = std::move(small);
    rows_.insert(rows_.end(), zero.begin(), zero.end());
    list_y_ = map_y_ + map_h_;
    row_top_ = std::clamp(row_top_, 0, std::max(0, int(rows_.size()) - kListRows));
    int cur = pane_->current();
    focus_ = -1;
    for (size_t b = 0; b < blocks_.size(); ++b)
        if (blocks_[b].index == cur)
            focus_ = int(b);
    if (row_focus_ >= int(rows_.size()))
        row_focus_ = -1;
    if (row_focus_ >= 0 && rows_[size_t(row_focus_)].index != cur)
        row_focus_ = -1;
    hover_ = -1;
    layout_buttons();
}

bool SpaceView::scanning() const {
    const FolderModel &m = pane_->model();
    if (m.loading)
        return true;
    for (int i = 0; i < m.count(); ++i)
        if (m.entries[size_t(i)].is_dir && App::get().sizes().pending(m.path(i)))
            return true;
    return false;
}

void SpaceView::layout_buttons() {
    // Files' space_map_view.rs: row 1 at y (the hidden toggle on the
    // right), row 2 at y + 34, Marks at y + 64 on the right.
    buttons_.clear();
    const int X = x(), Y = y(), W = w();
    bool hidden = pane_->show_hidden();
    buttons_.push_back({X + W - 186, Y, 184, 28, hidden ? fo::tr("Hide hidden") : fo::tr("Show hidden"),
                        hidden ? "eye-off" : "eye", std::string(fo::tr("Show hidden")) + ", Ctrl+H", kHidden});
    if (pane_->can_go_up())
        buttons_.push_back({X, Y + 34, 82, 28, fo::tr("Back"), "arrow-up", "", kBack});
    buttons_.push_back({X + 88, Y + 34, 112, 28, fo::tr("Recalculate"), "refresh-cw",
                        fo::tr("Refresh the Space calculation"), kRecalc});
    if (scanning())
        buttons_.push_back({X + 206, Y + 34, 36, 28, "", "x", fo::tr("Cancel the Space calculation"), kCancel});
    buttons_.push_back({X + W - 66, Y + 64, 66, 24, fo::tr("Marks"), "", fo::tr("Selection tools"), kMarks});
    if (int(rows_.size()) > kListRows) {
        buttons_.push_back({X + W - 64, list_y_ + 8, 28, 22, "\xE2\x86\x91", "", "", kListUp});
        buttons_.push_back({X + W - 30, list_y_ + 8, 28, 22, "\xE2\x86\x93", "", "", kListDown});
    }
}

int SpaceView::button_at(int ex, int ey) const {
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Btn &b = buttons_[i];
        if (ex >= b.x && ex < b.x + b.w && ey >= b.y && ey < b.y + b.h)
            return int(i);
    }
    return -1;
}

int SpaceView::row_at(int ex, int ey) const {
    int top = list_y_ + 32;
    if (rows_.empty() || ey < top || ex < x() || ex >= x() + w())
        return -1;
    int r = row_top_ + (ey - top) / kRow;
    int last = std::min(int(rows_.size()), row_top_ + kListRows);
    return r < last ? r : -1;
}

void SpaceView::press(int id) {
    switch (id) {
    case kHidden: pane_->toggle_hidden(); break;
    case kBack: pane_->go_up(); break;
    case kRecalc: pane_->measure_listed(); break;
    case kCancel:
        App::get().sizes().cancel();
        layout();
        break;
    case kMarks: {
        std::vector<fo::MenuItem> m = {
            fo::MenuItem(fo::tr("Select all"), "Ctrl+A", [this] { pane_->select_all(); }).with_icon("square-check"),
            fo::MenuItem(fo::tr("Invert selection"), "Ctrl+I", [this] { pane_->invert_selection(); }),
            fo::MenuItem(fo::tr("Clear selection"), "Ctrl+Shift+D", [this] { pane_->clear_selection(); })
                .with_icon("x"),
        };
        const Btn *b = nullptr;
        for (const auto &k : buttons_)
            if (k.id == kMarks)
                b = &k;
        if (b)
            fo::PopupMenu::open_below(pane_->win(), b->x, b->y, b->w, b->h, m, true);
        break;
    }
    case kListUp: row_top_ = std::max(0, row_top_ - kListRows); break;
    case kListDown: row_top_ = std::min(std::max(0, int(rows_.size()) - kListRows), row_top_ + kListRows); break;
    }
    redraw();
}

void SpaceView::focus_row(int r) {
    if (r < 0 || r >= int(rows_.size()))
        return;
    row_focus_ = r;
    focus_ = -1;
    if (r < row_top_)
        row_top_ = r;
    if (r >= row_top_ + kListRows)
        row_top_ = r - kListRows + 1;
    pane_->files()->set_current(rows_[size_t(r)].index, true);
    redraw();
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
    row_focus_ = -1;
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
        int k = button_at(Fl::event_x(), Fl::event_y());
        if (b != hover_ || k != hover_button_) {
            hover_ = b;
            hover_button_ = k;
            redraw();
        }
        return 1;
    }
    case FL_LEAVE:
        hover_ = -1;
        hover_button_ = -1;
        redraw();
        return 1;
    case FL_MOUSEWHEEL:
        if (rows_.size() > size_t(kListRows) && Fl::event_y() >= list_y_) {
            row_top_ = std::clamp(row_top_ + (Fl::event_dy() > 0 ? 1 : -1), 0, int(rows_.size()) - kListRows);
            redraw();
            return 1;
        }
        return 0;
    case FL_PUSH: {
        take_focus();
        pane_->win()->set_active(pane_);
        int k = button_at(Fl::event_x(), Fl::event_y());
        if (k >= 0 && Fl::event_button() == FL_LEFT_MOUSE) {
            int id = buttons_[size_t(k)].id;
            // The pane may relist (and this view relayout) under the press.
            Fl::add_timeout(0, [](void *d) {
                auto *pr = static_cast<std::pair<SpaceView *, int> *>(d);
                pr->first->press(pr->second);
                delete pr;
            }, new std::pair<SpaceView *, int>(this, id));
            return 1;
        }
        int r = row_at(Fl::event_x(), Fl::event_y());
        if (r >= 0) {
            focus_row(r);
            int index = rows_[size_t(r)].index;
            if (Fl::event_button() == FL_RIGHT_MOUSE)
                pane_->context_menu(index, Fl::event_x(), Fl::event_y());
            else if (Fl::event_clicks())
                pane_->open_index(index);
            return 1;
        }
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
        if (row_focus_ >= 0) {
            // In the list: Up and Down move; Up from the first row goes
            // back to the map.
            switch (k) {
            case FL_Up:
                if (row_focus_ == 0) {
                    row_focus_ = -1;
                    focus_block(blocks_.empty() ? -1 : int(blocks_.size()) - 1);
                } else {
                    focus_row(row_focus_ - 1);
                }
                return 1;
            case FL_Down: focus_row(std::min(int(rows_.size()) - 1, row_focus_ + 1)); return 1;
            case FL_Page_Up: focus_row(std::max(0, row_focus_ - kListRows)); return 1;
            case FL_Page_Down: focus_row(std::min(int(rows_.size()) - 1, row_focus_ + kListRows)); return 1;
            case FL_Enter:
            case FL_KP_Enter: pane_->open_index(rows_[size_t(row_focus_)].index); return 1;
            }
            return 0;
        }
        switch (k) {
        case FL_Left: focus_block(nearest(focus_, -1, 0)); return 1;
        case FL_Right: focus_block(nearest(focus_, 1, 0)); return 1;
        case FL_Up: focus_block(nearest(focus_, 0, -1)); return 1;
        case FL_Down: {
            // Down from the map's bottom edge enters the list (Files).
            int n = nearest(focus_, 0, 1);
            if ((n == focus_ || blocks_.empty()) && !rows_.empty())
                focus_row(0);
            else
                focus_block(n);
            return 1;
        }
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
    const int r6 = t.radius() > 0 ? 6 : 0;
    // Files' Space toolbar.
    bool busy = scanning();
    std::string head = std::string(fo::tr("Space")) + " \xC2\xB7 " +
                       (m.loading ? std::string(fo::tr("calculating\xE2\x80\xA6"))
                                  : fo::i18n::arg(fo::tr("%1 visible"), si_size(total_)) +
                                        (partial_ ? std::string(" \xC2\xB7 ") + fo::tr("partial") : std::string()));
    fo::text::draw(head, x(), y() + 2, w() - 194, 28, t.foreground(), fo::text::REGULAR, 13);
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Btn &b = buttons_[i];
        bool hot = int(i) == hover_button_;
        fo::paint::round_fill(b.x, b.y, b.w, b.h, b.id >= kListUp ? (r6 ? 4 : 0) : r6,
                              hot ? fo::mix(t.card(), t.selection(), 0.5) : t.card());
        int tx = b.x + 8;
        if (!b.icon.empty()) {
            fo::icon::draw(b.icon, b.label.empty() ? b.x + (b.w - 13) / 2 : tx, b.y + (b.h - 13) / 2, 13, t.accent());
            tx += 20;
        }
        if (!b.label.empty())
            fo::text::draw(b.label, b.id >= kListUp ? b.x : tx, b.y, b.id >= kListUp ? b.w : b.x + b.w - tx - 4, b.h,
                           b.id >= kListUp ? t.foreground() : t.accent(), fo::text::REGULAR, 12,
                           b.id >= kListUp ? FL_ALIGN_CENTER : FL_ALIGN_LEFT);
    }
    if (!m.loading) {
        std::string tot = fo::i18n::arg(fo::tr("Apparent total %1"), (partial_ ? "\xE2\x89\xA5" : "") + si_size(total_));
        if (pane_->show_hidden())
            tot += std::string(" \xC2\xB7 ") + fo::tr("incl. hidden");
        int tx = x() + (busy ? 248 : 206);
        fo::text::draw(tot, tx, y() + 34, std::max(0, x() + w() - tx), 28, t.dim(), fo::text::REGULAR, 10);
    }
    fo::text::draw(fo::tr("Observation \xC2\xB7 F5 or Recalculate refreshes"), x(), y() + 66, w() - 74, 18, t.dim(),
                   fo::text::REGULAR, 10);
    fo::text::draw(fo::tr("Subfolders not watched \xC2\xB7 not a snapshot"), x(), y() + 88, w() - 8, 18, t.dim(),
                   fo::text::REGULAR, 10);
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
        if (pane_->files()->selected(k.index) && k.w > 6 && k.h > 6)
            fo::paint::round_frame(bx + 2, by + 2, k.w - 4, k.h - 4, 0, 2, t.selection());
        if (k.w >= kLabelW && k.h >= kLabelH) {
            fl_push_clip(bx + 4, by + 2, k.w - 8, k.h - 4);
            fo::icon::draw(e.is_dir ? "folder" : "image", bx + 8, by + 9, 11, t.background());
            fo::text::draw(display_name(e), bx + 24, by + 4, k.w - 28, 20, t.background(), fo::text::REGULAR, 11);
            fo::text::draw(si_size(k.bytes), bx + 8, by + 24, k.w - 12, 18, t.background(), fo::text::REGULAR, 11);
            fl_pop_clip();
        }
    }
    if (blocks_.empty() && !m.loading)
        fo::text::draw(m.count() == 0 ? std::string(fo::tr("Empty folder")) + " \xC2\xB7 0 B"
                                      : std::string(fo::tr("No complete positive sizes to show")),
                       x(), map_y_, w(), 50, t.dim(), fo::text::REGULAR, 12, FL_ALIGN_CENTER);
    // Files' "No area/label" list under the map.
    if (!rows_.empty()) {
        std::string lh = fo::i18n::arg(fo::tr("No area/label \xC2\xB7 %1 \xC2\xB7 wheel / \xE2\x86\x91\xE2\x86\x93"),
                                       std::to_string(rows_.size()));
        fo::text::draw(lh, x(), list_y_ + 8, w() - 70, 22, t.dim(), fo::text::REGULAR, 10);
        int top = list_y_ + 32;
        fl_push_clip(x(), top, w(), kRow * kListRows);
        for (int r = row_top_; r < std::min(int(rows_.size()), row_top_ + kListRows); ++r) {
            const Row &row = rows_[size_t(r)];
            int ry = top + (r - row_top_) * kRow;
            if (pane_->files()->selected(row.index))
                fo::paint::round_fill(x(), ry, w() - 4, kRow - 2, r6 ? 4 : 0, t.selection());
            if (r == row_focus_) {
                fo::paint::fill(x() + 1, ry + 4, 2, kRow - 10, t.accent());
                if (Fl::focus() == this)
                    fo::paint::round_frame(x(), ry, w() - 4, kRow - 2, r6 ? 4 : 0, 1, t.accent());
            }
            const Entry &e = m.entries[size_t(row.index)];
            fo::text::draw(display_name(e) + " \xC2\xB7 " + row.state, x() + 6, ry, w() - 16, kRow - 2, t.foreground(),
                           fo::text::REGULAR, 11, FL_ALIGN_LEFT, fo::text::Elide::Middle);
        }
        fl_pop_clip();
    }
    fo::text::draw(fo::tr("Folders include hidden descendants"), x(), y() + h() - 22, w(), 22, t.dim(),
                   fo::text::REGULAR, 10);
    // The tooltip of the pointed block, or of a focused one too small for
    // its own label (Files' Space tooltips).
    int tip = hover_ >= 0 ? hover_ : -1;
    if (tip < 0 && focus_ >= 0 && Fl::focus() == this) {
        const Block &k = blocks_[size_t(focus_)];
        if (!(k.w >= kLabelW && k.h >= kLabelH))
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

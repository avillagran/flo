#include "ui/space_view.h"

#include "core/fsutil.h"
#include "core/tree_size.h"
#include "ui/app.h"
#include "ui/file_view.h"
#include "ui/main_window.h"
#include "ui/pane.h"
#include "ui/sizes.h"
#include "ui/tile_colors.h"

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

tile_colors::Rgb8 rgb8(fo::Rgb c) { return {c.r, c.g, c.b}; }

// Files' rectangle rounding: the edges, not the size, so neighbours meet.
void round_rect(const fo::RectF &r, int &x, int &y, int &w, int &h) {
    x = int(std::lround(r.x));
    y = int(std::lround(r.y));
    w = int(std::lround(r.x + r.w)) - x;
    h = int(std::lround(r.y + r.h)) - y;
}

fo::RectF rect_f(int x, int y, int w, int h) { return fo::RectF{double(x), double(y), double(w), double(h)}; }

// A quarter-size rectangle at the centre: the zoom anchor when the
// folder's block is not known.
fo::RectF centre(const fo::RectF &full) {
    int fx = int(full.x), fy = int(full.y), fw = int(full.w), fh = int(full.h);
    return rect_f(fx + fw * 3 / 8, fy + fh * 3 / 8, std::max(1, fw / 4), std::max(1, fh / 4));
}

// Files' Grid glyph of a kind (grid_view.rs type_glyph).
const char *tile_glyph(bool dir, int kind) {
    if (dir)
        return "folder";
    switch (Kind(kind)) {
    case Kind::Image: return "image";
    case Kind::Video: return "video";
    case Kind::Audio: return "music";
    case Kind::Pdf: return fo::glyph::available() ? "pdf" : "file-text";
    default: return "file";
    }
}

// Files' corner unit: 1 with rounded corners, 0 square.
int corner() { return fo::theme().radius() > 0 ? 1 : 0; }

// Files' labelled-tile rows and type icon (space_map_view.rs).
constexpr int kNameBottom = 31, kSizeTop = 32, kIconMin = 18, kIconMax = 64;

bool icon_layout(int x, int y, int w, int h, int &ix, int &iy, int &iw, int &ih, int &size) {
    if (w < SpaceView::kLabelW || h < SpaceView::kLabelH)
        return false;
    int top = y + kNameBottom;
    int bottom = std::max(y + h - kSizeTop, y + 30);
    ih = bottom - top - 4;
    iw = w - 20;
    size = int(std::lround(std::min(ih, iw) * 0.8));
    if (size < kIconMin)
        return false;
    size = std::min(size, kIconMax);
    ix = x + 10;
    iy = top + 2;
    return true;
}
} // namespace

fo::Rgb SpaceView::tile_fill(const std::string &name) {
    const fo::Theme &t = fo::theme();
    const fo::Rgb colors[] = {t.blue(), t.cyan(), t.magenta(), t.green(), t.yellow()};
    return colors[tile_colors::index(name)];
}

fo::Rgb SpaceView::readable(fo::Rgb fill) {
    const fo::Theme &t = fo::theme();
    tile_colors::Rgb8 c = tile_colors::readable(rgb8(fill), rgb8(t.color("darker_background")), rgb8(t.bright_foreground()));
    return fo::Rgb(c.r, c.g, c.b);
}

SpaceView::SpaceView(int x, int y, int w, int h, Pane *pane) : Fl_Widget(x, y, w, h), pane_(pane) {
    box(FL_NO_BOX);
    set_visible_focus();
}

std::vector<SpaceView::Block> SpaceView::treemap(const std::vector<std::pair<int, uint64_t>> &sizes, int W, int H) {
    // Files' weighted binary treemap (files_core space_map.rs layout): split
    // the run near half its bytes along the longer side, the smaller part
    // at the low edge; edges are rounded so neighbours meet.
    std::vector<Block> out;
    if (sizes.empty() || W <= 0 || H <= 0)
        return out;
    std::vector<std::pair<int, uint64_t>> items = sizes;
    std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::vector<uint64_t> prefix(1, 0);
    for (const auto &it : items)
        prefix.push_back(prefix.back() + it.second);
    if (prefix.back() == 0)
        return out;
    struct Work {
        size_t start, end;
        double x0, y0, x1, y1;
    };
    std::vector<Work> stack{{0, items.size(), 0, 0, double(W), double(H)}};
    while (!stack.empty()) {
        Work k = stack.back();
        stack.pop_back();
        if (k.end - k.start == 1) {
            Block b;
            b.index = items[k.start].first;
            b.bytes = items[k.start].second;
            b.x = int(std::lround(k.x0)), b.y = int(std::lround(k.y0));
            b.w = int(std::lround(k.x1)) - b.x, b.h = int(std::lround(k.y1)) - b.y;
            out.push_back(b);
            continue;
        }
        const uint64_t sum = prefix[k.end] - prefix[k.start];
        auto weight = [&](size_t end) { return prefix[end] - prefix[k.start]; };
        auto balance = [&](size_t end) {
            uint64_t left = weight(end), right = sum - left;
            return left > right ? left - right : right - left;
        };
        size_t low = k.start + 1, high = k.end - 1;
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (weight(middle) < sum / 2)
                low = middle + 1;
            else
                high = middle;
        }
        size_t split = low > k.start + 1 && balance(low - 1) <= balance(low) ? low - 1 : low;
        const uint64_t left_w = weight(split), right_w = sum - left_w;
        const bool left_small = left_w <= right_w;
        const uint64_t smaller = std::min(left_w, right_w);
        const bool horizontal = k.x1 - k.x0 >= k.y1 - k.y0;
        const double a0 = horizontal ? k.x0 : k.y0, a1 = horizontal ? k.x1 : k.y1;
        const double cut = a0 + (a1 - a0) * (double(smaller) / double(sum));
        Work left = k, right = k;
        left.end = split;
        right.start = split;
        if (horizontal) {
            if (left_small)
                left.x1 = cut, right.x0 = cut;
            else
                right.x1 = cut, left.x0 = cut;
        } else if (left_small) {
            left.y1 = cut, right.y0 = cut;
        } else {
            right.y1 = cut, left.y0 = cut;
        }
        stack.push_back(right);
        stack.push_back(left);
    }
    std::stable_sort(out.begin(), out.end(), [](const Block &a, const Block &b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.index < b.index;
    });
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
        map_x_ = x(), map_y_ = y() + kToolbar, map_w_ = std::max(0, w());
        map_h_ = std::max(0, h() - kToolbar - reserve);
        blocks_ = treemap(sizes, map_w_, map_h_);
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
    // Files lists what has no area first, then the blocks too small for a label.
    rows_ = std::move(zero);
    rows_.insert(rows_.end(), small.begin(), small.end());
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
    layout_path_ = m.base;
    ++layout_gen_;
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
    case FL_HIDE:
        // Another view takes over: a later return is not a navigation.
        leave();
        return Fl_Widget::handle(event);
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

// --- Motion (space_map_zoom.rs) ------------------------------------------------------

SpaceView::Affine SpaceView::Affine::between(const fo::RectF &from, const fo::RectF &to) {
    Affine a;
    a.sx = std::fabs(from.w) > 1e-9 ? to.w / from.w : 1;
    a.sy = std::fabs(from.h) > 1e-9 ? to.h / from.h : 1;
    a.tx = to.x - from.x * a.sx;
    a.ty = to.y - from.y * a.sy;
    return a;
}

fo::RectF SpaceView::Affine::apply(const fo::RectF &r) const {
    return fo::RectF{r.x * sx + tx, r.y * sy + ty, r.w * sx, r.h * sy};
}

fo::RectF SpaceView::Plan::place(const fo::RectF &r) const {
    fo::RectF f = live.apply(r);
    if (reveal >= 1)
        return f;
    double cx = f.x + f.w / 2, cy = f.y + f.h / 2;
    return fo::RectF{cx - f.w * reveal / 2, cy - f.h * reveal / 2, f.w * reveal, f.h * reveal};
}

const SpaceView::Tile *SpaceView::Snapshot::find(const std::string &name) const {
    for (const auto &t : tiles)
        if (t.name == name)
            return &t;
    return nullptr;
}

SpaceView::Tile SpaceView::tile_of(const Block &b) const {
    const FolderModel &m = pane_->model();
    const Entry &e = m.entries[size_t(b.index)];
    Tile t;
    t.x = b.x, t.y = b.y, t.w = b.w, t.h = b.h;
    t.name = display_name(e);
    t.dir = e.is_dir;
    t.kind = int(e.kind);
    t.bytes = b.bytes;
    if (e.is_dir)
        if (const FolderSizes::Result *r = App::get().sizes().get(m.path(b.index)))
            t.files = r->files;
    return t;
}

void SpaceView::remember(const std::string &path) {
    fo::RectF map = rect_f(map_x_, map_y_, map_w_, map_h_);
    if (!cache_.empty() && cache_.front().path == path && cache_.front().gen == layout_gen_ &&
        cache_.front().map == map)
        return;
    cache_.erase(std::remove_if(cache_.begin(), cache_.end(), [&](const Snapshot &s) { return s.path == path; }),
                 cache_.end());
    Snapshot s;
    s.path = path;
    s.gen = layout_gen_;
    s.map = map;
    // The layout lists larger blocks first: the biggest are kept.
    for (size_t b = 0; b < blocks_.size() && s.tiles.size() < kSnapshotTiles; ++b)
        if (blocks_[b].w > 0 && blocks_[b].h > 0)
            s.tiles.push_back(tile_of(blocks_[b]));
    cache_.push_front(std::move(s));
    while (cache_.size() > kSnapshots)
        cache_.pop_back();
}

const SpaceView::Snapshot *SpaceView::snapshot(const std::string &path) const {
    for (const auto &s : cache_)
        if (s.path == path)
            return &s;
    return nullptr;
}

void SpaceView::leave() {
    shown_.clear();
    zoom_.active = false;
    reveal_pending_ = false;
    animating_ = false;
    ring_valid_ = false;
}

void SpaceView::start(const std::string &previous, const std::string &path, bool ready) {
    bool in;
    std::string child;
    if (path_dirname(path) == previous && path != previous)
        in = true, child = path;
    else if (path_dirname(previous) == path && path != previous)
        in = false, child = previous;
    else
        return;
    const std::string name = path_basename(child);
    if (name.empty())
        return;
    const Snapshot *parent = snapshot(path_dirname(child));
    const Snapshot *from = in ? parent : snapshot(child);
    if (!in && !from && !parent)
        return; // nothing painted on either side: nothing to zoom
    Zoom &z = zoom_;
    z = Zoom();
    z.in = in;
    if (from)
        z.has_from = true, z.from = *from;
    if (!in && parent)
        z.has_parent = true, z.parent = *parent;
    const Tile *t = parent ? parent->find(name) : nullptr;
    if (t) {
        z.has_block = true;
        z.block = rect_f(int(parent->map.x) + t->x, int(parent->map.y) + t->y, t->w, t->h);
        z.tile = *t;
    } else {
        z.tile.name = name;
        z.tile.dir = true;
    }
    came_in_ = in;
    z.active = true;
    z.t.jump(0);
    z.t.set(1.0, kZoomMs, fo::anim::Ease::OutCubic);
    if (!ready) {
        reveal_path_ = path;
        reveal_pending_ = true;
        reveal_ = fo::Anim<double>();
    }
}

bool SpaceView::plan(const std::string &path, const fo::RectF &full, bool ready, Plan &out) {
    if (shown_ != path) {
        std::string previous = shown_;
        shown_ = path;
        zoom_.active = false;
        reveal_pending_ = false;
        if (!previous.empty() && fo::anim::enabled())
            start(previous, path, ready);
    }
    out = Plan();
    if (reveal_pending_ && reveal_path_ == path && ready) {
        if (!reveal_.initialized()) {
            reveal_.jump(0);
            reveal_.set(1.0, kRevealMs, fo::anim::Ease::OutCubic);
        }
        double v = reveal_.value();
        if (!reveal_.running()) {
            reveal_pending_ = false;
        } else {
            out.reveal = std::clamp(v, 0.0, 1.0);
            animating_ = true;
        }
    }
    if (zoom_.active) {
        double v = zoom_.t.value();
        if (!zoom_.t.running()) {
            zoom_.active = false;
        } else {
            v = std::clamp(v, 0.0, 1.0);
            fo::RectF block = zoom_.has_block ? zoom_.block : centre(full);
            if (zoom_.in) {
                fo::RectF frame = fo::lerp(block, full, v);
                out.live = Affine::between(full, frame);
                out.back = Affine::between(block, frame);
                out.has_back = true;
                out.frame = frame;
            } else {
                fo::RectF frame = fo::lerp(full, block, v);
                Affine around = Affine::between(block, frame);
                out.live = around;
                out.back = around;
                out.has_back = true;
                if (zoom_.has_from) {
                    out.front = Affine::between(zoom_.from.map, frame);
                    out.has_front = true;
                }
                out.frame = frame;
            }
            out.has_frame = true;
            out.moving = true;
            animating_ = true;
        }
    }
    out.outline = !ready && !zoom_.active && reveal_pending_ && reveal_path_ == path && came_in_;
    return out.moving || out.outline || out.reveal < 1;
}

void SpaceView::paint_back(const Plan &plan, bool ready) {
    if (!zoom_.active)
        return;
    const Snapshot *source = zoom_.in ? (zoom_.has_from ? &zoom_.from : nullptr)
                                      : (zoom_.has_parent && !ready ? &zoom_.parent : nullptr);
    if (source && plan.has_back)
        for (const auto &t : source->tiles) {
            if (t.name == zoom_.tile.name)
                continue;
            int x, y, w, h;
            round_rect(plan.back.apply(rect_f(int(source->map.x) + t.x, int(source->map.y) + t.y, t.w, t.h)), x, y,
                       w, h);
            draw_tile(x, y, w, h, t, false, false, false);
        }
    if (plan.has_frame && (!zoom_.in || !ready)) {
        int x, y, w, h;
        round_rect(plan.frame, x, y, w, h);
        draw_tile(x, y, w, h, zoom_.tile, false, false, false);
    }
}

void SpaceView::paint_front(const Plan &plan) {
    if (!zoom_.active || !plan.has_front || !zoom_.has_from)
        return;
    const Snapshot &s = zoom_.from;
    for (const auto &t : s.tiles) {
        int x, y, w, h;
        round_rect(plan.front.apply(rect_f(int(s.map.x) + t.x, int(s.map.y) + t.y, t.w, t.h)), x, y, w, h);
        draw_tile(x, y, w, h, t, false, false, false);
    }
}

void SpaceView::draw_ring(const fo::RectF *target, int radius) {
    // Files' focus ring glides between blocks and rows of the same folder,
    // and jumps when nothing was focused last frame or the folder changed.
    if (!target) {
        ring_valid_ = false;
        return;
    }
    const std::string key = pane_->folder();
    if (ring_valid_ && ring_key_ == key)
        ring_.set(*target, kRingMs, fo::anim::Ease::OutCubic);
    else
        ring_.jump(*target);
    ring_key_ = key;
    ring_valid_ = true;
    fo::RectF at = ring_.value();
    if (ring_.running())
        animating_ = true;
    int x, y, w, h;
    round_rect(at, x, y, w, h);
    if (w > 4 && h > 4)
        fo::paint::round_frame(x, y, w, h, radius, 2, fo::theme().accent());
}

// --- Painting --------------------------------------------------------------------------

void SpaceView::draw_tile(int x, int y, int w, int h, const Tile &t, bool selected, bool hovered, bool focused) {
    if (w <= 0 || h <= 0)
        return;
    const int rad = corner();
    const fo::Rgb fill = tile_fill(t.name), fg = readable(fill);
    // Marks and hover are the light band; keyboard focus is the accent ring
    // drawn over the outer gap, so focus alone never looks marked.
    if (selected || hovered)
        fo::paint::round_fill(x + 1, y + 1, std::max(0, w - 2), std::max(0, h - 2), 7 * rad, fg);
    const int gap = focused ? 5 : (selected || hovered) ? 3 : 1;
    const int ix = x + gap, iy = y + gap, iw = std::max(0, w - 2 * gap), ih = std::max(0, h - 2 * gap);
    if (iw <= 0 || ih <= 0)
        return;
    fo::paint::round_fill(ix, iy, iw, ih, 6 * rad, fill);
    if (w < kLabelW || h < kLabelH)
        return;
    fl_push_clip(ix, iy, iw, ih);
    const char *glyph = tile_glyph(t.dir, t.kind);
    int gx, gy, gw, gh, gs;
    bool icon = icon_layout(x, y, w, h, gx, gy, gw, gh, gs);
    int tx = x + 10;
    if (!icon) {
        // No room for the icon between the rows: a small glyph leads the name.
        fo::icon::draw(glyph, tx, y + 7 + (24 - 13) / 2, 13, fg);
        tx += 13 + 8;
    }
    fo::text::draw(t.name, tx, y + 7, std::max(0, x + w - 10 - tx), 24, fg, fo::text::REGULAR, 13);
    if (icon)
        fo::icon::draw(glyph, gx + (gw - gs) / 2, gy + (gh - gs) / 2, gs, fg);
    std::string size = si_size(t.bytes);
    if (t.dir && w > 220)
        size += " \xC2\xB7 " + fo::trn("%n files", (long long)t.files);
    fo::text::draw(size, x + 10, std::max(y + h - kSizeTop, y + 30), w - 20, 24, fg, fo::text::REGULAR, 12);
    fl_pop_clip();
}

bool SpaceView::label_fits(const Tile &t) const {
    if (t.w < kLabelW || t.h < kLabelH)
        return false;
    int gx, gy, gw, gh, gs;
    int room = t.w - 20 - (icon_layout(0, 0, t.w, t.h, gx, gy, gw, gh, gs) ? 0 : 26);
    return int(std::ceil(fo::text::width(t.name, fo::text::REGULAR, 13))) <= room;
}

void SpaceView::draw_tip(int tip, int px, int py, bool pointer) {
    // Files' tooltip (paint_tip): next to the pointer on hover, under (or
    // over) the block for keyboard focus, kept inside the view.
    const fo::Theme &t = fo::theme();
    const FolderModel &m = pane_->model();
    const Block &k = blocks_[size_t(tip)];
    const Entry &e = m.entries[size_t(k.index)];
    std::vector<std::string> lines;
    lines.push_back(display_name(e));
    std::string detail = kind_word(e) + " \xC2\xB7 " + si_size(k.bytes);
    if (e.is_dir)
        if (const FolderSizes::Result *r = App::get().sizes().get(m.path(k.index)))
            detail = fo::i18n::arg(fo::i18n::arg(fo::i18n::arg(fo::tr("%1 \xC2\xB7 %2 files, %3 folders"), detail),
                                                 std::to_string(r->files)),
                                   std::to_string(r->folders));
    lines.push_back(detail);
    if (total_ > 0) {
        double share = 100.0 * double(k.bytes) / double(total_);
        char buf[32];
        if (share < 0.1)
            std::snprintf(buf, sizeof buf, "<0.1 %%");
        else
            std::snprintf(buf, sizeof buf, "%.1f %%", share);
        std::string l3 = fo::i18n::arg(fo::tr("%1 of this folder"), buf);
        if (e.is_dir)
            l3 += std::string(" \xC2\xB7 ") + fo::tr("Enter opens it");
        lines.push_back(l3);
    }
    const int bx = x(), by = y(), bw = w(), bh = h();
    const int max = std::clamp(bw - 16, 120, 420);
    int width = 0;
    for (const auto &l : lines)
        width = std::max(width, int(std::ceil(fo::text::width(l, fo::text::REGULAR, 12))));
    width = std::min(width + 24, max);
    const int height = int(lines.size()) * 20 + 12;
    const int tx0 = map_x_ + k.x, ty0 = map_y_ + k.y;
    bool inside = pointer && px >= tx0 && px < tx0 + k.w && py >= ty0 && py < ty0 + k.h;
    int ax, ay;
    bool below;
    if (inside) {
        ax = px + 14, ay = py + 20;
        below = ay + height <= by + bh;
    } else {
        ax = tx0 + k.w / 2 - width / 2, ay = ty0 + k.h + 6;
        below = ay + height <= by + bh;
    }
    int ty = below ? ay : inside ? ay - 40 - height : ty0 - 6 - height;
    int tx = std::clamp(ax, bx + 8, std::max(bx + 8, bx + bw - width - 8));
    ty = std::clamp(ty, by + 8, std::max(by + 8, by + bh - height - 8));
    const int r = 5 * corner();
    fo::paint::round_fill(tx, ty, width, height, r, t.lighter_background());
    fo::paint::round_fill(tx + 1, ty + 1, width - 2, height - 2, r, t.color("darker_background"));
    for (size_t i = 0; i < lines.size(); ++i)
        fo::text::draw(lines[i], tx + 12, ty + 6 + int(i) * 20, width - 24, 20, i == 0 ? t.foreground() : t.dim(),
                       fo::text::REGULAR, 12, FL_ALIGN_LEFT, fo::text::Elide::Middle);
}

void SpaceView::draw() {
    const fo::Theme &t = fo::theme();
    animating_ = false;
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
        tot += std::string(" \xC2\xB7 ") + fo::tr("incl. hidden");
        int tx = x() + (busy ? 248 : 206);
        fo::text::draw(tot, tx, y() + 34, std::max(0, x() + w() - tx), 28, t.dim(), fo::text::REGULAR, 10);
    }
    fo::text::draw(fo::tr("Observation \xC2\xB7 F5 or Recalculate refreshes"), x(), y() + 66, w() - 74, 18, t.dim(),
                   fo::text::REGULAR, 10);
    fo::text::draw(fo::tr("Subfolders not watched \xC2\xB7 not a snapshot"), x(), y() + 88, w() - 8, 18, t.dim(),
                   fo::text::REGULAR, 10);

    // The map. Its layout belongs to the folder shown once the listing is
    // in and something has a size (or nothing is left to measure).
    const std::string path = pane_->folder();
    const bool ready = !m.loading && layout_path_ == path && (!blocks_.empty() || !busy);
    const int rad = corner();
    bool held = false;
    if (m.loading && !shown_.empty() && shown_ != path && fo::anim::enabled()) {
        // A listing usually lands within a frame or two: the old map holds
        // still meanwhile, so the zoom starts from a settled frame.
        const double now = fo::anim::now_ms();
        if (hold_path_ != path)
            hold_path_ = path, hold_since_ = now;
        if (now - hold_since_ < kHoldMs) {
            held = true;
            animating_ = true;
            if (const Snapshot *s = snapshot(shown_)) {
                fl_push_clip(int(s->map.x), int(s->map.y), int(s->map.w), int(s->map.h));
                for (const auto &k : s->tiles)
                    draw_tile(int(s->map.x) + k.x, int(s->map.y) + k.y, k.w, k.h, k, false, false, false);
                fl_pop_clip();
            }
        }
    }
    if (held) {
        // Nothing else of the map this frame.
    } else if (!ready) {
        // The sizes of a freshly opened folder come later: the zoom runs
        // on the retained layouts and never waits for them.
        fo::RectF full = rect_f(x(), y() + kToolbar, std::max(1, w()), std::max(24, h() - kToolbar - 26));
        Plan plan;
        bool moving = false;
        if (this->plan(path, full, false, plan)) {
            if (plan.moving) {
                fl_push_clip(int(full.x), int(full.y), int(full.w), int(full.h));
                paint_back(plan, false);
                paint_front(plan);
                fl_pop_clip();
                moving = true;
            } else if (plan.outline) {
                int ox, oy, ow, oh;
                round_rect(full, ox, oy, ow, oh);
                fo::paint::round_frame(ox, oy, ow, oh, 6 * rad, 2, tile_fill(path_basename(path)));
            }
        }
        if (!moving)
            fo::text::draw(fo::tr("Calculating sizes\xE2\x80\xA6"), x(), y() + kToolbar, w(), 50, t.dim(),
                           fo::text::REGULAR, 12, FL_ALIGN_CENTER);
        draw_ring(nullptr, 0);
    } else {
        remember(path);
        fo::RectF map = rect_f(map_x_, map_y_, map_w_, map_h_);
        Plan plan;
        bool planned = this->plan(path, map, true, plan);
        fl_push_clip(map_x_, map_y_, map_w_, map_h_);
        if (planned)
            paint_back(plan, true);
        fo::RectF ring;
        bool has_ring = false;
        for (size_t b = 0; b < blocks_.size(); ++b) {
            const Block &k = blocks_[b];
            if (k.w < 1 || k.h < 1)
                continue;
            fo::RectF r = rect_f(map_x_ + k.x, map_y_ + k.y, k.w, k.h);
            // Hits and keys use the final rectangle; only painting follows the zoom.
            fo::RectF painted = planned ? plan.place(r) : r;
            int px, py, pw, ph;
            round_rect(painted, px, py, pw, ph);
            bool focused = int(b) == focus_;
            draw_tile(px, py, pw, ph, tile_of(k), pane_->files()->selected(k.index), int(b) == hover_, focused);
            if (focused) {
                ring = rect_f(px + 1, py + 1, std::max(0, pw - 2), std::max(0, ph - 2));
                has_ring = true;
            }
        }
        if (planned)
            paint_front(plan);
        if (has_ring && planned && plan.transforms()) {
            // Mid-zoom the ring rides its block instead of gliding behind it.
            int fx, fy, fw, fh;
            round_rect(ring, fx, fy, fw, fh);
            fo::paint::round_frame(fx, fy, fw, fh, 7 * rad, 2, t.accent());
            draw_ring(nullptr, 0);
        } else if (has_ring) {
            draw_ring(&ring, 7 * rad);
        }
        fl_pop_clip();
        if (blocks_.empty())
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
                    fo::paint::fill(x(), ry, w() - 4, kRow - 2, t.selection());
                if (r == row_focus_) {
                    fo::paint::fill(x() + 1, ry + 4, 2, kRow - 10, t.accent());
                    // A tiny block is a block and a row: one ring, on the block.
                    if (!has_ring) {
                        fo::RectF rr = rect_f(x(), ry, w() - 4, kRow - 2);
                        draw_ring(&rr, 4 * rad);
                    }
                }
                const Entry &e = m.entries[size_t(row.index)];
                fo::text::draw(display_name(e) + " \xC2\xB7 " + row.state, x() + 6, ry, w() - 16, kRow - 2,
                               t.foreground(), fo::text::REGULAR, 11, FL_ALIGN_LEFT, fo::text::Elide::Middle);
            }
            fl_pop_clip();
        }
        if (!has_ring && row_focus_ < 0)
            draw_ring(nullptr, 0);
        fo::text::draw(fo::tr("Folders include hidden descendants"), x(), y() + h() - 22, w(), 22, t.dim(),
                       fo::text::REGULAR, 10);
        // Pointing explains any block; keyboard focus only one too small
        // for its own name and size, and never while a zoom moves the tiles.
        bool zooming = planned && plan.transforms();
        int tip = hover_;
        if (tip < 0 && focus_ >= 0 && Fl::focus() == this && !zooming && !label_fits(tile_of(blocks_[size_t(focus_)])))
            tip = focus_;
        if (tip >= 0 && tip < int(blocks_.size()))
            draw_tip(tip, Fl::event_x(), Fl::event_y(), tip == hover_);
    }
    fl_pop_clip();
    // Only a running motion asks for another frame; an idle map asks nothing.
    if (animating_)
        fo::anim::request_frame(this);
}

} // namespace flo::ui

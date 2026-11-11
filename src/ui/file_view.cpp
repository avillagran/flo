#include "ui/file_view.h"

#include "ui/app.h"
#include "ui/sizes.h"
#include "ui/thumbs.h"

#include "core/fsutil.h"
#include "core/tree_size.h"

#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/shell.h"
#include "fltk-omarchy/text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>

namespace flo::ui {

namespace {

int rounded(int px) { return fo::theme().radius() > 0 ? px : 0; }

// Files' column layout of a List row (list_columns.rs, Single profile).
struct Cells {
    int name_x, name_w, size_x = 0, size_w = 0, date_x = 0, date_w = 0, kind_x = 0, kind_w = 0;
};
Cells list_cells(int width, bool dual) {
    const int start = dual ? 30 : 48, pad = dual ? 6 : 10, min_name = dual ? 128 : 160;
    const int size_w = dual ? 74 : 104, date_w = dual ? 0 : 86, kind_w = dual ? 0 : 92;
    int avail = std::max(0, width - start - pad);
    bool size = avail >= min_name + 8 + size_w;
    bool date = date_w > 0 && avail >= min_name + 16 + size_w + date_w;
    bool kind = date && kind_w > 0 && avail >= min_name + 24 + size_w + date_w + kind_w;
    Cells c;
    c.name_x = start;
    c.name_w = avail - (size ? 8 + size_w : 0) - (date ? 8 + date_w : 0) - (kind ? 8 + kind_w : 0);
    if (size)
        c.size_x = start + c.name_w + 8, c.size_w = size_w;
    if (date)
        c.date_x = start + c.name_w + 16 + size_w, c.date_w = date_w;
    if (kind)
        c.kind_x = start + c.name_w + 24 + size_w + date_w, c.kind_w = kind_w;
    return c;
}

} // namespace

void draw_folder_card(int x, int y, int w, int h, fo::Rgb fill, fo::Rgb edge, int edge_px) {
    // Files draws folders as the Nerd folder glyph in blue.
    (void)fill, (void)edge, (void)edge_px;
    int s = std::min(w, h) * 2 / 3;
    fo::icon::draw("folder", x + (w - s) / 2, y + (h - s) / 2, s, fo::theme().blue());
}

AgeSection age_section(int64_t mtime_ns, int64_t now_s) {
    if (mtime_ns <= 0)
        return AgeSection::Undated;
    std::time_t t = std::time_t(now_s);
    std::tm lt{};
    localtime_r(&t, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    int64_t midnight = int64_t(std::mktime(&lt));
    int64_t s = mtime_ns / 1000000000;
    if (s >= midnight)
        return AgeSection::Today;
    if (s >= now_s - 7 * 86400)
        return AgeSection::ThisWeek;
    return AgeSection::Earlier;
}

std::string relative_when(int64_t mtime_ns) {
    if (mtime_ns <= 0)
        return "\xE2\x80\x94";
    int64_t now = int64_t(std::time(nullptr));
    int64_t s = std::max<int64_t>(0, now - mtime_ns / 1000000000);
    if (s < 60)
        return fo::tr("now");
    if (s < 3600)
        return fo::i18n::arg(fo::tr("%1 min ago"), std::to_string(s / 60));
    if (s < 86400)
        return fo::i18n::arg(fo::tr("%1 h ago"), std::to_string(s / 3600));
    if (s < 172800)
        return fo::tr("yesterday");
    return fo::i18n::arg(fo::tr("%1 d ago"), std::to_string(s / 86400));
}

std::string kind_word(const Entry &e) {
    if (e.is_dir)
        return fo::tr("Folder");
    std::string mime = e.mime ? e.mime : "";
    std::string major = mime.substr(0, mime.find('/'));
    std::string minor = mime.find('/') == std::string::npos ? "" : mime.substr(mime.find('/') + 1);
    auto has = [&](std::initializer_list<const char *> keys) {
        for (const char *k : keys)
            if (minor.find(k) != std::string::npos)
                return true;
        return false;
    };
    if (major == "image") return fo::tr("Image");
    if (major == "video") return fo::tr("Video");
    if (major == "audio") return fo::tr("Audio");
    if (major == "font") return fo::tr("Font");
    if (minor == "pdf") return "PDF";
    if (has({"zip", "compressed", "tar", "gzip", "bzip", "xz", "zstd", "rar", "archive"})) return fo::tr("Archive");
    if (has({"shellscript", "python", "rust", "javascript", "x-c", "java", "asm"})) return fo::tr("Code");
    if (major == "text") return fo::tr("Text");
    if (has({"json", "toml", "yaml", "xml"})) return fo::tr("Data");
    if (has({"executable"})) return fo::tr("Executable");
    if (has({"document", "msword", "opendocument", "spreadsheet", "presentation", "rtf"})) return fo::tr("Document");
    std::string ext = e.extension();
    return ext.empty() ? fo::tr("File") : fo::i18n::arg(fo::tr("%1 file"), ext);
}

fo::Rgb badge_color(const Entry &e) {
    const fo::Theme &t = fo::theme();
    std::string mime = e.mime ? e.mime : "";
    if (mime.compare(0, 6, "image/") == 0)
        return t.magenta();
    if (mime.find("zip") != std::string::npos || mime.find("compressed") != std::string::npos ||
        mime.find("tar") != std::string::npos || mime.find("gzip") != std::string::npos)
        return t.yellow();
    if (mime.find("shell") != std::string::npos || mime.find("python") != std::string::npos ||
        mime.find("rust") != std::string::npos)
        return t.green();
    if (mime.compare(0, 5, "text/") == 0 || mime.find("json") != std::string::npos ||
        mime.find("toml") != std::string::npos)
        return t.cyan();
    return t.blue();
}

std::string folder_summary(const Entry &e, const std::string &path) {
    if (const FolderSizes::Result *r = App::get().sizes().get(path))
        return "\xE2\x88\x91 " + si_size(r->bytes);
    if (App::get().sizes().pending(path))
        return fo::tr("counting\xE2\x80\xA6");
    if (e.item_count >= 0)
        return fo::trn("%n items", e.item_count);
    return fo::tr("Folder");
}

FileView::FileView(int x, int y, int w, int h, FolderModel *model) : fo::Collection(x, y, w, h), model_(model) {
    focus_ring_on_selected(true);
    always_ring(true);
    mode(Mode::Groups);
}

void FileView::mode(Mode m) {
    mode_ = m;
    switch (m) {
    case Mode::Groups:
        always_ring(true);
        layout(Layout::Bands);
        padding(0);
        ring_radius(11);
        break;
    case Mode::Grid:
        always_ring(true);
        layout(Layout::Grid);
        tile_size(int(std::lround(kCellW * zoom_)), int(std::lround(kCellH * zoom_)));
        padding(0);
        ring_radius(8);
        break;
    case Mode::List:
    case Mode::Dual:
        always_ring(true);
        layout(Layout::List);
        row_height(m == Mode::Dual ? 32 : kRowH);
        padding(0);
        ring_radius(9);
        break;
    case Mode::Column:
        layout(Layout::List);
        row_height(32);
        padding(4);
        ring_radius(7);
        always_ring(false); // only the column with the keyboard shows it
        break;
    }
    redraw();
}

void FileView::zoom(double z) {
    if (!(z > 0))
        z = 1.0;
    zoom_ = z;
    if (mode_ == Mode::Grid)
        tile_size(int(std::lround(kCellW * zoom_)), int(std::lround(kCellH * zoom_)));
    redraw();
}

int FileView::handle(int event) {
    if (event == FL_MOUSEWHEEL && mode_ == Mode::Grid && (Fl::event_state() & FL_CTRL) && Fl::event_dy() != 0) {
        if (zoom_cb_)
            zoom_cb_(Fl::event_dy() < 0 ? 1.1 : 1.0 / 1.1);
        return 1;
    }
    return fo::Collection::handle(event);
}

void FileView::groups(const int ends[5]) {
    std::copy(ends, ends + 5, ends_);
    std::vector<Band> b;
    Band cards;
    cards.count = ends_[0];
    cards.min_width = 240;
    cards.item_h = 74;
    cards.gap = 8;
    cards.header_h = 28;
    cards.after = 6;
    b.push_back(cards);
    for (int k = 1; k < 5; ++k) {
        Band rows;
        rows.count = ends_[k] - ends_[k - 1];
        rows.item_h = kRowH;
        rows.header_h = rows.count > 0 ? 26 : 0;
        rows.after = rows.count > 0 ? 10 : 0;
        b.push_back(rows);
    }
    bands(std::move(b));
}

std::string FileView::item_text(int i) const {
    return model_ && i >= 0 && i < model_->count() ? display_name(model_->entries[size_t(i)]) : std::string();
}

void FileView::draw() {
    fo::Collection::draw();
    if (count() == 0 && model_ && !model_->loading && model_->error.empty() && !empty_.empty()) {
        const fo::Theme &t = fo::theme();
        fl_push_clip(x(), y(), w(), h());
        if (mode_ == Mode::Column)
            fo::text::draw(empty_, x() + 12, y() + 8, w() - 24, 32, t.dim(), fo::text::REGULAR, 12);
        else
            fo::text::draw(empty_, x(), y() + 80, w(), 30, t.dim(), fo::text::REGULAR, 15);
        fl_pop_clip();
    }
}

void FileView::draw_band_header(int band, int X, int Y, int W, int H) {
    static const char *labels[] = {"Folders", "Today", "This week", "Earlier", "Undated"};
    int count = band == 0 ? ends_[0] : ends_[band] - ends_[band - 1];
    std::string label = fo::text::upper(fo::tr(labels[band])) + " \xC2\xB7 " + std::to_string(count);
    const fo::Theme &t = fo::theme();
    fo::text::draw(label, X, Y, W, 24, t.dim(), fo::text::REGULAR, 10, FL_ALIGN_LEFT);
    if (band == 0) {
        // Files' quick action beside the section title: a new folder.
        int bx = X + int(fo::text::width(label, fo::text::REGULAR, 10)) + 14;
        new_folder_x_ = bx, new_folder_y_ = Y;
        fo::paint::round_fill(bx, Y, 28, 24, rounded(4), t.selection());
        fo::icon::draw("folder-plus", bx + 7, Y + 5, 14, t.accent());
    }
    (void)H;
}

void FileView::band_header_clicked(int band, int ex, int ey) {
    if (band == 0 && ex >= new_folder_x_ && ex < new_folder_x_ + 28 && ey >= new_folder_y_ &&
        ey < new_folder_y_ + 24 && new_folder_)
        new_folder_();
}

void FileView::draw_item(int i, int x, int y, int w, int h, const ItemState &s) {
    if (!model_ || i < 0 || i >= model_->count())
        return;
    switch (mode_) {
    case Mode::Groups:
        if (i < ends_[0])
            draw_card_item(i, x, y, w, h, s);
        else
            draw_row_item(i, x, y, w, h, s);
        break;
    case Mode::Grid: draw_tile_item(i, x, y, w, h, s); break;
    case Mode::List:
    case Mode::Dual: draw_row_item(i, x, y, w, h, s); break;
    case Mode::Column: draw_column_item(i, x, y, w, h, s); break;
    }
}

// Files' extension badge, or the row's thumbnail.
void FileView::draw_badge(const Entry &e, const std::string &path, int x, int y, int size, fo::Rgb matte) {
    const fo::Theme &t = fo::theme();
    (void)matte;
    if (e.is_dir) {
        fo::icon::draw("folder", x + 2, y + 2, size - 4, t.blue());
        return;
    }
    if (App::get().settings().thumbnails && (e.kind == Kind::Image || e.kind == Kind::Video)) {
        if (Fl_RGB_Image *img = App::get().thumbs().get(path, e, kListPx, size, size, true, this)) {
            img->draw(x + (size - img->w()) / 2, y + (size - img->h()) / 2);
            return;
        }
    }
    int b = size - 4;
    fo::paint::round_fill(x + 2, y + 2, b, b, rounded(6), badge_color(e));
    std::string ext = e.extension();
    if (ext.empty())
        ext = "FILE";
    if (ext.size() > 4)
        ext = ext.substr(0, 4);
    fo::text::draw(ext, x + 2, y + 2, b, b, t.background(), fo::text::REGULAR, 9, FL_ALIGN_CENTER, fo::text::Elide::None);
}

void FileView::draw_card_item(int i, int X, int Y, int W, int H, const ItemState &s) {
    const fo::Theme &t = fo::theme();
    const Entry &e = model_->entries[size_t(i)];
    const std::string path = model_->path(i);
    bool marked = s.selected || s.drop_target;
    fo::Rgb fill = marked ? t.selection() : s.hover > 0 ? fo::mix(t.card(), t.selection(), s.hover * 0.5) : t.card();
    fo::paint::round_fill(X, Y, W, H, rounded(11), fill);
    if (s.current)
        fo::paint::fill(X + 4, Y + 8, 2, std::max(0, H - 16), t.accent());
    fo::icon::draw("folder", X + 12, Y + 22, 25, t.blue());
    fl_push_clip(X + 1, Y + 1, W - 2, H - 2);
    fo::text::draw(display_name(e), X + 54, Y + 10, W - 64, 24, t.bright_foreground(), fo::text::REGULAR, 13);
    fo::text::draw(folder_summary(e, path), X + 54, Y + 34, W - 64, 18, t.dim(), fo::text::REGULAR, 11);
    fo::text::draw(relative_when(e.mtime_ns), X + 54, Y + 51, W - 64, 16, t.dim(), fo::text::REGULAR, 10);
    fl_pop_clip();
}

void FileView::draw_tile_item(int i, int X, int Y, int W, int H, const ItemState &s) {
    const fo::Theme &t = fo::theme();
    const Entry &e = model_->entries[size_t(i)];
    const std::string path = model_->path(i);
    std::string detail = e.is_dir ? folder_summary(e, path) : si_size(uint64_t(std::max<int64_t>(0, e.size)));
    int ax, ay, aw, ah;
    bool thumb = !e.is_dir && App::get().settings().thumbnails && (e.kind == Kind::Image || e.kind == Kind::Video);
    // The icon is drawn here so it scales with the zoom (Files: 36 px x zoom).
    draw_tile(X, Y, W, H, s, "", display_name(e), detail, e.is_dir ? t.blue() : badge_color(e), false, &ax, &ay,
              &aw, &ah);
    if (!thumb) {
        int is = std::max(8, std::min(int(std::lround((e.is_dir ? 48 : 36) * zoom_)), ah - 4));
        fo::icon::draw(e.is_dir ? "folder" : kind_icon(e), ax + (aw - is) / 2, ay + (ah - is) / 2, is,
                       e.is_dir ? t.blue() : badge_color(e));
    }
    if (thumb) {
        if (Fl_RGB_Image *img = App::get().thumbs().get(path, e, kGridPx, aw, ah, false, this)) {
            img->draw(ax + (aw - img->w()) / 2, ay + (ah - img->h()) / 2);
            if (e.kind == Kind::Video)
                fo::icon::draw("play", ax + aw / 2 - 8, ay + ah / 2 - 8, 16, t.bright_foreground());
        } else {
            int is = std::max(8, std::min(int(std::lround(36 * zoom_)), ah - 4));
            fo::icon::draw(kind_icon(e), ax + (aw - is) / 2, ay + (ah - is) / 2, is, badge_color(e));
        }
    } else if (e.is_dir) {
        // Flo's folder peek: the first pictures inside, under the folder glyph.
        auto it = model_->peeks.find(path);
        if (it != model_->peeks.end() && !it->second.empty() && App::get().settings().thumbnails) {
            int n = int(std::min<size_t>(2, it->second.size()));
            int pw = 28, ph = 28, px = ax + aw - n * (pw + 3) - 2, py = ay + ah - ph - 2;
            for (int k = 0; k < n; ++k) {
                const Entry &pe = it->second[size_t(k)];
                if (Fl_RGB_Image *img =
                        App::get().thumbs().get(path_join(path, pe.name), pe, kListPx, pw, ph, true, this))
                    img->draw(px + k * (pw + 3), py);
            }
        }
    }
}

void FileView::draw_row_item(int i, int X, int Y, int W, int H, const ItemState &s) {
    const fo::Theme &t = fo::theme();
    const Entry &e = model_->entries[size_t(i)];
    const std::string path = model_->path(i);
    const bool dual = mode_ == Mode::Dual;
    Cells c = list_cells(W, dual);
    fo::Rgb fill = t.background();
    bool filled = false;
    if (s.selected || s.drop_target) {
        fill = t.selection();
        filled = true;
    } else if (s.hover > 0) {
        fill = fo::mix(t.background(), t.card(), s.hover);
        filled = true;
    }
    if (filled)
        fo::paint::round_fill(X, Y, W, H, rounded(9), fill);
    if (s.current)
        fo::paint::fill(X + 1, Y + 4, 2, std::max(0, H - 8), t.accent());
    if (dual) {
        fo::icon::draw(e.is_dir ? "folder" : kind_icon(e), X + 8, Y + (H - 14) / 2, 14,
                       e.is_dir ? t.blue() : t.dim());
    } else {
        draw_badge(e, path, X + 6, Y + 3, 32, fill);
        if (e.is_link)
            fo::text::draw("\xE2\x86\x92", X + 26, Y + 20, 14, 14, t.bright_foreground(), fo::text::REGULAR, 11,
                           FL_ALIGN_CENTER, fo::text::Elide::None);
    }
    int fs = dual ? 12 : 13;
    std::string name = display_name(e);
    if (show_folders_ && e.name.find('/') != std::string::npos) {
        std::string where = e.name.substr(0, e.name.rfind('/'));
        int nw = std::min(c.name_w, int(fo::text::width(name, fo::text::REGULAR, fs)) + 1);
        fo::text::draw(name, X + c.name_x, Y, nw, H, t.bright_foreground(), fo::text::REGULAR, fs, FL_ALIGN_LEFT,
                       fo::text::Elide::Middle);
        if (nw + 16 < c.name_w)
            fo::text::draw(where, X + c.name_x + nw + 12, Y, c.name_w - nw - 12, H, t.dim(), fo::text::REGULAR, 11,
                           FL_ALIGN_LEFT, fo::text::Elide::Middle);
    } else {
        fo::text::draw(name, X + c.name_x, Y, c.name_w, H, t.bright_foreground(), fo::text::REGULAR, fs,
                       FL_ALIGN_LEFT, fo::text::Elide::Middle);
    }
    if (c.size_w) {
        if (e.is_dir) {
            if (const FolderSizes::Result *r = App::get().sizes().get(path))
                fo::text::draw("\xE2\x88\x91 " + si_size(r->bytes), X + c.size_x, Y, c.size_w, H, t.accent(),
                               fo::text::REGULAR, 11, FL_ALIGN_RIGHT);
        } else {
            std::string sz = si_size(uint64_t(std::max<int64_t>(0, e.size)));
            size_t sp = sz.find(' ');
            fo::text::draw(sz.substr(0, sp), X + c.size_x, Y, c.size_w - 32, H, t.dim(), fo::text::REGULAR, 12,
                           FL_ALIGN_RIGHT);
            fo::text::draw(sz.substr(sp + 1), X + c.size_x + c.size_w - 28, Y, 28, H, t.dim(), fo::text::REGULAR, 11,
                           FL_ALIGN_LEFT);
        }
    }
    if (c.date_w)
        fo::text::draw(relative_when(e.mtime_ns), X + c.date_x, Y, c.date_w, H, t.dim(), fo::text::REGULAR, 11,
                       FL_ALIGN_RIGHT);
    if (c.kind_w)
        fo::text::draw(kind_word(e), X + c.kind_x, Y, c.kind_w, H, t.dim(), fo::text::REGULAR, 11, FL_ALIGN_RIGHT);
}

void FileView::draw_column_item(int i, int X, int Y, int W, int H, const ItemState &s) {
    const fo::Theme &t = fo::theme();
    const Entry &e = model_->entries[size_t(i)];
    const std::string name = display_name(e);
    bool on_trail = !trail_.empty() && name == trail_;
    if (s.selected || s.drop_target)
        fo::paint::round_fill(X, Y, W, H, rounded(7), t.selection());
    else if (on_trail)
        fo::paint::round_fill(X, Y, W, H, rounded(7), t.card());
    else if (s.hover > 0)
        fo::paint::round_fill(X, Y, W, H, rounded(7), fo::mix(t.background(), t.card(), s.hover));
    fo::icon::draw(e.is_dir ? "folder" : "file", X + 8, Y + (H - 14) / 2, 14, e.is_dir ? t.blue() : t.dim());
    int right = X + W - 8;
    if (e.is_dir) {
        fo::text::draw("\xE2\x80\xBA", right - 8, Y, 8, H, t.dim(), fo::text::REGULAR, 12, FL_ALIGN_CENTER,
                       fo::text::Elide::None);
        right -= 12;
        if (const FolderSizes::Result *r = App::get().sizes().get(model_->path(i))) {
            std::string sz = si_size(r->bytes);
            int sw = int(fo::text::width(sz, fo::text::REGULAR, 10)) + 2;
            fo::text::draw(sz, right - sw, Y, sw, H, t.accent(), fo::text::REGULAR, 10, FL_ALIGN_RIGHT);
            right -= sw + 6;
        }
    }
    fo::text::draw(name, X + 30, Y, right - X - 30, H, t.bright_foreground(), fo::text::REGULAR, 12, FL_ALIGN_LEFT,
                   fo::text::Elide::Middle);
}

} // namespace flo::ui

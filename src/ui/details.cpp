#include "ui/details.h"

#include "core/fsutil.h"
#include "core/trash.h"
#include "ui/app.h"
#include "ui/file_view.h"
#include "ui/pane.h"
#include "ui/sizes.h"
#include "ui/thumbs.h"

#include "core/tree_size.h"

#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/process.h"
#include "fltk-omarchy/text.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace flo::ui {

namespace {
const int kPad = 28;

uint32_t be32(const unsigned char *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const unsigned char *p) { return uint16_t(p[0] << 8 | p[1]); }
uint16_t le16(const unsigned char *p) { return uint16_t(p[1] << 8 | p[0]); }
uint32_t le32(const unsigned char *p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

std::string read_head(const std::string &path, size_t n) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return {};
    std::string buf(n, '\0');
    ssize_t got = read(fd, &buf[0], n);
    close(fd);
    buf.resize(got > 0 ? size_t(got) : 0);
    return buf;
}

std::string dims(long w, long h) {
    if (w <= 0 || h <= 0)
        return {};
    return std::to_string(w) + " \xC3\x97 " + std::to_string(h);
}
} // namespace

std::string image_dimensions(const std::string &path) {
    std::string b = read_head(path, 64 * 1024);
    auto *p = reinterpret_cast<const unsigned char *>(b.data());
    size_t n = b.size();
    if (n >= 24 && !std::memcmp(p, "\x89PNG\r\n\x1a\n", 8))
        return dims(be32(p + 16), be32(p + 20));
    if (n >= 10 && (!std::memcmp(p, "GIF87a", 6) || !std::memcmp(p, "GIF89a", 6)))
        return dims(le16(p + 6), le16(p + 8));
    if (n >= 26 && p[0] == 'B' && p[1] == 'M')
        return dims(long(int32_t(le32(p + 18))), std::labs(long(int32_t(le32(p + 22)))));
    if (n >= 30 && !std::memcmp(p, "RIFF", 4) && !std::memcmp(p + 8, "WEBP", 4)) {
        if (!std::memcmp(p + 12, "VP8X", 4))
            return dims(1 + long(p[24] | p[25] << 8 | p[26] << 16), 1 + long(p[27] | p[28] << 8 | p[29] << 16));
        if (!std::memcmp(p + 12, "VP8 ", 4) && n >= 30)
            return dims(le16(p + 26) & 0x3fff, le16(p + 28) & 0x3fff);
        if (!std::memcmp(p + 12, "VP8L", 4) && n >= 25) {
            uint32_t v = le32(p + 21);
            return dims(1 + long(v & 0x3fff), 1 + long((v >> 14) & 0x3fff));
        }
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (p[i] != 0xFF) {
                ++i;
                continue;
            }
            unsigned char m = p[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                i += 2;
                continue;
            }
            size_t len = be16(p + i + 2);
            if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) ||
                (m >= 0xCD && m <= 0xCF))
                return dims(be16(p + i + 7), be16(p + i + 5));
            i += 2 + len;
        }
    }
    return {};
}

DetailsPane::DetailsPane(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    box(FL_NO_BOX);
    end();
    resizable(nullptr);
    clear_visible_focus();
}

DetailsPane::~DetailsPane() = default;

void DetailsPane::resize(int X, int Y, int W, int H) { Fl_Widget::resize(X, Y, W, H); }

void DetailsPane::relabel() {
    path_.clear();
    has_ = false;
    show_from(pane_);
}

void DetailsPane::show_from(Pane *pane) {
    pane_ = pane;
    const Entry *e = pane && visible_r() ? pane->current_entry() : nullptr;
    std::string path = e ? pane->current_path() : std::string();
    has_ = e != nullptr;
    bool same = has_ && path == path_ && e->mtime_ns == entry_.mtime_ns;
    path_ = path;
    if (e)
        entry_ = *e;
    trashed_ = has_ && in_trash_folder(path_) && path_dirname(path_) == trash::home_trash() + "/files";
    if (!same)
        describe();
    redraw();
}

void DetailsPane::describe() {
    facts_ = std::make_shared<Facts>();
    if (!has_)
        return;
    auto facts = facts_;
    std::string path = path_;
    Kind kind = entry_.kind;
    bool is_dir = entry_.is_dir, trashed = trashed_;
    uint64_t gen = ++generation_;
    fo::tasks::run(
        [facts, path, kind, is_dir, trashed] {
            if (trashed) {
                trash::Item item;
                if (trash::item_for_trashed_path(path, item)) {
                    facts->origin = item.original_path;
                    facts->deleted_at = item.deleted_at;
                }
            }
            if (is_dir)
                return;
            if (kind == Kind::Image)
                facts->dimensions = image_dimensions(path);
            if (kind == Kind::Text) {
                std::string head = read_head(path, 4096);
                if (head.find('\0') == std::string::npos)
                    facts->text = head;
            }
        },
        [this, facts, gen] {
            if (gen != generation_)
                return;
            facts->ready = true;
            redraw();
        });
}

// Files' inspector (view.rs inspector()): positions follow the window's
// height the way Files' do; the buttons are drawn and hit here.
void DetailsPane::buttons(std::vector<Btn> &out) const {
    out.clear();
    if (!has_)
        return;
    const int iw = w(), x0 = x(), bottom = y() + h();
    const int ph = std::max(38, std::min(98, (h() + 122 - 424) / 2));
    const int title_y = y() + 62 + ph;
    if (entry_.is_dir && title_y + 164 - y() <= h() - 52) {
        std::string label;
        if (App::get().sizes().pending(path_))
            label = fo::tr("Calculating\xE2\x80\xA6");
        else if (App::get().sizes().get(path_))
            label = std::string(fo::tr("Recalculate")) + " \xC2\xB7 Ctrl+Space";
        else
            label = std::string(fo::tr("Calculate")) + " \xC2\xB7 Ctrl+Space";
        out.push_back({x0 + 16, title_y + 130, iw - 32, 30, "calculator", label, false, Act::Measure});
    }
    out.push_back({x0 + 16, bottom - 136, iw - 32, 30, "edit", std::string(fo::tr("Rename")) + " \xC2\xB7 F2", false, Act::Rename});
    int half = (iw - 40) / 2;
    out.push_back({x0 + 16, bottom - 94, half, 34, "", trashed_ ? fo::tr("Restore") : fo::tr("Open"), true, Act::Open});
    out.push_back({x0 + 24 + half, bottom - 94, half, 34, "image", fo::tr("Preview"), false, Act::Preview});
    out.push_back({x0 + 16, bottom - 52, 60, 34, "link", "", false, Act::CopyPath});
    out.push_back({x0 + 80, bottom - 52, 68, 34, "copy", "F5", false, Act::Copy});
    out.push_back({x0 + 152, bottom - 52, 68, 34, "arrow-right", "F6", false, Act::Move});
}

int DetailsPane::handle(int event) {
    std::vector<Btn> bs;
    buttons(bs);
    int ex = Fl::event_x(), ey = Fl::event_y();
    int at = -1;
    for (size_t i = 0; i < bs.size(); ++i)
        if (ex >= bs[i].x && ex < bs[i].x + bs[i].w && ey >= bs[i].y && ey < bs[i].y + bs[i].h)
            at = int(i);
    switch (event) {
    case FL_ENTER:
    case FL_MOVE:
        if (at != hover_) {
            hover_ = at;
            redraw();
        }
        return 1;
    case FL_LEAVE:
        hover_ = -1;
        redraw();
        return 1;
    case FL_PUSH:
        return at >= 0 ? 1 : 0;
    case FL_RELEASE: {
        if (at < 0 || !pane_ || !has_)
            return 1;
        Pane *p = pane_;
        std::string path = path_;
        switch (bs[size_t(at)].act) {
        case Act::Measure: p->measure_focused(); break;
        case Act::Rename: p->rename(); break;
        case Act::Open:
            if (trashed_)
                p->restore({path});
            else
                p->open_path(path);
            break;
        case Act::Preview: p->preview(); break;
        case Act::CopyPath: p->copy_path(); break;
        case Act::Copy: p->transfer(false); break;
        case Act::Move: p->transfer(true); break;
        }
        return 1;
    }
    }
    return 0;
}

void DetailsPane::draw() {
    const fo::Theme &t = fo::theme();
    fo::paint::fill(x(), y(), w(), h(), t.background());
    fo::paint::vline(x(), y(), h(), t.card());
    fo::text::draw(fo::tr("INSPECTOR"), x() + 16, y() + 12, w() - 32, 22, t.dim(), fo::text::REGULAR, 10);
    if (!has_) {
        fo::text::draw(fo::tr("Select an item"), x() + 16, y() + 52, w() - 32, 28, t.dim(), fo::text::REGULAR, 12);
        return;
    }
    fl_push_clip(x() + 8, y() + 8, w() - 16, h() - 16);
    const int iw = w() - 32, ix = x() + 16;
    const int ph = std::max(38, std::min(98, (h() + 122 - 424) / 2));
    const int ty = y() + 50, title_y = y() + 62 + ph;
    const int r = t.radius() > 0 ? 7 : 0;
    const Facts &f = *facts_;
    // The preview tile.
    fo::paint::round_fill(ix, ty, iw, ph, r, t.card());
    Fl_RGB_Image *img = nullptr;
    if (!entry_.is_dir && App::get().settings().thumbnails)
        img = App::get().thumbs().get(path_, entry_, 256, iw, ph, false, this);
    if (img) {
        img->draw(ix + (iw - img->w()) / 2, ty + (ph - img->h()) / 2);
    } else if (entry_.is_dir) {
        fo::icon::draw("folder", ix + iw / 2 - 12, ty + ph / 2 - 12, 25, t.blue());
    } else if (entry_.kind == Kind::Text && f.ready && !f.text.empty()) {
        fl_push_clip(ix + 8, ty + 6, iw - 16, ph - 12);
        fl_font(FL_COURIER, 9);
        fl_color(t.foreground().fl());
        int ly = ty + 6 + fl_height() - fl_descent();
        size_t pos = 0;
        while (pos < f.text.size() && ly < ty + ph) {
            size_t nl = f.text.find('\n', pos);
            std::string line = f.text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            for (auto &c : line)
                if (c == '\t')
                    c = ' ';
            fl_draw(line.c_str(), ix + 8, ly);
            ly += fl_height();
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
        fl_pop_clip();
    } else {
        int bx = ix + iw / 2 - 24, by = ty + (ph - 48) / 2;
        fo::paint::round_fill(bx, by, 48, 48, r ? 10 : 0, badge_color(entry_));
        std::string ext = entry_.extension();
        fo::text::draw(ext.empty() ? "FILE" : ext.substr(0, 5), bx + 2, by, 44, 48, t.background(), fo::text::REGULAR, 14,
                       FL_ALIGN_CENTER, fo::text::Elide::None);
    }
    // Name, kind and the facts.
    fo::text::draw(display_name(entry_), ix, title_y, iw, 36, t.bright_foreground(), fo::text::REGULAR, 13,
                   FL_ALIGN_LEFT, fo::text::Elide::Middle);
    std::string kind = entry_.is_link && !entry_.link_target.empty()
                           ? fo::i18n::arg(fo::tr("Link \xE2\x86\x92 %1"), entry_.link_target)
                           : entry_.is_dir ? fo::tr("Folder") : std::string(entry_.mime ? entry_.mime : "");
    fo::text::draw(kind, ix, title_y + 44, iw, 22, t.dim(), fo::text::REGULAR, 11, FL_ALIGN_LEFT,
                   fo::text::Elide::Middle);
    int line_y = title_y + 72;
    if (entry_.is_dir) {
        const FolderSizes::Result *sz = App::get().sizes().get(path_);
        std::string total = sz ? fo::i18n::arg(fo::tr("Total: %1"), si_size(sz->bytes)) : fo::tr("Size not calculated");
        fo::text::draw(total, ix, line_y, iw, 22, t.foreground(), fo::text::REGULAR, 11);
        std::string count;
        if (sz)
            count = fo::trn("%n entries", int(sz->files + sz->folders)) + " \xC2\xB7 " + relative_when(entry_.mtime_ns);
        else if (entry_.item_count >= 0)
            count = fo::trn("%n entries", entry_.item_count) + " \xC2\xB7 " + relative_when(entry_.mtime_ns);
        else
            count = fo::tr("Does not follow links");
        fo::text::draw(count, ix, line_y + 26, iw, 22, t.dim(), fo::text::REGULAR, 11);
    } else {
        fo::text::draw(fo::tr("Size"), ix, line_y, 80, 22, t.foreground(), fo::text::REGULAR, 12);
        fo::text::draw(si_size(uint64_t(std::max<int64_t>(0, entry_.size))), ix + 80, line_y, iw - 80, 22,
                       t.foreground(), fo::text::REGULAR, 12);
        fo::text::draw(relative_when(entry_.mtime_ns), ix, line_y + 26, iw, 22, t.foreground(), fo::text::REGULAR,
                       12);
        int fy = line_y + 52;
        auto fact = [&](const std::string &label, const std::string &value) {
            if (value.empty() || fy + 22 > y() + h() - 176)
                return;
            fo::text::draw(label, ix, fy, 80, 20, t.dim(), fo::text::REGULAR, 11);
            fo::text::draw(value, ix + 80, fy, iw - 80, 20, t.foreground(), fo::text::REGULAR, 11, FL_ALIGN_LEFT,
                           fo::text::Elide::Middle);
            fy += 22;
        };
        fact(fo::tr("Pixels"), f.dimensions);
        if (trashed_) {
            fact(fo::tr("From"), f.origin.empty() ? std::string() : tilde(path_dirname(f.origin)));
            fact(fo::tr("Deleted"), f.deleted_at > 0 ? fo::i18n::format_date(std::time_t(f.deleted_at)) : std::string());
        }
        if (title_y + 164 - y() <= h() - 52 && fy <= line_y + 52)
            fo::text::draw(fo::tr("Enter opens \xC2\xB7 Backspace goes up"), ix, title_y + 130, iw, 28, t.dim(),
                           fo::text::REGULAR, 11);
    }
    fl_pop_clip();
    std::vector<Btn> bs;
    buttons(bs);
    for (size_t i = 0; i < bs.size(); ++i) {
        const Btn &b = bs[i];
        bool hov = int(i) == hover_;
        fo::Rgb fill = b.primary ? t.card() : hov ? fo::mix(t.background(), t.card(), 0.6) : t.background();
        if (fill != t.background())
            fo::paint::round_fill(b.x, b.y, b.w, b.h, r, fill);
        fo::Rgb fg = b.primary ? t.accent() : hov ? t.foreground() : t.dim();
        int lx = b.x + 10;
        if (!b.icon.empty()) {
            int iwid = b.label.empty() ? b.w : 0;
            if (iwid)
                fo::icon::draw(b.icon, b.x + (b.w - 12) / 2, b.y + (b.h - 12) / 2, 12, fg);
            else {
                fo::icon::draw(b.icon, lx, b.y + (b.h - 12) / 2, 12, fg);
                lx += 18;
            }
        }
        if (!b.label.empty())
            fo::text::draw(b.label, lx, b.y, b.x + b.w - lx - 6, b.h, fg, fo::text::REGULAR, 12);
    }
}

} // namespace flo::ui

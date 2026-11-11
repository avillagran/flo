#include "ui/details.h"

#include "core/fsutil.h"
#include "core/trash.h"
#include "ui/app.h"
#include "ui/file_view.h"
#include "ui/pane.h"
#include "ui/thumbs.h"

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
    open_ = new fo::Button(x + kPad, y + h - kPad - 40, w - 2 * kPad, 40, nullptr);
    open_->primary(true);
    open_->clear_visible_focus();
    open_->hide();
    end();
    resizable(nullptr);
    open_->on_click([this] {
        if (!pane_ || !has_)
            return;
        if (trashed_)
            pane_->restore({path_});
        else
            pane_->open_path(path_);
    });
}

DetailsPane::~DetailsPane() = default;

void DetailsPane::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    open_->resize(X + kPad, Y + H - kPad - 40, std::max(40, W - 2 * kPad), 40);
}

void DetailsPane::show_from(Pane *pane) {
    pane_ = pane;
    const Entry *e = pane && visible_r() ? pane->current_entry() : nullptr;
    std::string path = e ? pane->current_path() : std::string();
    if (e && path == path_ && has_ && e->mtime_ns == entry_.mtime_ns && e->item_count == entry_.item_count)
        return;
    has_ = e != nullptr;
    path_ = path;
    if (e)
        entry_ = *e;
    trashed_ = has_ && in_trash_folder(path_) && path_dirname(path_) == trash::home_trash() + "/files";
    if (has_) {
        open_->copy_label(trashed_ ? fo::tr("Restore") : entry_.is_dir ? fo::tr("Open folder") : fo::tr("Open"));
        open_->show();
    } else {
        open_->hide();
    }
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

void DetailsPane::draw() {
    const fo::Theme &t = fo::theme();
    auto hint = [&] {
        // The Enter hint inside the button, as Folio has it.
        if (open_->visible())
            fo::text::draw("\xE2\x8F\x8E", open_->x(), open_->y(), open_->w() - 14, open_->h(),
                           t.accent_foreground(), fo::text::REGULAR, fo::text::size::BODY, FL_ALIGN_RIGHT);
    };
    if (!(damage() & ~FL_DAMAGE_CHILD)) {
        draw_children(); // the button's hover
        hint();
        return;
    }
    fo::paint::fill(x(), y(), w(), h(), t.dark_background());
    fo::paint::vline(x(), y(), h(), t.hairline());
    if (!has_) {
        fo::text::draw(fo::tr("Nothing selected"), x(), y(), w(), h(), t.secondary_foreground(), fo::text::REGULAR,
                       fo::text::size::BODY, FL_ALIGN_CENTER);
        return;
    }
    fl_push_clip(x() + 1, y(), w() - 1, h());
    const int iw = w() - 2 * kPad, ix = x() + kPad;
    int yy = y() + kPad;
    const int ph = iw * 3 / 4;
    // The large look.
    const Facts &f = *facts_;
    if (entry_.kind == Kind::Text && !entry_.is_dir) {
        fo::paint::fill(ix, yy, iw, ph, t.background());
        fo::paint::round_frame(ix, yy, iw, ph, 0, 1, t.hairline());
        if (f.ready && !f.text.empty()) {
            fl_push_clip(ix + 14, yy + 14, iw - 28, ph - 28);
            fl_font(FL_COURIER, 10);
            fl_color(t.foreground().fl());
            int ly = yy + 14 + fl_height() - fl_descent();
            size_t pos = 0;
            while (pos < f.text.size() && ly < yy + ph) {
                size_t nl = f.text.find('\n', pos);
                std::string line = f.text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                for (auto &c : line)
                    if (c == '\t')
                        c = ' ';
                fl_draw(line.c_str(), ix + 14, ly);
                ly += fl_height();
                if (nl == std::string::npos)
                    break;
                pos = nl + 1;
            }
            fl_pop_clip();
        }
    } else {
        Fl_RGB_Image *img = entry_.is_dir ? nullptr : App::get().thumbs().get(path_, entry_, 512, iw, ph, false, this);
        if (img) {
            img->draw(ix + (iw - img->w()) / 2, yy + (ph - img->h()) / 2);
        } else if (entry_.is_dir) {
            draw_folder_card(ix + iw / 2 - 64, yy + ph / 2 - 46, 128, 92, t.lighter_background(), t.accent(), 0);
        } else {
            fo::paint::fill(ix, yy, iw, ph, t.background());
            fo::icon::draw(kind_icon(entry_), ix + iw / 2 - 36, yy + ph / 2 - 36, 72, kind_color(entry_), 1.2);
        }
    }
    yy += ph + 22;
    // Name and summary.
    auto lines = fo::text::wrap(display_name(entry_), iw, fo::text::fltk_measure(fo::text::SEMIBOLD, 15), 3);
    for (const auto &l : lines) {
        fo::text::draw(l, ix, yy, iw, 20, t.bright_foreground(), fo::text::SEMIBOLD, 15, FL_ALIGN_LEFT,
                       fo::text::Elide::None);
        yy += 20;
    }
    std::string type = entry_.is_dir ? fo::tr("Folder") : type_description(entry_.mime);
    std::string size = size_text(entry_);
    std::string summary = type.empty() ? size : size.empty() ? type : type + " \xC2\xB7 " + size;
    fo::text::draw(summary, ix, yy + 2, iw, 18, t.secondary_foreground(), fo::text::REGULAR, fo::text::size::LABEL);
    yy += 22 + 22;
    auto row = [&](const std::string &label, const std::string &value) {
        if (value.empty())
            return;
        fo::text::draw(label, ix, yy, iw, 18, t.secondary_foreground(), fo::text::REGULAR, fo::text::size::LABEL);
        int lw = int(fo::text::width(label, fo::text::REGULAR, fo::text::size::LABEL)) + 16;
        fo::text::draw(value, ix + lw, yy, iw - lw, 18, t.foreground(), fo::text::REGULAR, fo::text::size::LABEL,
                       FL_ALIGN_RIGHT, fo::text::Elide::Middle);
        yy += 30;
    };
    if (trashed_) {
        row(fo::tr("From"), f.origin.empty() ? std::string() : tilde(path_dirname(f.origin)));
        row(fo::tr("Deleted"), f.deleted_at > 0 ? fo::i18n::format_date(std::time_t(f.deleted_at)) + " " +
                                                      fo::i18n::format_time(std::time_t(f.deleted_at))
                                                : std::string());
    } else {
        std::time_t m = std::time_t(entry_.mtime_ns / 1000000000);
        row(fo::tr("Modified"), m > 0 ? fo::i18n::format_date(m) + " " + fo::i18n::format_time(m) : std::string());
    }
    row(fo::tr("Dimensions"), f.dimensions);
    if (entry_.is_link)
        row(fo::tr("Points to"), entry_.link_target);
    fl_pop_clip();
    draw_children();
    hint();
}

} // namespace flo::ui

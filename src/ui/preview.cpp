// `flo preview PATH`: the large look at one file, in its own process (as
// Folio's `folio preview`, like Sushi for Nautilus), so a decoder that
// crashes takes the preview down and never the file manager.
//
// Pictures (JPEG, PNG, GIF, BMP, SVG) are decoded at full size with zoom;
// other types show the largest thumbnail the system can make (WebP, HEIC,
// AVIF, video frames, PDF pages, covers); text shows its opening part.
// Left/Right step through the folder, Enter opens the file in its app,
// Space, Esc or Q close.
//
// It talks to the Flo window that started it over stdin and stdout, one
// path per line: Flo writes the item selected, the preview writes the item
// it steps to, and each side follows the other.

#include "core/entry.h"
#include "core/event_queue.h"
#include "core/fsutil.h"
#include "core/lister.h"
#include "core/thumbnail_service.h"
#include "ui/app.h"
#include "ui/details.h"
#include "ui/file_view.h"
#include "ui/strings.h"
#include "ui/util.h"

#include "fltk-omarchy/fo.h"

#include <FL/Fl.H>
#include <FL/Fl_BMP_Image.H>
#include <FL/Fl_GIF_Image.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_SVG_Image.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_draw.H>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace flo::ui {

namespace {

const int kHeader = 60;

std::string read_text(const std::string &path, size_t limit) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return {};
    std::string s;
    char buf[65536];
    while (s.size() < limit) {
        ssize_t n = read(fd, buf, std::min(sizeof buf, limit - s.size()));
        if (n <= 0)
            break;
        s.append(buf, size_t(n));
    }
    close(fd);
    return s;
}

bool looks_binary(const std::string &s) { return s.find('\0') != std::string::npos; }

// Full-size decoding by content, for the formats FLTK reads itself.
Fl_RGB_Image *decode_full(const std::string &path) {
    std::string head = read_text(path, 16);
    auto *p = reinterpret_cast<const unsigned char *>(head.data());
    Fl_RGB_Image *img = nullptr;
    if (head.size() >= 3 && p[0] == 0xFF && p[1] == 0xD8)
        img = new Fl_JPEG_Image(path.c_str());
    else if (head.size() >= 8 && !std::memcmp(p, "\x89PNG", 4))
        img = new Fl_PNG_Image(path.c_str());
    else if (head.size() >= 6 && !std::memcmp(p, "GIF8", 4)) {
        Fl_GIF_Image gif(path.c_str());
        if (!gif.fail() && gif.w() > 0)
            img = new Fl_RGB_Image(&gif, FL_BLACK);
    }
    else if (head.size() >= 2 && p[0] == 'B' && p[1] == 'M')
        img = new Fl_BMP_Image(path.c_str());
    else if (path.size() > 4 && (path.compare(path.size() - 4, 4, ".svg") == 0 || path.compare(path.size() - 5, 5, ".svgz") == 0))
        img = new Fl_SVG_Image(path.c_str());
    if (img && (img->fail() || img->w() <= 0 || img->h() <= 0)) {
        delete img;
        img = nullptr;
    }
    return img;
}

class PreviewWindow;

// The picture (or stand-in) area.
class Stage : public Fl_Widget {
public:
    Stage(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_NO_BOX); }
    ~Stage() override {
        delete img_;
        clear_peek();
    }
    // A folder's peek: up to two of its pictures (Folio's folder view).
    void clear_peek() {
        for (auto *i : peek_)
            delete i;
        peek_.clear();
        redraw();
    }
    // Pictures keep their folder order whichever thumbnail arrives first.
    void add_peek(std::size_t slot, Fl_RGB_Image *img) {
        if (slot >= 2) {
            delete img;
            return;
        }
        if (peek_.size() <= slot)
            peek_.resize(slot + 1, nullptr);
        delete peek_[slot];
        peek_[slot] = img;
        redraw();
    }
    void set_image(Fl_RGB_Image *img) {
        delete img_;
        img_ = img;
        zoom_ = 1;
        pan_x_ = pan_y_ = 0;
        redraw();
    }
    bool has_image() const { return img_ != nullptr; }
    void set_entry(const Entry &e, const std::string &note) {
        entry_ = e;
        note_ = note;
        redraw();
    }
    void zoom_by(double f) {
        zoom_ = std::max(0.1, std::min(16.0, zoom_ * f));
        redraw();
    }
    void fit() {
        zoom_ = 1;
        pan_x_ = pan_y_ = 0;
        redraw();
    }
    void draw() override {
        const fo::Theme &t = fo::theme();
        fl_push_clip(x(), y(), w(), h());
        fo::paint::fill(x(), y(), w(), h(), t.dark_background());
        if (img_) {
            int iw = img_->data_w(), ih = img_->data_h();
            double f = std::min(double(w() - 48) / iw, double(h() - 48) / ih);
            f = std::min(f, 1.0) * zoom_; // never upscale to fit
            int dw = std::max(1, int(iw * f)), dh = std::max(1, int(ih * f));
            img_->scale(dw, dh, 0, 1);
            img_->draw(x() + (w() - dw) / 2 + pan_x_, y() + (h() - dh) / 2 + pan_y_);
        } else if (entry_.is_dir && !peek_.empty()) {
            // Pictures 220 x 165, filled and cropped, 16 apart.
            const int pw = 220, ph = 165, gap = 16;
            const int n = int(std::count_if(peek_.begin(), peek_.end(), [](Fl_RGB_Image *i) { return i != nullptr; }));
            int px = x() + (w() - (n * pw + (n - 1) * gap)) / 2, py = y() + h() / 2 - ph / 2 - 30;
            for (auto *img : peek_) {
                if (!img)
                    continue;
                double f = std::max(double(pw) / img->data_w(), double(ph) / img->data_h());
                int dw = std::max(1, int(img->data_w() * f)), dh = std::max(1, int(img->data_h() * f));
                img->scale(dw, dh, 0, 1);
                fl_push_clip(px, py, pw, ph);
                img->draw(px + (pw - dw) / 2, py + (ph - dh) / 2);
                fl_pop_clip();
                px += pw + gap;
            }
            fo::text::draw(note_, x(), py + ph + 18, w(), 24, t.secondary_foreground(), fo::text::REGULAR,
                           fo::text::size::BODY, FL_ALIGN_CENTER);
        } else if (entry_.is_dir) {
            draw_folder_card(x() + w() / 2 - 128, y() + h() / 2 - 120, 256, 184, t.lighter_background(), t.accent(), 0);
            fo::text::draw(note_, x(), y() + h() / 2 + 80, w(), 24, t.secondary_foreground(), fo::text::REGULAR,
                           fo::text::size::BODY, FL_ALIGN_CENTER);
        } else {
            fo::icon::draw(kind_icon(entry_), x() + w() / 2 - 48, y() + h() / 2 - 80, 96, kind_color(entry_), 1.2);
            fo::text::draw(note_, x(), y() + h() / 2 + 36, w(), 24, t.secondary_foreground(), fo::text::REGULAR,
                           fo::text::size::BODY, FL_ALIGN_CENTER);
        }
        fl_pop_clip();
    }
    int handle(int e) override {
        switch (e) {
        case FL_PUSH:
            drag_x_ = Fl::event_x() - pan_x_;
            drag_y_ = Fl::event_y() - pan_y_;
            return 1;
        case FL_DRAG:
            if (zoom_ > 1) {
                pan_x_ = Fl::event_x() - drag_x_;
                pan_y_ = Fl::event_y() - drag_y_;
                redraw();
            }
            return 1;
        case FL_MOUSEWHEEL:
            if (Fl::event_state() & FL_CTRL) {
                zoom_by(Fl::event_dy() < 0 ? 1.25 : 0.8);
                return 1;
            }
            return 0;
        }
        return 0;
    }

private:
    Fl_RGB_Image *img_ = nullptr;
    std::vector<Fl_RGB_Image *> peek_;
    Entry entry_;
    std::string note_;
    double zoom_ = 1;
    int pan_x_ = 0, pan_y_ = 0, drag_x_ = 0, drag_y_ = 0;
};

std::string of_text(int i, int n) {
    std::string s = fo::tr("%1 of %2");
    auto put = [&s](const char *key, const std::string &v) {
        auto at = s.find(key);
        if (at != std::string::npos)
            s.replace(at, 2, v);
    };
    put("%1", std::to_string(i));
    put("%2", std::to_string(n));
    return s;
}

// The title strip: name, then "3 of 12 · JPEG image · 2.1 MB · 1600 × 1200".
class Header : public Fl_Widget {
public:
    Header(int x, int y, int w, int h, PreviewWindow *win) : Fl_Widget(x, y, w, h), win_(win) { box(FL_NO_BOX); }
    void draw() override;

private:
    PreviewWindow *win_;
};

class PreviewWindow : public fo::Window {
public:
    PreviewWindow(EventQueue &q, const std::string &path, bool hidden, SortMode sort)
        : fo::Window(1100, 760, "Flo"), queue_(q), hidden_(hidden), sort_(sort) {
        size_range(480, 360);
        header_ = new Header(0, 0, w(), kHeader, this);
        stage_ = new Stage(0, kHeader, w(), h() - kHeader);
        text_ = new Fl_Text_Display(0, kHeader, w(), h() - kHeader);
        open_b_ = new fo::IconButton(w() - 92, 12, 36, 36, "external-link");
        close_b_ = new fo::IconButton(w() - 52, 12, 36, 36, "x");
        end();
        resizable(stage_);
        open_b_->tooltip_text(fo::tr("Open (\xE2\x8F\x8E)"));
        close_b_->tooltip_text(fo::tr("Close (Space)"));
        open_b_->on_click([this] { open_default(path_); });
        close_b_->on_click([this] { hide(); });
        text_->buffer(&buffer_);
        text_->box(FL_FLAT_BOX);
        text_->textfont(FL_COURIER);
        text_->textsize(13);
        text_->scrollbar_width(8);
        text_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        text_->hide();
        theme_colors();
        fo::on_theme_change([this] {
            theme_colors();
            redraw();
        });
        shortcut("Space", [this] { hide(); });
        shortcut("Escape", [this] { hide(); });
        shortcut("q", [this] { hide(); });
        shortcut("Left", [this] { step(-1, true); });
        shortcut("Right", [this] { step(1, true); });
        shortcut("Enter", [this] { open_default(path_); });
        shortcut("Ctrl+=", [this] { stage_->zoom_by(1.25); });
        shortcut("+", [this] { stage_->zoom_by(1.25); });
        shortcut("Ctrl+-", [this] { stage_->zoom_by(0.8); });
        shortcut("-", [this] { stage_->zoom_by(0.8); });
        shortcut("Ctrl+0", [this] { stage_->fit(); });
        shortcut("?", [this] {
            fo::HelpOverlay::show(this, {{"\xE2\x86\x90 / \xE2\x86\x92", fo::tr("Previous / next file")},
                                         {"\xE2\x8F\x8E", fo::tr("Open in its application")},
                                         {"+ / - / Ctrl+0", fo::tr("Zoom in, out, fit")},
                                         {"\xE2\x86\x91 / \xE2\x86\x93", fo::tr("Scroll text")},
                                         {"Space / Esc / Q", fo::tr("Close")}});
        });
        thumbs_ = std::make_unique<ThumbnailService>(queue_);
        show_path(path, false);
        list_siblings();
    }

    void show_path(const std::string &path, bool report) {
        path_ = path;
        Entry e;
        if (!stat_path(path, e, EntryOptions{true})) {
            e.name = path_basename(path);
        }
        e.name = path_basename(path);
        entry_ = e;
        ++generation_;
        int at = -1;
        for (size_t i = 0; i < siblings_.size(); ++i)
            if (siblings_[i] == path)
                at = int(i);
        if (at >= 0)
            index_ = at;
        if (report) {
            std::fputs((path + "\n").c_str(), stdout);
            std::fflush(stdout);
        }
        copy_label(e.name.c_str());
        stage_->set_image(nullptr);
        stage_->clear_peek();
        text_->hide();
        stage_->show();
        std::string note;
        if (e.is_dir) {
            FolderSummary sum = summarize_folder(path, hidden_, 2);
            note = fo::trn("%n items", sum.count);
            load_peek(sum);
        } else if (e.kind == Kind::Text || (e.mime && std::strncmp(e.mime, "text/", 5) == 0)) {
            std::string t = read_text(path, 512 * 1024);
            if (!looks_binary(t)) {
                buffer_.text(t.c_str());
                text_->scroll(0, 0);
                stage_->hide();
                text_->show();
            }
        } else {
            note = fo::tr("Making a preview\xE2\x80\xA6");
            load_picture();
        }
        stage_->set_entry(e, note);
        dims_ = e.kind == Kind::Image ? image_dimensions(path) : std::string();
        redraw();
    }

    void load_picture() {
        uint64_t gen = generation_;
        std::string path = path_;
        auto *self = this;
        auto result = std::make_shared<Fl_RGB_Image *>(nullptr);
        if (entry_.kind == Kind::Image) {
            fo::tasks::run([result, path] { *result = decode_full(path); },
                           [self, result, gen] {
                               if (gen != self->generation_) {
                                   delete *result;
                                   return;
                               }
                               if (*result)
                                   self->stage_->set_image(*result);
                               else
                                   self->load_thumbnail();
                           });
        } else {
            load_thumbnail();
        }
    }

    // A folder's first pictures, from the shared cache's 256 px bucket.
    void load_peek(const FolderSummary &sum) {
        uint64_t gen = generation_;
        for (std::size_t k = 0; k < sum.peek.size(); ++k) {
            const Entry &pe = sum.peek[k];
            thumbs_->request(path_join(sum.folder, pe.name), 256, [this, gen, k](const ThumbnailResult &r) {
                if (gen != generation_ || r.status != ThumbnailStatus::Ready)
                    return;
                auto *img = new Fl_PNG_Image(r.thumbnail.c_str());
                if (img->fail())
                    delete img;
                else
                    stage_->add_peek(k, img);
            }, pe.mime);
        }
    }

    // Anything else: the biggest thumbnail the installed thumbnailers make.
    void load_thumbnail() {
        uint64_t gen = generation_;
        thumbs_->request(path_, 1024, [this, gen](const ThumbnailResult &r) {
            if (gen != generation_)
                return;
            if (r.status == ThumbnailStatus::Ready) {
                auto *img = new Fl_PNG_Image(r.thumbnail.c_str());
                if (img->fail()) {
                    delete img;
                } else {
                    stage_->set_image(img);
                    return;
                }
            }
            stage_->set_entry(entry_, fo::tr("No preview for this kind of file. \xE2\x8F\x8E opens it."));
        }, entry_.mime ? entry_.mime : "");
    }

    void list_siblings() {
        std::string folder = path_dirname(path_);
        bool hidden = hidden_;
        SortMode sort = sort_;
        auto out = std::make_shared<std::vector<std::string>>();
        fo::tasks::run(
            [out, folder, hidden, sort] {
                ListOptions o;
                o.show_hidden = hidden;
                o.sort = sort;
                o.sniff = false;
                Listing l = list_folder(folder, o);
                for (size_t i = 0; i < l.entries.size(); ++i)
                    out->push_back(l.path_of(i));
            },
            [this, out] {
                siblings_ = *out;
                index_ = -1;
                for (size_t i = 0; i < siblings_.size(); ++i)
                    if (siblings_[i] == path_)
                        index_ = int(i);
                redraw();
            });
    }

    void step(int by, bool report) {
        if (siblings_.empty() || index_ < 0)
            return;
        int to = std::max(0, std::min(int(siblings_.size()) - 1, index_ + by));
        if (to == index_)
            return;
        index_ = to;
        show_path(siblings_[size_t(to)], report);
    }

    void theme_colors() {
        const fo::Theme &t = fo::theme();
        color(t.dark_background().fl());
        text_->color(t.dark_background().fl());
        text_->textcolor(t.foreground().fl());
        text_->selection_color(t.accent().fl());
        text_->cursor_color(t.accent().fl());
    }

    void resize(int X, int Y, int W, int H) override {
        fo::Window::resize(X, Y, W, H);
        stage_->resize(0, kHeader, W, H - kHeader);
        text_->resize(24, kHeader + 12, W - 36, H - kHeader - 12);
        header_->resize(0, 0, W, kHeader);
        open_b_->resize(W - 92, 12, 36, 36);
        close_b_->resize(W - 52, 12, 36, 36);
    }

    void hide() override {
        fo::Window::hide();
        std::fflush(stdout);
    }

    EventQueue &queue_;
    std::unique_ptr<ThumbnailService> thumbs_;
    Header *header_;
    Stage *stage_;
    Fl_Text_Display *text_;
    Fl_Text_Buffer buffer_;
    fo::IconButton *open_b_, *close_b_;
    std::string path_, dims_;
    Entry entry_;
    bool hidden_;
    SortMode sort_;
    std::vector<std::string> siblings_;
    int index_ = -1;
    uint64_t generation_ = 0;
};

void Header::draw() {
    const fo::Theme &t = fo::theme();
    PreviewWindow &p = *win_;
    fo::paint::fill(x(), y(), w(), h(), t.background());
    fo::paint::hline(x(), y() + h() - 1, w(), t.hairline());
    int tw = w() - 24 - 110;
    fo::text::draw(p.entry_.name, 24, 10, tw, 22, t.bright_foreground(), fo::text::SEMIBOLD, fo::text::size::TITLE,
                   FL_ALIGN_LEFT, fo::text::Elide::Middle);
    std::vector<std::string> parts;
    if (p.index_ >= 0 && !p.siblings_.empty())
        parts.push_back(of_text(p.index_ + 1, int(p.siblings_.size())));
    std::string type = p.entry_.is_dir ? fo::tr("Folder") : type_description(p.entry_.mime ? p.entry_.mime : "");
    if (!type.empty())
        parts.push_back(type);
    if (!p.entry_.is_dir)
        parts.push_back(fo::i18n::format_size(uint64_t(std::max<int64_t>(0, p.entry_.size))));
    if (!p.dims_.empty())
        parts.push_back(p.dims_);
    std::string sub;
    for (const auto &s : parts)
        sub += (sub.empty() ? "" : " \xC2\xB7 ") + s;
    fo::text::draw(sub, 24, 32, tw, 18, t.secondary_foreground(), fo::text::REGULAR, fo::text::size::LABEL);
}

PreviewWindow *g_preview = nullptr;
std::string g_stdin;

void stdin_readable(int fd, void *) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        // Flo went away (or asked us to close): so do we.
        Fl::remove_fd(fd);
        if (g_preview)
            g_preview->hide();
        return;
    }
    g_stdin.append(buf, size_t(n));
    size_t nl;
    std::string last;
    while ((nl = g_stdin.find('\n')) != std::string::npos) {
        last = g_stdin.substr(0, nl);
        g_stdin.erase(0, nl + 1);
    }
    if (g_preview && !last.empty() && last[0] == '/' && last != g_preview->path_)
        g_preview->show_path(last, false);
}

} // namespace

} // namespace flo::ui

int run_preview(int argc, char **argv) {
    using namespace flo;
    std::string path;
    bool hidden = false;
    SortMode sort = SortMode::Name;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--hidden")
            hidden = true;
        else if (a == "--sort" && i + 1 < argc)
            parse_sort_mode(argv[++i], sort);
        else if (path.empty())
            path = absolute_path(a);
    }
    if (path.empty() || !exists_nofollow(path)) {
        std::fprintf(stderr, "flo preview: no such file\n");
        return 2;
    }
    ui::tune_malloc();
    ui::register_catalogs();
    fo::InitOptions opts;
    opts.app_class = "flo-preview";
    fo::init(opts);
    EventQueue queue;
    Fl::add_fd(queue.fd(), FL_READ, [](int, void *q) { static_cast<EventQueue *>(q)->dispatch(); }, &queue);
    auto *win = new ui::PreviewWindow(queue, path, hidden, sort);
    ui::g_preview = win;
    if (!isatty(0))
        Fl::add_fd(0, FL_READ, ui::stdin_readable, nullptr);
    win->show();
    int rc = Fl::run();
    ui::g_preview = nullptr;
    delete win;
    return rc;
}

#include "ui/thumbs.h"

#include "ui/app.h"

#include "fltk-omarchy/anim.h"

#include <FL/Fl.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flo::ui {

ThumbImages::ThumbImages(ThumbnailService &service, std::size_t max_bytes)
    : service_(service), max_bytes_(max_bytes) {}

ThumbImages::~ThumbImages() {
    Fl::remove_timeout(sweep_cb, this);
    clear();
}

void ThumbImages::clear() {
    for (auto &i : lru_)
        delete i.img;
    lru_.clear();
    index_.clear();
    bytes_ = 0;
}

bool ThumbImages::wanted(const Entry &e) {
    if (e.is_dir || e.broken_link)
        return false;
    if (!S_ISREG(e.mode) && !e.is_link)
        return false;
    return e.kind != Kind::Text;
}

std::string ThumbImages::source_key(const std::string &path, const Entry &e, int request_px) {
    return path + '\n' + std::to_string(e.mtime_ns) + '\n' + std::to_string(e.size) + '\n' + std::to_string(request_px);
}

bool ThumbImages::impossible(const std::string &path, const Entry &e, int request_px) const {
    return none_.count(source_key(path, e, request_px)) != 0;
}

static float screen_scale_of(Fl_Widget *w) {
    Fl_Window *win = w ? (w->as_window() ? w->as_window() : w->top_window()) : nullptr;
    int n = win && win->shown() ? win->screen_num() : 0;
    float s = Fl::screen_scale(n);
    return s > 0 ? s : 1.0f;
}

Fl_RGB_Image *ThumbImages::get(const std::string &path, const Entry &e, int request_px, int bw, int bh, bool crop,
                               Fl_Widget *who) {
    if (!wanted(e) || bw <= 0 || bh <= 0)
        return nullptr;
    const std::string skey = source_key(path, e, request_px);
    if (none_.count(skey))
        return nullptr;
    float scale = screen_scale_of(who);
    int pw = std::max(1, int(std::lround(bw * scale))), ph = std::max(1, int(std::lround(bh * scale)));
    const std::string key = skey + '\n' + std::to_string(pw) + 'x' + std::to_string(ph) + (crop ? "c" : "f");
    auto it = index_.find(key);
    if (it != index_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->img;
    }
    // The thumbnail is known (or already in the shared cache): scale it now.
    auto png = png_.find(skey);
    std::string file;
    if (png != png_.end()) {
        file = png->second;
    } else {
        ThumbnailCache::Hit hit;
        if (service_.lookup(path, request_px, hit)) {
            file = hit.path;
            png_[skey] = file;
        }
    }
    if (!file.empty()) {
        Fl_RGB_Image *img = decode_scaled(file, pw, ph, crop);
        if (img) {
            img->scale(std::max(1, int(std::lround(img->data_w() / scale))),
                       std::max(1, int(std::lround(img->data_h() / scale))), 0, 1);
            insert(key, img);
            return img;
        }
        png_.erase(skey); // gone or unreadable: ask again
    }
    // Ask the workers (once per source), and remember who wants it.
    Pending &p = pending_[skey];
    p.wanted_at = fo::anim::now_ms();
    bool known = false;
    for (auto &w : p.waiters)
        if (!w->deleted() && w->widget() == who)
            known = true;
    if (!known && who)
        p.waiters.push_back(std::make_unique<Fl_Widget_Tracker>(who));
    if (p.ticket == 0) {
        p.ticket = service_.request(path, request_px,
                                    [this, skey](const ThumbnailResult &r) { arrived(skey, r); }, e.mime);
        if (!sweeping_) {
            sweeping_ = true;
            Fl::add_timeout(1.0, sweep_cb, this);
        }
    }
    return nullptr;
}

void ThumbImages::arrived(const std::string &skey, const ThumbnailResult &r) {
    auto it = pending_.find(skey);
    if (it == pending_.end())
        return;
    if (r.status == ThumbnailStatus::Ready && !r.thumbnail.empty())
        png_[skey] = r.thumbnail;
    else
        none_[skey] = true;
    for (auto &w : it->second.waiters)
        if (!w->deleted())
            w->widget()->redraw();
    pending_.erase(it);
    App::get().trim_soon();
}

void ThumbImages::sweep_cb(void *data) { static_cast<ThumbImages *>(data)->sweep(); }

// Requests nobody has drawn for a while (scrolled away, folder left) are
// cancelled; they are asked again if they come back into view.
void ThumbImages::sweep() {
    double now = fo::anim::now_ms();
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.wanted_at > 1500) {
            service_.cancel(it->second.ticket);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    if (pending_.empty()) {
        sweeping_ = false;
        return;
    }
    Fl::repeat_timeout(1.0, sweep_cb, this);
}

void ThumbImages::insert(const std::string &key, Fl_RGB_Image *img) {
    std::size_t b = std::size_t(img->data_w()) * std::size_t(img->data_h()) * std::size_t(img->d());
    lru_.push_front(Image{key, img, b});
    index_[key] = lru_.begin();
    bytes_ += b;
    // Keep at least the newest few, whatever their size.
    while (bytes_ > max_bytes_ && lru_.size() > 8) {
        Image &old = lru_.back();
        bytes_ -= old.bytes;
        index_.erase(old.key);
        delete old.img;
        lru_.pop_back();
    }
}

// Area-averaging scale of the decoded thumbnail straight to the target, with
// a centered crop when filling.
Fl_RGB_Image *ThumbImages::decode_scaled(const std::string &file, int pw, int ph, bool crop) const {
    Fl_PNG_Image src(file.c_str());
    if (src.fail() || src.data_w() <= 0 || src.data_h() <= 0 || !src.data() || !src.data()[0])
        return nullptr;
    const int sw = src.data_w(), sh = src.data_h(), d = src.d();
    if (d < 1 || d > 4)
        return nullptr;
    const int ld = src.ld() ? src.ld() : sw * d;
    const auto *in = reinterpret_cast<const unsigned char *>(src.data()[0]);
    // Source rectangle and output size.
    double sx0 = 0, sy0 = 0, sww = sw, shh = sh;
    int ow, oh;
    if (crop) {
        ow = pw, oh = ph;
        double want = double(pw) / ph, have = double(sw) / sh;
        if (have > want) {
            sww = sh * want;
            sx0 = (sw - sww) / 2;
        } else {
            shh = sw / want;
            sy0 = (sh - shh) / 2;
        }
    } else {
        double f = std::min(double(pw) / sw, double(ph) / sh);
        ow = std::max(1, int(std::lround(sw * f)));
        oh = std::max(1, int(std::lround(sh * f)));
    }
    if (!crop && ow >= sw && oh >= sh) {
        ow = sw; // never upscale a fitted picture
        oh = sh;
    }
    auto *out = new unsigned char[size_t(ow) * size_t(oh) * size_t(d)];
    const double fx = sww / ow, fy = shh / oh;
    for (int y = 0; y < oh; ++y) {
        double y0 = sy0 + y * fy, y1 = y0 + fy;
        int iy0 = int(y0), iy1 = std::min(sh, std::max(iy0 + 1, int(std::ceil(y1))));
        for (int x = 0; x < ow; ++x) {
            double x0 = sx0 + x * fx, x1 = x0 + fx;
            int ix0 = int(x0), ix1 = std::min(sw, std::max(ix0 + 1, int(std::ceil(x1))));
            unsigned acc[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int yy = iy0; yy < iy1; ++yy) {
                const unsigned char *row = in + size_t(yy) * size_t(ld);
                for (int xx = ix0; xx < ix1; ++xx) {
                    const unsigned char *px = row + size_t(xx) * size_t(d);
                    for (int c = 0; c < d; ++c)
                        acc[c] += px[c];
                    ++n;
                }
            }
            unsigned char *o = out + (size_t(y) * size_t(ow) + size_t(x)) * size_t(d);
            for (int c = 0; c < d; ++c)
                o[c] = static_cast<unsigned char>(n ? acc[c] / n : 0);
        }
    }
    auto *img = new Fl_RGB_Image(out, ow, oh, d);
    img->alloc_array = 1;
    return img;
}

} // namespace flo::ui

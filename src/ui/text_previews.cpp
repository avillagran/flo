#include "ui/text_previews.h"

#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/process.h"
#include "fltk-omarchy/theme.h"

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

namespace flo::ui {

namespace {
constexpr std::size_t kMaxItems = 512;
} // namespace

const std::vector<std::string> *TextPreviews::get(const std::string &path, const Entry &e, Fl_Widget *who) {
    if (!wanted(e))
        return nullptr;
    const std::string key = path + "\x1f" + std::to_string(e.mtime_ns) + "\x1f" + std::to_string(e.size);
    auto it = index_.find(key);
    if (it != index_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        Item &item = *it->second;
        if (item.pending) {
            if (who)
                item.waiters.push_back(std::make_unique<Fl_Widget_Tracker>(who));
            return nullptr;
        }
        return item.none ? nullptr : &item.lines;
    }
    lru_.emplace_front();
    Item &item = lru_.front();
    item.key = key;
    if (who)
        item.waiters.push_back(std::make_unique<Fl_Widget_Tracker>(who));
    index_[key] = lru_.begin();
    while (lru_.size() > kMaxItems) {
        // Drop the oldest that is not being read.
        auto victim = std::prev(lru_.end());
        while (victim != lru_.begin() && victim->pending)
            --victim;
        if (victim->pending)
            break;
        index_.erase(victim->key);
        lru_.erase(victim);
    }
    auto result = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    const text_preview::Cache cache = cache_;
    const int64_t mtime = e.mtime_ns, size = e.size;
    fo::tasks::run([cache, path, mtime, size, result, ok] { *ok = text_preview::fetch(cache, path, mtime, size, *result); },
                   [this, key, result, ok] {
                       auto found = index_.find(key);
                       if (found == index_.end())
                           return;
                       Item &done = *found->second;
                       done.pending = false;
                       done.none = !*ok;
                       if (*ok)
                           done.lines = text_preview::page_lines(*result);
                       for (auto &w : done.waiters)
                           if (!w->deleted())
                               w->widget()->redraw();
                       done.waiters.clear();
                   });
    return nullptr;
}

void draw_text_page(const std::vector<std::string> &lines, int x, int y, int w, int h, bool marked) {
    const fo::Theme &t = fo::theme();
    if (w <= 4 || h <= 4)
        return;
    int ph = h, pw = std::min(w, int(std::lround(h * 0.85)));
    int px = x + (w - pw) / 2, py = y;
    const int r = t.radius() > 0 ? std::min(4, std::max(1, ph / 12)) : 0;
    fo::Rgb paper = t.lighter_background();
    fo::Rgb edge = marked ? t.accent() : fo::mix(paper, t.foreground(), 0.16);
    fo::paint::round_fill(px, py, pw, ph, r, edge);
    fo::paint::round_fill(px + 1, py + 1, pw - 2, ph - 2, r, paper);
    const int margin = std::max(2, int(std::lround(pw * 0.08)));
    fl_push_clip(px + margin, py + 1, pw - 2 * margin, ph - 2);
    if (ph >= 60) {
        // Folio: the monospace face at a size that reads on the tile.
        const int size = std::max(6, ph / 17);
        fl_font(FL_COURIER, size);
        fl_color(t.foreground().fl());
        int ly = py + margin + fl_height() - fl_descent();
        for (const auto &line : lines) {
            if (ly > py + ph - margin / 2)
                break;
            fl_draw(line.c_str(), px + margin, ly);
            ly += fl_height();
        }
    } else {
        // Too small for letters: a stroke per line, as long as the line.
        const int step = std::max(2, ph / 9);
        const int full = pw - 2 * margin;
        fo::Rgb ink = fo::mix(paper, t.foreground(), 0.55);
        int ly = py + margin;
        for (const auto &line : lines) {
            if (ly + 1 > py + ph - margin)
                break;
            size_t lead = line.find_first_not_of(' ');
            if (lead != std::string::npos) {
                int indent = int(std::min<size_t>(lead, 24)) * full / 32;
                int len = std::max(2, int(std::min<size_t>(line.size() - lead, 32)) * full / 32);
                fo::paint::fill(px + margin + indent, ly, std::min(len, full - indent), 1, ink);
            }
            ly += step;
        }
    }
    fl_pop_clip();
}

} // namespace flo::ui

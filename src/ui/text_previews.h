// Text previews in the thumbnail slot (core/text_preview.h): the opening
// lines of text and code files, read off the UI thread (the toolkit's task
// pool), kept on disk in Flo's cache and in memory here (an LRU of 512
// files), and drawn as a small page in the theme's colours by
// draw_text_page(). Binary files and special files get no page and keep
// their icon.
#pragma once

#include "core/entry.h"
#include "core/text_preview.h"

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Fl_Widget;
class Fl_Widget_Tracker;

namespace flo::ui {

class TextPreviews {
public:
    static bool wanted(const Entry &e) { return !e.is_dir && e.kind == Kind::Text; }
    // The page's lines, or nullptr while they are read or when there are
    // none. `who` is redrawn when they arrive.
    const std::vector<std::string> *get(const std::string &path, const Entry &e, Fl_Widget *who);
    std::size_t entries() const { return lru_.size(); }

private:
    struct Item {
        std::string key;
        std::vector<std::string> lines;
        bool none = false, pending = true;
        std::vector<std::unique_ptr<Fl_Widget_Tracker>> waiters;
    };
    std::list<Item> lru_; // front = most recently used
    std::unordered_map<std::string, std::list<Item>::iterator> index_;
    text_preview::Cache cache_;
};

// Folio's text page: a sheet a little narrower than it is tall (0.85),
// centered in the box and as tall as it, in the card colour with a faint
// edge, the lines in the foreground in the monospace face. Too small for
// letters (under 60 px), each line is a short stroke of its length.
void draw_text_page(const std::vector<std::string> &lines, int x, int y, int w, int h, bool marked = false);

} // namespace flo::ui

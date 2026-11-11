// Thumbnails as drawable images, sized for where they are drawn.
//
// flo-core finds or makes the freedesktop thumbnail (a PNG of 128, 256, 512
// or 1024 px); this cache decodes it once, scales it straight to the box it
// is drawn in (a 128x92 tile, a 20 px list icon, the details pane) at the
// screen's real resolution, and keeps only that. The decoded thumbnail is
// dropped at once, and the scaled images are bounded by bytes (LRU), so a
// folder of photos costs a few MiB however many thumbnails it has.
#pragma once

#include "core/entry.h"
#include "core/thumbnail_service.h"

#include <FL/Fl_Image.H>

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Fl_Widget;
class Fl_Widget_Tracker;

namespace flo::ui {

class ThumbImages {
public:
    explicit ThumbImages(ThumbnailService &service, std::size_t max_bytes = 8u << 20);
    ~ThumbImages();

    // Whether thumbnails are worth asking for this entry (not folders, not
    // text, which Folio draws as a page).
    static bool wanted(const Entry &e);

    // The image for `path` fitted (or, with `crop`, filling and cropped) to
    // a box of bw x bh FLTK units, or nullptr while it is being made or when
    // none can be. `request_px` picks the cache bucket (128, 256, 512,
    // 1024). `who` is redrawn when it arrives.
    Fl_RGB_Image *get(const std::string &path, const Entry &e, int request_px, int bw, int bh, bool crop,
                      Fl_Widget *who);
    // True when the thumbnail is known to be impossible (draw a stand-in
    // for good, not just while waiting).
    bool impossible(const std::string &path, const Entry &e, int request_px) const;

    std::size_t bytes() const { return bytes_; }
    std::size_t images() const { return lru_.size(); }
    void clear();

private:
    struct Image {
        std::string key;
        Fl_RGB_Image *img = nullptr;
        std::size_t bytes = 0;
    };
    struct Pending {
        uint64_t ticket = 0;
        double wanted_at = 0;
        std::vector<std::unique_ptr<Fl_Widget_Tracker>> waiters;
    };
    static std::string source_key(const std::string &path, const Entry &e, int request_px);
    void arrived(const std::string &skey, const ThumbnailResult &r);
    Fl_RGB_Image *decode_scaled(const std::string &png, int pw, int ph, bool crop) const;
    void insert(const std::string &key, Fl_RGB_Image *img);
    void sweep();
    static void sweep_cb(void *);

    ThumbnailService &service_;
    std::size_t max_bytes_, bytes_ = 0;
    std::list<Image> lru_; // front = most recently used
    std::unordered_map<std::string, std::list<Image>::iterator> index_;
    // Thumbnails on disk, by source key: their PNG path once known.
    std::unordered_map<std::string, std::string> png_;
    std::unordered_map<std::string, bool> none_; // source key -> no thumbnail possible
    std::unordered_map<std::string, Pending> pending_;
    bool sweeping_ = false;
};

} // namespace flo::ui

// Thumbnails for the UI: cache hits answered at once, everything else made
// by a few flo-thumbnailer worker processes. The UI process never decodes
// anything but the final small PNGs.
#pragma once

#include "core/event_queue.h"
#include "core/thumbnail_cache.h"

#include <functional>
#include <memory>
#include <string>

namespace flo {

struct ThumbnailServiceOptions {
    std::string cache_root;  // default: ThumbnailCache::default_root()
    std::string worker_path; // default: $FLO_THUMBNAILER, else flo-thumbnailer beside the executable, else $PATH
    int workers = 0;         // concurrent worker processes; 0 = 2..4 by CPU count
    int request_timeout_ms = 30000; // a worker taking longer is killed
    int idle_exit_ms = 20000;       // idle workers exit after this long
};

enum class ThumbnailStatus {
    Ready,       // `thumbnail` is a valid PNG in the shared cache
    Failed,      // generation failed (remembered by a failure marker)
    Unsupported, // no decoder or thumbnailer handles this type
    Skipped,     // not a regular file, inside the cache, changed meanwhile
};
const char *thumbnail_status_name(ThumbnailStatus s);

struct ThumbnailResult {
    std::string path;      // the file asked about
    int pixels = 0;        // the size asked for
    ThumbnailStatus status = ThumbnailStatus::Failed;
    std::string thumbnail; // PNG to load when Ready
    int thumbnail_pixels = 0; // its bucket (>= the requested one)
    std::string error;
};

class ThumbnailService {
public:
    explicit ThumbnailService(EventQueue &queue, ThumbnailServiceOptions options = {});
    ~ThumbnailService();
    ThumbnailService(const ThumbnailService &) = delete;
    ThumbnailService &operator=(const ThumbnailService &) = delete;

    using Callback = std::function<void(const ThumbnailResult &)>;
    // Queues a request; the most recent requests are served first (the tiles
    // just scrolled into view). `mime` may be empty. The callback runs on the
    // UI thread unless the ticket was cancelled first.
    uint64_t request(const std::string &path, int pixels, Callback callback, const std::string &mime = {});
    // Forgets a request: dropped if not started, its result ignored if it
    // was (a running worker finishes and caches it anyway).
    void cancel(uint64_t ticket);
    void cancel_all();

    // Synchronous cache lookup (no worker, a stat and a small read).
    bool lookup(const std::string &path, int pixels, ThumbnailCache::Hit &hit) const;
    const ThumbnailCache &cache() const { return cache_; }

    struct Impl;

private:
    ThumbnailCache cache_;
    std::shared_ptr<Impl> impl_;
};

} // namespace flo

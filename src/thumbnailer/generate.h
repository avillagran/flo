// Thumbnail generation inside the worker process.
#pragma once

#include "core/process.h"
#include "core/thumbnail_cache.h"
#include "core/thumbnailers.h"
#include "thumbnailer/decode.h"

#include <string>

namespace flo::thumb {

enum class Outcome {
    Ready,       // `thumbnail` names a valid cached PNG
    Failed,      // a decoder or thumbnailer ran and failed (marker written)
    Unsupported, // nothing can thumbnail this type (no marker)
    Skipped,     // not a regular file, inside the cache, or unreadable
};
const char *outcome_name(Outcome o);

class Generator {
public:
    Generator(ThumbnailCache cache, ThumbnailerRegistry registry);
    ~Generator();

    // Limits and wall timeout for each external thumbnailer run.
    ProcessLimits child_limits;
    int child_timeout_ms = 15000;

    Outcome make(const std::string &path, const std::string &mime_hint, int pixels, std::string &thumbnail,
                 std::string &error);
    // Writes a failure marker for the file's current revision.
    bool mark_failed(const std::string &path);

    const ThumbnailCache &cache() const { return cache_; }

private:
    bool run_external(const ThumbnailerEntry &t, const std::string &path, int pixels, Image &out,
                      std::string &error);
    bool store(const std::string &target, const Image &image, const std::string &path, const struct stat &st);
    bool scratch_dir(std::string &dir);

    ThumbnailCache cache_;
    ThumbnailerRegistry registry_;
    std::string scratch_;
    unsigned counter_ = 0;
};

// Creates `dir` (and missing parents up to the cache root) as 0700 when
// missing; refuses symlinks, directories owned by someone else and
// group/world-writable ones.
bool ensure_private_dir(const std::string &dir);

} // namespace flo::thumb

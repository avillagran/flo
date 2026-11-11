// The freedesktop shared thumbnail cache (Thumbnail Managing Standard),
// read as untrusted input. Pure: no decoding, no inflating; it walks PNG
// chunk headers and the plain tEXt chunks only.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <sys/stat.h>

namespace flo {

class ThumbnailCache {
public:
    // `root` is the cache directory ($XDG_CACHE_HOME/thumbnails by default);
    // tests inject one inside their fixture.
    explicit ThumbnailCache(std::string root = default_root());
    static std::string default_root();
    const std::string &root() const { return root_; }

    // The bucket for a requested size: normal 128, large 256, x-large 512,
    // xx-large 1024 (anything bigger also xx-large).
    static const char *bucket_for(int pixels);
    static int bucket_pixels(const char *bucket); // 0 for unknown

    // MD5 of the file URI in lower-case hex, plus ".png".
    static std::string key_for_uri(const std::string &uri);
    std::string path_for_uri(const std::string &uri, int pixels) const;
    // Flo's own failure markers: <root>/fail/flo/<md5>.png
    std::string fail_path_for_uri(const std::string &uri) const;

    // Whether `path` lies inside the cache (lexically, or through the real
    // path of its folder): thumbnails are never made of thumbnails.
    bool inside_cache(const std::string &path) const;

    struct Hit {
        std::string path; // the thumbnail PNG to load
        int pixels = 0;   // its bucket size
    };
    // Finds a valid thumbnail for `file` (absolute path, `st` its stat):
    // the requested bucket first, then bigger ones (a bigger thumbnail can
    // be scaled down). Thumbnails written by any application count.
    bool lookup(const std::string &file, const struct stat &st, int pixels, Hit &hit) const;
    // A valid failure marker for this file revision exists.
    bool failed_before(const std::string &file, const struct stat &st) const;

private:
    std::string root_;
};

struct PngMeta {
    int width = 0, height = 0;
    std::map<std::string, std::string> text; // tEXt keyword -> value
};

// Reads PNG metadata from an untrusted file: refuses symlinks, non-regular
// files, files not owned by the user, files over max_bytes; requires the
// signature, a first 13-byte IHDR with 1 <= width, height <= max_dim, at
// most 4096 chunks and a reachable IEND; reads only tEXt chunks of at most
// 16 KiB with a valid CRC; a repeated Thumb:: key rejects the file.
bool read_png_meta(const std::string &file, std::size_t max_bytes, int max_dim, PngMeta &out);
// The same over bytes already in memory (no ownership checks).
bool parse_png_meta(const std::string &bytes, int max_dim, PngMeta &out);

// Whether metadata describes this exact file revision: Thumb::URI equals
// `uri`, Thumb::MTime equals the mtime in whole seconds, and Thumb::Size,
// when present, equals the size.
bool thumbnail_matches(const PngMeta &meta, const std::string &uri, const struct stat &st);

// CRC-32 (PNG/zlib polynomial).
uint32_t crc32_update(uint32_t crc, const unsigned char *data, std::size_t len);

} // namespace flo

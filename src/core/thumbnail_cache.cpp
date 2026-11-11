#include "core/thumbnail_cache.h"
#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <cstdint>
#include <cstring>
#include <glib.h>
#include <unistd.h>

namespace flo {

ThumbnailCache::ThumbnailCache(std::string root) : root_(clean_path(root)) {}

std::string ThumbnailCache::default_root() { return xdg::cache_home() + "/thumbnails"; }

const char *ThumbnailCache::bucket_for(int pixels) {
    if (pixels <= 128) return "normal";
    if (pixels <= 256) return "large";
    if (pixels <= 512) return "x-large";
    return "xx-large";
}

int ThumbnailCache::bucket_pixels(const char *bucket) {
    if (!std::strcmp(bucket, "normal")) return 128;
    if (!std::strcmp(bucket, "large")) return 256;
    if (!std::strcmp(bucket, "x-large")) return 512;
    if (!std::strcmp(bucket, "xx-large")) return 1024;
    return 0;
}

std::string ThumbnailCache::key_for_uri(const std::string &uri) {
    gchar *hex = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), static_cast<gssize>(uri.size()));
    std::string out = std::string(hex) + ".png";
    g_free(hex);
    return out;
}

std::string ThumbnailCache::path_for_uri(const std::string &uri, int pixels) const {
    return root_ + "/" + bucket_for(pixels) + "/" + key_for_uri(uri);
}

std::string ThumbnailCache::fail_path_for_uri(const std::string &uri) const {
    return root_ + "/fail/flo/" + key_for_uri(uri);
}

bool ThumbnailCache::inside_cache(const std::string &path) const {
    if (path_inside(path, root_))
        return true;
    const std::string real_root = real_path(root_);
    const std::string real_parent = real_path(path_dirname(path));
    return !real_root.empty() && !real_parent.empty() && path_inside(real_parent, real_root);
}

static std::size_t cap_for(int pixels) {
    // An RGBA thumbnail compresses well below width*height*4; these caps
    // bound what is read without rejecting real thumbnails.
    if (pixels <= 128) return 1u << 20;
    if (pixels <= 256) return 2u << 20;
    if (pixels <= 512) return 8u << 20;
    return 16u << 20;
}

bool ThumbnailCache::lookup(const std::string &file, const struct stat &st, int pixels, Hit &hit) const {
    const std::string uri = path_to_uri(file);
    static const int sizes[] = {128, 256, 512, 1024};
    const int want = bucket_pixels(bucket_for(pixels));
    for (int px : sizes) {
        if (px < want)
            continue;
        const std::string candidate = path_for_uri(uri, px);
        PngMeta meta;
        if (read_png_meta(candidate, cap_for(px), px, meta) && thumbnail_matches(meta, uri, st)) {
            hit.path = candidate;
            hit.pixels = px;
            return true;
        }
    }
    return false;
}

bool ThumbnailCache::failed_before(const std::string &file, const struct stat &st) const {
    const std::string uri = path_to_uri(file);
    PngMeta meta;
    return read_png_meta(fail_path_for_uri(uri), 64 * 1024, 1024, meta) && thumbnail_matches(meta, uri, st);
}

// ---------------------------------------------------------------------------

uint32_t crc32_update(uint32_t crc, const unsigned char *data, std::size_t len) {
    struct Table {
        uint32_t v[256];
        Table() {
            for (uint32_t n = 0; n < 256; ++n) {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                v[n] = c;
            }
        }
    };
    static const Table t; // thread-safe initialization
    const uint32_t *table = t.v;
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t be32(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool parse_png_meta(const std::string &bytes, int max_dim, PngMeta &out) {
    out = PngMeta();
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.data());
    const std::size_t n = bytes.size();
    if (n < 8 + 25 || std::memcmp(p, sig, 8) != 0)
        return false;
    std::size_t at = 8;
    int chunks = 0;
    bool seen_end = false;
    while (at + 12 <= n) {
        if (++chunks > 4096)
            return false;
        const uint32_t len = be32(p + at);
        if (len > 0x7FFFFFFFu || at + 12 + len > n)
            return false;
        const unsigned char *type = p + at + 4;
        const unsigned char *body = p + at + 8;
        if (chunks == 1) {
            if (std::memcmp(type, "IHDR", 4) != 0 || len != 13)
                return false;
            const uint32_t w = be32(body), h = be32(body + 4);
            if (w < 1 || h < 1 || w > static_cast<uint32_t>(max_dim) || h > static_cast<uint32_t>(max_dim))
                return false;
            out.width = static_cast<int>(w);
            out.height = static_cast<int>(h);
        } else if (std::memcmp(type, "tEXt", 4) == 0 && len <= 16384) {
            const uint32_t crc = crc32_update(0, type, 4 + len);
            if (crc != be32(body + len))
                return false;
            const void *nul = std::memchr(body, 0, len);
            if (nul) {
                const std::size_t klen = static_cast<const unsigned char *>(nul) - body;
                std::string key(reinterpret_cast<const char *>(body), klen);
                std::string value(reinterpret_cast<const char *>(body) + klen + 1, len - klen - 1);
                if (key.compare(0, 7, "Thumb::") == 0 && out.text.count(key))
                    return false;
                out.text.emplace(std::move(key), std::move(value));
            }
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            seen_end = true;
            break;
        }
        at += 12 + len;
    }
    return seen_end && out.width > 0;
}

bool read_png_meta(const std::string &file, std::size_t max_bytes, int max_dim, PngMeta &out) {
    struct stat st;
    if (::lstat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() ||
        static_cast<uint64_t>(st.st_size) > max_bytes)
        return false;
    std::string bytes;
    if (!read_file_capped(file, max_bytes, bytes, false))
        return false;
    return parse_png_meta(bytes, max_dim, out);
}

bool thumbnail_matches(const PngMeta &meta, const std::string &uri, const struct stat &st) {
    auto it = meta.text.find("Thumb::URI");
    if (it == meta.text.end() || it->second != uri)
        return false;
    it = meta.text.find("Thumb::MTime");
    if (it == meta.text.end() || it->second != std::to_string(static_cast<long long>(st.st_mtim.tv_sec)))
        return false;
    it = meta.text.find("Thumb::Size");
    if (it != meta.text.end() && it->second != std::to_string(static_cast<long long>(st.st_size)))
        return false;
    return true;
}

} // namespace flo

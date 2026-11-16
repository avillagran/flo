// Text previews: the opening lines of a text or code file, drawn as a small
// page in the thumbnail slot (after Folio's text pages). Only a capped
// prefix is read, only from regular files, and anything that looks binary
// (a NUL byte, or bytes that are not UTF-8 before the cut) has no preview.
//
// Previews are cached like thumbnails, in Flo's own cache rather than the
// shared freedesktop one (the page is drawn in the theme's colours, so no
// picture is stored): $XDG_CACHE_HOME/flo/text-previews/<md5 of the URI>.txt,
// 0600 in a 0700 folder, holding the file's modification time and size
// (checked as Thumb::MTime and Thumb::Size are) and the prefix. A changed
// file is read again; a network location is read once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flo::text_preview {

constexpr std::size_t kPrefix = 4096;

// Reads up to `max` bytes of a regular file into `out` as text. False for
// binary data, special files (a FIFO is never waited on) and errors.
bool read_prefix(const std::string &path, std::string &out, std::size_t max = kPrefix);
// True when `bytes` reads as text: no NUL, and valid UTF-8 except for a
// sequence cut at the very end when `cut` (the file goes on).
bool looks_like_text(const std::string &bytes, bool cut);
// The lines a page shows: CRLF and CR end lines, tabs become four spaces,
// at most `max_lines` lines of at most `max_cols` code points.
std::vector<std::string> page_lines(const std::string &text, int max_lines = 40, int max_cols = 96);

class Cache {
public:
    explicit Cache(std::string root = default_root());
    static std::string default_root(); // $XDG_CACHE_HOME/flo/text-previews
    const std::string &root() const { return root_; }
    std::string path_for(const std::string &file) const;
    // The cached prefix when it matches the file's mtime and size.
    bool load(const std::string &file, int64_t mtime_ns, int64_t size, std::string &text) const;
    void store(const std::string &file, int64_t mtime_ns, int64_t size, const std::string &text) const;

private:
    std::string root_;
};

// For a worker: the prefix from the cache when valid, else read from the
// file (and cached). False when the file has no text preview.
bool fetch(const Cache &cache, const std::string &file, int64_t mtime_ns, int64_t size, std::string &text);

} // namespace flo::text_preview

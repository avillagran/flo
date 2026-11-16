#include "core/text_preview.h"

#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <glib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace flo::text_preview {

bool looks_like_text(const std::string &b, bool cut) {
    size_t i = 0, n = b.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(b[i]);
        if (c == 0)
            return false;
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (!len)
            return false;
        if (i + size_t(len) > n)
            return cut; // a character cut by the end of the prefix
        for (int k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(b[i + size_t(k)]) & 0xC0) != 0x80)
                return false;
        // Control characters other than tab, newlines and form feed say binary.
        if (len == 1 && c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != 0x1b)
            return false;
        i += size_t(len);
    }
    return true;
}

bool read_prefix(const std::string &path, std::string &out, std::size_t max) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }
    out.resize(max);
    size_t got = 0;
    while (got < max) {
        ssize_t r = ::read(fd, &out[got], max - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += size_t(r);
    }
    ::close(fd);
    out.resize(got);
    const bool cut = static_cast<uint64_t>(st.st_size) > got;
    if (!looks_like_text(out, cut)) {
        out.clear();
        return false;
    }
    return true;
}

std::vector<std::string> page_lines(const std::string &text, int max_lines, int max_cols) {
    std::vector<std::string> lines;
    std::string cur;
    int cols = 0;
    bool full = false;
    auto end_line = [&] {
        lines.push_back(cur);
        cur.clear();
        cols = 0;
        full = false;
    };
    for (size_t i = 0; i < text.size() && int(lines.size()) < max_lines; ++i) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            end_line();
            continue;
        }
        if (c == '\n') {
            end_line();
            continue;
        }
        if (full)
            continue;
        if (c == '\t') {
            for (int k = 0; k < 4 && cols < max_cols; ++k, ++cols)
                cur += ' ';
        } else {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
                ++cols; // a code point starts
            if (cols > max_cols) {
                full = true;
                continue;
            }
            cur += c;
        }
        if (cols >= max_cols)
            full = true;
    }
    if (!cur.empty() && int(lines.size()) < max_lines)
        lines.push_back(cur);
    return lines;
}

// --- The cache ------------------------------------------------------------------

Cache::Cache(std::string root) : root_(std::move(root)) {}

std::string Cache::default_root() { return path_join(xdg::cache_home(), "flo/text-previews"); }

std::string Cache::path_for(const std::string &file) const {
    const std::string uri = path_to_uri(file);
    gchar *hex = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), static_cast<gssize>(uri.size()));
    std::string out = path_join(root_, std::string(hex) + ".txt");
    g_free(hex);
    return out;
}

static std::string header(int64_t mtime_ns, int64_t size) {
    return "flo-text-preview 1 " + std::to_string(mtime_ns) + " " + std::to_string(size) + "\n";
}

bool Cache::load(const std::string &file, int64_t mtime_ns, int64_t size, std::string &text) const {
    std::string data;
    if (!read_file_capped(path_for(file), kPrefix + 128, data, false))
        return false;
    const std::string h = header(mtime_ns, size);
    if (data.compare(0, h.size(), h) != 0)
        return false;
    text = data.substr(h.size());
    return true;
}

void Cache::store(const std::string &file, int64_t mtime_ns, int64_t size, const std::string &text) const {
    if (!make_dirs(root_, 0700))
        return;
    atomic_write_file(path_for(file), header(mtime_ns, size) + text.substr(0, kPrefix), 0600);
}

bool fetch(const Cache &cache, const std::string &file, int64_t mtime_ns, int64_t size, std::string &text) {
    if (cache.load(file, mtime_ns, size, text))
        return true;
    if (!read_prefix(file, text))
        return false;
    cache.store(file, mtime_ns, size, text);
    return true;
}

} // namespace flo::text_preview

#include "core/entry.h"
#include "core/fsutil.h"
#include "core/natural_sort.h"

#include <algorithm>
#include <cstring>

#include <fcntl.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo {

const char *kind_name(Kind k) {
    switch (k) {
    case Kind::Folder: return "folder";
    case Kind::Pdf: return "pdf";
    case Kind::Image: return "image";
    case Kind::Video: return "video";
    case Kind::Audio: return "audio";
    case Kind::Text: return "text";
    case Kind::Archive: return "archive";
    case Kind::Other: break;
    }
    return "other";
}

static bool starts_with(const char *s, const char *prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

Kind kind_for_mime(const char *mime) {
    if (!mime)
        return Kind::Other;
    if (!std::strcmp(mime, "inode/directory")) return Kind::Folder;
    if (!std::strcmp(mime, "application/pdf")) return Kind::Pdf;
    if (starts_with(mime, "image/")) return Kind::Image;
    if (starts_with(mime, "video/")) return Kind::Video;
    if (starts_with(mime, "audio/")) return Kind::Audio;
    static const char *const text[] = {"application/json", "application/x-shellscript", "application/toml",
                                       "application/xml", "application/x-yaml"};
    if (starts_with(mime, "text/"))
        return Kind::Text;
    for (const char *t : text)
        if (!std::strcmp(mime, t))
            return Kind::Text;
    static const char *const archives[] = {
        "application/zip", "application/x-tar", "application/gzip", "application/x-xz", "application/zstd",
        "application/x-7z-compressed", "application/x-compressed-tar", "application/x-zstd-compressed-tar",
        "application/vnd.rar", "application/x-iso9660-image", "application/x-bzip2-compressed-tar",
        "application/x-xz-compressed-tar"};
    for (const char *a : archives)
        if (!std::strcmp(mime, a))
            return Kind::Archive;
    return Kind::Other;
}

const char *sort_mode_name(SortMode m) {
    switch (m) {
    case SortMode::Name: return "name";
    case SortMode::Kind: return "kind";
    case SortMode::Newest: return "modified";
    case SortMode::Oldest: return "oldest";
    case SortMode::Size: return "size";
    }
    return "name";
}

bool parse_sort_mode(const std::string &s, SortMode &out) {
    if (s == "name") out = SortMode::Name;
    else if (s == "kind") out = SortMode::Kind;
    else if (s == "modified" || s == "newest") out = SortMode::Newest;
    else if (s == "oldest") out = SortMode::Oldest;
    else if (s == "size") out = SortMode::Size;
    else return false;
    return true;
}

std::string Entry::extension() const {
    if (is_dir)
        return {};
    const std::string base = path_basename(name);
    const std::size_t dot = extension_start(base, false);
    if (dot == std::string::npos)
        return {};
    std::string ext = base.substr(dot + 1);
    for (char &c : ext)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 32);
    return ext;
}

const char *mime_for_name(const std::string &name, bool *uncertain) {
    gboolean unsure = FALSE;
    gchar *type = g_content_type_guess(name.c_str(), nullptr, 0, &unsure);
    const char *interned = g_intern_string(type ? type : "application/octet-stream");
    g_free(type);
    if (uncertain)
        *uncertain = unsure;
    return interned;
}

const char *mime_for_file(int dirfd, const std::string &name, const struct stat &st, bool sniff) {
    if (S_ISDIR(st.st_mode))
        return g_intern_static_string("inode/directory");
    if (!S_ISREG(st.st_mode)) {
        if (S_ISCHR(st.st_mode)) return g_intern_static_string("inode/chardevice");
        if (S_ISBLK(st.st_mode)) return g_intern_static_string("inode/blockdevice");
        if (S_ISFIFO(st.st_mode)) return g_intern_static_string("inode/fifo");
        if (S_ISSOCK(st.st_mode)) return g_intern_static_string("inode/socket");
        return g_intern_static_string("application/octet-stream");
    }
    bool uncertain = false;
    const char *by_name = mime_for_name(name, &uncertain);
    if (st.st_size == 0)
        return std::strcmp(by_name, "application/octet-stream") == 0 ? g_intern_static_string("application/x-zerosize")
                                                                     : by_name;
    if (!uncertain || !sniff)
        return by_name;
    // Unknown by name (no or an unknown extension): look at the first bytes,
    // without following links and without blocking on a FIFO swapped in.
    const int fd = ::openat(dirfd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
    if (fd < 0)
        return by_name;
    unsigned char buf[1024];
    struct stat check;
    ssize_t n = -1;
    if (::fstat(fd, &check) == 0 && S_ISREG(check.st_mode))
        n = ::read(fd, buf, sizeof buf);
    ::close(fd);
    if (n <= 0)
        return by_name;
    gboolean unsure = FALSE;
    gchar *type = g_content_type_guess(name.c_str(), buf, static_cast<gsize>(n), &unsure);
    const char *interned = g_intern_string(type ? type : by_name);
    g_free(type);
    return interned;
}

bool stat_entry(int dirfd, const std::string &name, Entry &e, const EntryOptions &opt) {
    struct stat st;
    if (::fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
        return false;
    e.name = name;
    e.mode = st.st_mode;
    e.hidden = !name.empty() && name[0] == '.';
    e.is_link = S_ISLNK(st.st_mode);
    struct stat target = st;
    if (e.is_link) {
        char buf[4096];
        const ssize_t n = ::readlinkat(dirfd, name.c_str(), buf, sizeof buf - 1);
        if (n >= 0)
            e.link_target.assign(buf, static_cast<std::size_t>(n));
        if (::fstatat(dirfd, name.c_str(), &target, 0) != 0) {
            e.broken_link = true;
            target = st;
        }
    }
    e.is_dir = S_ISDIR(target.st_mode);
    e.size = e.is_dir ? 0 : static_cast<int64_t>(target.st_size);
    e.mtime_ns = timespec_ns(target.st_mtim);
    if (e.broken_link)
        e.mime = g_intern_static_string("inode/symlink");
    else
        e.mime = mime_for_file(dirfd, name, target, opt.sniff && !e.is_link);
    e.kind = kind_for_mime(e.mime);
    return true;
}

bool stat_path(const std::string &path, Entry &e, const EntryOptions &opt) {
    const std::string dir = path_dirname(path);
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = stat_entry(fd, path_basename(path), e, opt);
    ::close(fd);
    return ok;
}

bool entry_less(const Entry &a, const Entry &b, SortMode mode) {
    // Folders first in every order, as everywhere else on the desktop.
    if (a.is_dir != b.is_dir)
        return a.is_dir;
    switch (mode) {
    case SortMode::Newest:
        if (a.mtime_ns != b.mtime_ns)
            return a.mtime_ns > b.mtime_ns;
        break;
    case SortMode::Oldest:
        if (a.mtime_ns != b.mtime_ns)
            return a.mtime_ns < b.mtime_ns;
        break;
    case SortMode::Size:
        // Biggest first; folders have no size of their own and stay by name.
        if (!a.is_dir && a.size != b.size)
            return a.size > b.size;
        break;
    case SortMode::Kind:
        if (!a.is_dir) {
            if (a.kind != b.kind)
                return std::strcmp(kind_name(a.kind), kind_name(b.kind)) < 0;
            const std::string ea = a.extension(), eb = b.extension();
            if (ea != eb)
                return natural_compare(ea, eb) < 0;
        }
        break;
    case SortMode::Name:
        break;
    }
    return natural_compare(a.name, b.name) < 0;
}

void sort_entries(std::vector<Entry> &entries, SortMode mode) {
    std::stable_sort(entries.begin(), entries.end(),
                     [mode](const Entry &a, const Entry &b) { return entry_less(a, b, mode); });
}

} // namespace flo

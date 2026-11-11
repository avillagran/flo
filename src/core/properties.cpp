#include "core/properties.h"

#include "core/entry.h"
#include "core/fsutil.h"

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <vector>

#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452 // the same number on every Linux architecture
#endif

namespace flo::props {

namespace {

Timestamp stamp(const struct timespec &ts) { return {int64_t(ts.tv_sec), int64_t(ts.tv_nsec), true}; }

std::string user_name(uid_t uid) {
    std::vector<char> buf(16384);
    struct passwd pw, *res = nullptr;
    if (::getpwuid_r(uid, &pw, buf.data(), buf.size(), &res) == 0 && res && res->pw_name)
        return res->pw_name;
    return std::to_string(uid);
}

std::string group_name(gid_t gid) {
    std::vector<char> buf(16384);
    struct group gr, *res = nullptr;
    if (::getgrgid_r(gid, &gr, buf.data(), buf.size(), &res) == 0 && res && res->gr_name)
        return res->gr_name;
    return std::to_string(gid);
}

void fill_from_stat(const struct stat &st, Info &out) {
    if (S_ISLNK(st.st_mode))
        out.kind = Kind::Symlink;
    else if (S_ISDIR(st.st_mode))
        out.kind = Kind::Directory;
    else if (S_ISREG(st.st_mode))
        out.kind = Kind::File;
    else
        out.kind = Kind::Other;
    out.apparent = uint64_t(st.st_size);
    out.allocated = uint64_t(st.st_blocks) * 512;
    out.hardlinks = uint64_t(st.st_nlink);
    out.uid = st.st_uid;
    out.gid = st.st_gid;
    out.mode = uint32_t(st.st_mode) & 07777;
    out.device = uint64_t(st.st_dev);
    out.inode = uint64_t(st.st_ino);
    out.modified = stamp(st.st_mtim);
    out.accessed = stamp(st.st_atim);
    out.changed = stamp(st.st_ctim);
}

std::string errno_text(int e) { return std::strerror(e); }

} // namespace

std::string read(const std::string &path_in, Info &out) {
    out = Info();
    if (path_in.empty() || path_in[0] != '/')
        return "Only local paths have properties.";
    const std::string path = clean_path(path_in);
    out.path = path;
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0)
        return errno_text(errno);
    fill_from_stat(st, out);
    out.owner = user_name(st.st_uid);
    out.group = group_name(st.st_gid);
    struct statx sx;
    if (::statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_BTIME, &sx) == 0 &&
        (sx.stx_mask & STATX_BTIME))
        out.birth = {int64_t(sx.stx_btime.tv_sec), int64_t(sx.stx_btime.tv_nsec), true};
    if (out.kind == Kind::Symlink) {
        char buf[4096];
        ssize_t n = ::readlink(path.c_str(), buf, sizeof buf);
        if (n > 0)
            out.link_target.assign(buf, size_t(n));
        struct stat target;
        out.link_resolves = ::stat(path.c_str(), &target) == 0;
    } else if (out.kind == Kind::File) {
        const std::string name = path_basename(path);
        int dir = ::open(path_dirname(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const char *mime = dir >= 0 ? mime_for_file(dir, name, st, true) : mime_for_name(name);
        if (dir >= 0)
            ::close(dir);
        out.mime = mime ? mime : "";
    }
    // The volume: the link itself lives in its folder's filesystem.
    const std::string where = out.kind == Kind::Symlink ? path_dirname(path) : path;
    struct statvfs sv;
    if (::statvfs(where.c_str(), &sv) == 0) {
        out.volume = true;
        out.free_bytes = uint64_t(sv.f_bavail) * sv.f_frsize;
        out.total_bytes = uint64_t(sv.f_blocks) * sv.f_frsize;
        std::string best;
        for (const auto &m : read_mountinfo())
            if (path_inside(where, m.root_path) && m.root_path.size() >= best.size()) {
                best = m.root_path;
                out.fs_type = m.fs_type;
            }
    }
    return {};
}

bool editable(const Info &info) {
    return (info.kind == Kind::File || info.kind == Kind::Directory) && (info.mode & 07000) == 0 &&
           info.uid == uint32_t(::geteuid());
}

std::string apply_mode(const Info &observed, uint32_t mode, Info &after) {
    if (!editable(observed))
        return "Only your own files and folders without special bits can change here.";
    mode &= 0777;
    int fd = ::open(observed.path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno_text(errno);
    struct stat st;
    std::string why;
    if (::fstat(fd, &st) != 0) {
        why = errno_text(errno);
    } else if (uint64_t(st.st_dev) != observed.device || uint64_t(st.st_ino) != observed.inode ||
               st.st_uid != observed.uid || (uint32_t(st.st_mode) & 07777) != observed.mode ||
               !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
        why = "The object changed since it was read; reload before applying.";
    } else {
        int r = int(::syscall(SYS_fchmodat2, fd, "", mode, AT_EMPTY_PATH));
        if (r != 0 && (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
            // Older kernels: the descriptor's /proc link names this very inode
            // (it was checked above to be a file or folder, not a link).
            std::string proc = "/proc/self/fd/" + std::to_string(fd);
            r = ::chmod(proc.c_str(), mode);
        }
        if (r != 0)
            why = errno_text(errno);
    }
    ::close(fd);
    if (!why.empty())
        return why;
    if (!read(observed.path, after).empty() || after.inode != observed.inode || after.device != observed.device)
        after = Info();
    return {};
}

std::string symbolic_mode(uint32_t mode, char kind) {
    std::string s(10, '-');
    s[0] = kind;
    const char rwx[] = "rwx";
    for (int i = 0; i < 9; ++i)
        if (mode & (0400u >> i))
            s[size_t(1 + i)] = rwx[i % 3];
    if (mode & 04000)
        s[3] = (mode & 0100) ? 's' : 'S';
    if (mode & 02000)
        s[6] = (mode & 0010) ? 's' : 'S';
    if (mode & 01000)
        s[9] = (mode & 0001) ? 't' : 'T';
    return s;
}

std::string format_time(const Timestamp &t) {
    if (!t.known)
        return {};
    time_t secs = time_t(t.sec);
    struct tm tm;
    if (!::localtime_r(&secs, &tm))
        return std::to_string(t.sec) + " s Unix";
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

Census census(const std::vector<std::string> &roots, const std::atomic<bool> &cancel, uint64_t max_entries) {
    Census c;
    uint64_t seen = 0;
    const bool selection = roots.size() > 1;
    for (const auto &root : roots) {
        struct stat st;
        if (::lstat(root.c_str(), &st) != 0) {
            ++c.unreadable;
            c.complete = false;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (S_ISLNK(st.st_mode))
                ++c.links;
            else
                ++c.files, c.bytes += uint64_t(st.st_size);
            continue;
        }
        if (selection)
            ++c.folders;
        const dev_t dev = st.st_dev;
        // An explicit stack of open folders: no recursion, links never walked.
        std::vector<DIR *> stack;
        int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *d = fd >= 0 ? ::fdopendir(fd) : nullptr;
        if (!d) {
            if (fd >= 0)
                ::close(fd);
            ++c.unreadable;
            c.complete = false;
            continue;
        }
        stack.push_back(d);
        while (!stack.empty()) {
            if (cancel.load(std::memory_order_relaxed) || seen >= max_entries) {
                c.complete = false;
                for (DIR *open : stack)
                    ::closedir(open);
                return c;
            }
            DIR *top = stack.back();
            errno = 0;
            struct dirent *de = ::readdir(top);
            if (!de) {
                ::closedir(top);
                stack.pop_back();
                continue;
            }
            if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, ".."))
                continue;
            ++seen;
            struct stat es;
            if (::fstatat(::dirfd(top), de->d_name, &es, AT_SYMLINK_NOFOLLOW) != 0) {
                ++c.unreadable;
                c.complete = false;
                continue;
            }
            if (S_ISLNK(es.st_mode)) {
                ++c.links;
            } else if (S_ISDIR(es.st_mode)) {
                ++c.folders;
                if (es.st_dev != dev)
                    continue; // another filesystem: counted, not entered
                int cfd = ::openat(::dirfd(top), de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                DIR *child = cfd >= 0 ? ::fdopendir(cfd) : nullptr;
                if (!child) {
                    if (cfd >= 0)
                        ::close(cfd);
                    ++c.unreadable;
                    c.complete = false;
                    continue;
                }
                stack.push_back(child);
            } else {
                ++c.files;
                c.bytes += uint64_t(es.st_size);
            }
        }
    }
    return c;
}

} // namespace flo::props

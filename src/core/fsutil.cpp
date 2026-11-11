#include "core/fsutil.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/syscall.h>

namespace flo {

std::string path_join(const std::string &dir, const std::string &name) {
    if (dir.empty())
        return name;
    if (dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}

std::string path_dirname(const std::string &path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    const auto slash = p.rfind('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return p.substr(0, slash);
}

std::string path_basename(const std::string &path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string clean_path(const std::string &path) {
    if (path.empty())
        return path;
    const bool absolute = path[0] == '/';
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        std::string part = path.substr(i, j - i);
        if (part.empty() || part == ".") {
        } else if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(part);
        } else {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (k)
            out += '/';
        out += parts[k];
    }
    if (out.empty())
        out = ".";
    return out;
}

bool path_inside(const std::string &path, const std::string &root) {
    const std::string p = clean_path(path), r = clean_path(root);
    if (p == r)
        return true;
    if (r == "/")
        return !p.empty() && p[0] == '/';
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/';
}

std::string absolute_path(const std::string &path) {
    if (!path.empty() && path[0] == '/')
        return clean_path(path);
    char buf[PATH_MAX];
    if (!::getcwd(buf, sizeof buf))
        return clean_path(path);
    return clean_path(path_join(buf, path));
}

std::string real_path(const std::string &path) {
    char *r = ::realpath(path.c_str(), nullptr);
    if (!r)
        return {};
    std::string out = r;
    std::free(r);
    return out;
}

bool lstat_id(const std::string &path, FileId &id) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0)
        return false;
    id.dev = st.st_dev;
    id.ino = st.st_ino;
    id.mode = st.st_mode;
    return true;
}

bool exists_nofollow(const std::string &path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_dir_nofollow(const std::string &path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::size_t extension_start(const std::string &name, bool dir) {
    if (dir)
        return std::string::npos;
    std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return std::string::npos;
    // "photos.tar.gz" keeps ".tar.gz" together, as Folio does.
    if (dot > 4) {
        std::string before = name.substr(dot - 4, 4);
        for (char &c : before)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (before == ".tar")
            dot -= 4;
    }
    return dot;
}

std::string copy_name(const std::string &name, bool dir, int n) {
    if (n <= 0)
        return name;
    const std::size_t dot = extension_start(name, dir);
    const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
    if (n == 1)
        return base + " (copy)" + ext;
    return base + " (copy " + std::to_string(n) + ")" + ext;
}

std::string free_name(const std::string &folder, const std::string &name) {
    const std::string first = path_join(folder, name);
    if (!exists_nofollow(first))
        return name;
    const bool dir = is_dir_nofollow(first);
    for (int n = 1;; ++n) {
        const std::string candidate = copy_name(name, dir, n);
        if (!exists_nofollow(path_join(folder, candidate)))
            return candidate;
    }
}

int rename_noreplace_at(int from_dir, const char *from, int to_dir, const char *to) {
#ifdef SYS_renameat2
    long r = ::syscall(SYS_renameat2, from_dir, from, to_dir, to, RENAME_NOREPLACE);
    if (r == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return -1;
#endif
    // Filesystems without RENAME_NOREPLACE (some FUSE, older kernels): a hard
    // link also refuses an existing name. Directories can't be linked; for
    // them the check-then-rename below is the best available, and racy.
    if (::linkat(from_dir, from, to_dir, to, 0) == 0) {
        if (::unlinkat(from_dir, from, 0) == 0)
            return 0;
        const int saved = errno;
        ::unlinkat(to_dir, to, 0);
        errno = saved;
        return -1;
    }
    if (errno == EEXIST)
        return -1;
    struct stat st;
    if (::fstatat(to_dir, to, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        return -1;
    }
    return ::renameat(from_dir, from, to_dir, to);
}

int rename_noreplace(const std::string &from, const std::string &to) {
    return rename_noreplace_at(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str());
}

bool make_dirs(const std::string &path, mode_t mode) {
    if (path.empty())
        return false;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    const std::string parent = path_dirname(path);
    if (parent != path && !make_dirs(parent, mode))
        return false;
    if (::mkdir(path.c_str(), mode) == 0)
        return true;
    return errno == EEXIST && is_dir(path);
}

bool atomic_write_file(const std::string &path, const std::string &data, mode_t mode) {
    static std::atomic<unsigned> counter{0};
    const std::string dir = path_dirname(path);
    const std::string tmp = path_join(dir, "." + path_basename(path) + ".flo-" + std::to_string(::getpid()) +
                                               "-" + std::to_string(counter++) + ".tmp");
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    bool ok = ::fchmod(fd, mode) == 0;
    std::size_t done = 0;
    while (ok && done < data.size()) {
        const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            ok = false;
        else
            done += static_cast<std::size_t>(w);
    }
    if (ok)
        ok = ::fsync(fd) == 0;
    if (::close(fd) != 0)
        ok = false;
    if (ok)
        ok = ::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok)
        ::unlink(tmp.c_str()); // only the temporary file this call created
    return ok;
}

bool read_file_capped(const std::string &path, std::size_t max_bytes, std::string &out, bool follow) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (follow ? 0 : O_NOFOLLOW));
    if (fd < 0)
        return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) > max_bytes) {
        ::close(fd);
        return false;
    }
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t r = ::read(fd, &out[done], out.size() - done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        done += static_cast<std::size_t>(r);
    }
    out.resize(done);
    ::close(fd);
    return true;
}

mode_t current_umask() {
    std::string status;
    FILE *f = std::fopen("/proc/self/status", "re");
    if (f) {
        char line[256];
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, "Umask:", 6) == 0) {
                std::fclose(f);
                return static_cast<mode_t>(std::strtoul(line + 6, nullptr, 8));
            }
        std::fclose(f);
    }
    return 022;
}

int64_t timespec_ns(const struct timespec &ts) {
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

dev_t device_of(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? st.st_dev : 0;
}

// mountinfo escapes space, tab, newline and backslash as \ooo.
static std::string unescape_mount(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3' &&
            s[i + 2] >= '0' && s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7') {
            out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::vector<MountInfo> parse_mountinfo(const std::string &text) {
    std::vector<MountInfo> rows;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // id parent major:minor root mountpoint options [optional...] - type source superopts
        std::istringstream fields(line);
        std::vector<std::string> f;
        std::string w;
        while (fields >> w)
            f.push_back(w);
        std::size_t dash = 0;
        for (std::size_t i = 6; i < f.size(); ++i)
            if (f[i] == "-") {
                dash = i;
                break;
            }
        if (f.size() < 7 || dash == 0 || dash + 2 >= f.size())
            continue;
        MountInfo m;
        unsigned maj = 0, min = 0;
        if (std::sscanf(f[2].c_str(), "%u:%u", &maj, &min) == 2) {
            m.major = maj;
            m.minor = min;
        }
        m.root_path = unescape_mount(f[4]);
        m.options = f[5];
        m.fs_type = f[dash + 1];
        m.source = unescape_mount(f[dash + 2]);
        rows.push_back(std::move(m));
    }
    return rows;
}

std::vector<MountInfo> read_mountinfo() {
    std::string text;
    // /proc files report size 0; read in a loop instead of read_file_capped.
    FILE *f = std::fopen("/proc/self/mountinfo", "re");
    if (!f)
        return {};
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (8u << 20))
        text.append(buf, n);
    std::fclose(f);
    return parse_mountinfo(text);
}

bool is_pseudo_fs(const std::string &t) {
    static const char *const pseudo[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "securityfs", "cgroup", "cgroup2", "pstore",
        "efivarfs", "bpf", "debugfs", "tracefs", "configfs", "fusectl", "mqueue", "hugetlbfs",
        "autofs", "binfmt_misc", "rpc_pipefs", "nsfs", "ramfs", "squashfs", "overlay", "fuse.portal",
        "fuse.gvfsd-fuse", "selinuxfs", "rootfs", "devfs", "zram"};
    for (const char *p : pseudo)
        if (t == p)
            return true;
    return false;
}

bool is_remote_fs(const std::string &t) {
    static const char *const remote[] = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "coda", "afs",
                                         "ceph", "9p", "ncpfs", "ocfs2", "gfs2", "sshfs"};
    for (const char *r : remote)
        if (t == r)
            return true;
    return t.compare(0, 5, "fuse.") == 0 && t != "fuse.fuseblk";
}

} // namespace flo

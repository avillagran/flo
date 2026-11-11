// Small POSIX filesystem helpers shared by the core.
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sys/stat.h>
#include <sys/types.h>

namespace flo {

// Lexical path helpers (no filesystem access).
std::string path_join(const std::string &dir, const std::string &name);
std::string path_dirname(const std::string &path);  // "/a/b" -> "/a", "/a" -> "/"
std::string path_basename(const std::string &path); // "/a/b/" -> "b"
std::string clean_path(const std::string &path);    // collapses //, /./, /x/../
// `path` is `root` or lies below it (lexically, both cleaned).
bool path_inside(const std::string &path, const std::string &root);
// Absolute form of `path` relative to the current directory, cleaned.
std::string absolute_path(const std::string &path);
// realpath(3), or "" when it fails.
std::string real_path(const std::string &path);

// Identity of a filesystem object, taken without following a final symlink.
struct FileId {
    dev_t dev = 0;
    ino_t ino = 0;
    mode_t mode = 0;
    bool valid() const { return ino != 0 || dev != 0; }
    bool operator==(const FileId &o) const { return dev == o.dev && ino == o.ino; }
    bool operator!=(const FileId &o) const { return !(*this == o); }
};
bool lstat_id(const std::string &path, FileId &id);
bool exists_nofollow(const std::string &path);
bool is_dir(const std::string &path);          // follows symlinks
bool is_dir_nofollow(const std::string &path); // a real directory, not a link

// Where the extension starts in a file name (".tar.gz" counts as one), or
// npos for none (folders, dotfiles like ".bashrc").
std::size_t extension_start(const std::string &name, bool is_dir);

// Folio's freeName(): `name` if nothing exists by that name in `folder`,
// else "base (copy).ext", "base (copy 2).ext", ...
std::string free_name(const std::string &folder, const std::string &name);
// The candidate number n (0 = name itself, 1 = "(copy)", n>1 = "(copy n)").
std::string copy_name(const std::string &name, bool is_dir, int n);

// renameat2(RENAME_NOREPLACE) with a link()+unlink() fallback for
// filesystems without it. Never replaces an existing name: fails with
// errno == EEXIST instead.
int rename_noreplace(const std::string &from, const std::string &to);
int rename_noreplace_at(int from_dir, const char *from, int to_dir, const char *to);

// Creates every missing directory of `path` with `mode` (like mkdir -p).
bool make_dirs(const std::string &path, mode_t mode);

// Writes `data` to a temporary file beside `path` (O_EXCL, O_NOFOLLOW, the
// given mode whatever the umask) and renames it over `path`. Readers never
// see half a file. Returns false (and removes only its own temporary file)
// on failure.
bool atomic_write_file(const std::string &path, const std::string &data, mode_t mode);

// Reads a whole regular file, refusing symlinks and anything over max_bytes.
bool read_file_capped(const std::string &path, std::size_t max_bytes, std::string &out,
                      bool follow_symlinks = true);

// The process umask, read without changing it (umask(2) is racy in threads).
mode_t current_umask();

// Nanoseconds since the epoch for a struct stat time.
int64_t timespec_ns(const struct timespec &ts);

// The device of the filesystem a path is on (follows symlinks), or 0.
dev_t device_of(const std::string &path);

// Mount table rows from /proc/self/mountinfo (or a fixture text).
struct MountInfo {
    std::string root_path;  // mount point, unescaped
    std::string fs_type;
    std::string source;     // device, e.g. /dev/sda1
    std::string options;    // per-mount options (rw,nosuid,...)
    unsigned major = 0, minor = 0;
};
std::vector<MountInfo> parse_mountinfo(const std::string &text);
std::vector<MountInfo> read_mountinfo();
// Pseudo, network-less virtual filesystems that never hold user files.
bool is_pseudo_fs(const std::string &fs_type);
// Network and FUSE filesystems where reading file contents is slow.
bool is_remote_fs(const std::string &fs_type);

} // namespace flo

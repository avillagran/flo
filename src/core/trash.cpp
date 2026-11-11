#include "core/trash.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo::trash {

namespace {

std::string uid_str() { return std::to_string(static_cast<unsigned long>(::getuid())); }

std::string now_local() {
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

int64_t parse_local(const std::string &s) {
    std::tm tm{};
    if (!::strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm))
        return 0;
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm));
}

// A directory we own (or create, 0700), never a symlink.
bool own_dir(const std::string &dir, bool create) {
    struct stat st;
    if (::lstat(dir.c_str(), &st) != 0) {
        if (!create || errno != ENOENT || ::mkdir(dir.c_str(), 0700) != 0 || ::lstat(dir.c_str(), &st) != 0)
            return false;
    }
    return S_ISDIR(st.st_mode) && st.st_uid == ::getuid();
}

// The spec's $topdir/.Trash: a real directory with the sticky bit.
bool shared_trash_ok(const std::string &dir) {
    struct stat st;
    return ::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX);
}

// The topmost folder above `dir` still on the same device: the mount point
// (or btrfs subvolume root), as GIO finds it.
std::string topdir_for(const std::string &dir) {
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0)
        return {};
    std::string cur = dir;
    while (cur != "/") {
        const std::string up = path_dirname(cur);
        struct stat ust;
        if (::stat(up.c_str(), &ust) != 0 || ust.st_dev != st.st_dev)
            break;
        cur = up;
    }
    return cur;
}

bool ensure_trash(const std::string &trash_dir) {
    return own_dir(trash_dir, true) && own_dir(trash_dir + "/files", true) && own_dir(trash_dir + "/info", true);
}

bool write_all(int fd, const std::string &data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        done += static_cast<std::size_t>(w);
    }
    return true;
}

} // namespace

std::string home_trash() { return xdg::data_home() + "/Trash"; }

std::string topdir_of(const std::string &trash_dir) {
    const std::string uid = uid_str();
    const std::string base = path_basename(trash_dir);
    if (base == ".Trash-" + uid)
        return path_dirname(trash_dir);
    if (base == uid && path_basename(path_dirname(trash_dir)) == ".Trash")
        return path_dirname(path_dirname(trash_dir));
    return {};
}

std::vector<std::string> trash_dirs() {
    std::vector<std::string> dirs{home_trash()};
    const std::string uid = uid_str();
    for (const auto &m : read_mountinfo()) {
        if (is_pseudo_fs(m.fs_type) || is_remote_fs(m.fs_type))
            continue;
        const std::string top = m.root_path == "/" ? "" : m.root_path;
        for (const std::string &d : {top + "/.Trash/" + uid, top + "/.Trash-" + uid}) {
            if (d.find("/.Trash/") != std::string::npos && !shared_trash_ok(top + "/.Trash"))
                continue;
            if (is_dir_nofollow(d + "/files") && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
                dirs.push_back(d);
        }
    }
    return dirs;
}

bool in_trash(const std::string &path) {
    for (const auto &d : trash_dirs())
        if (path_inside(path, d + "/files"))
            return true;
    return false;
}

std::string format_trashinfo(const std::string &path_value, const std::string &deletion_date) {
    return "[Trash Info]\nPath=" + path_value + "\nDeletionDate=" + deletion_date + "\n";
}

bool parse_trashinfo(const std::string &text, const std::string &topdir, std::string &original_path,
                     std::string &deletion_date) {
    std::istringstream in(text);
    std::string line;
    bool section = false, have_path = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] == '[') {
            section = line == "[Trash Info]";
            continue;
        }
        if (!section)
            continue;
        if (line.compare(0, 5, "Path=") == 0 && !have_path) {
            std::string decoded;
            if (!percent_decode(line.substr(5), decoded) || decoded.empty())
                return false;
            if (decoded[0] != '/') {
                if (topdir.empty())
                    return false; // the home trash only stores absolute paths
                decoded = path_join(topdir, decoded);
            }
            original_path = clean_path(decoded);
            have_path = true;
        } else if (line.compare(0, 13, "DeletionDate=") == 0) {
            deletion_date = line.substr(13);
        }
    }
    return have_path;
}

bool put(const std::string &path_in, Receipt &receipt, std::string &error, const FileId *expected) {
    const std::string path = absolute_path(path_in);
    const std::string name = path_basename(path);
    if (path == "/" || name.empty() || name == "." || name == "..") {
        error = "This can't be moved to the trash.";
        return false;
    }
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        error = name + " is no longer there.";
        return false;
    }
    if (expected && (st.st_dev != expected->dev || st.st_ino != expected->ino)) {
        error = name + " was replaced by something else; it was left alone.";
        return false;
    }
    for (const auto &d : trash_dirs())
        if (path_inside(path, d) || path_inside(d, path)) {
            error = name + " is (or holds) a trash folder; it can't be trashed.";
            return false;
        }

    // The home trash serves its own device; everything else goes to its
    // volume's trash, so trashing never copies.
    std::string trash_dir, path_value;
    const std::string home = home_trash();
    make_dirs(path_dirname(home), 0700);
    const dev_t home_dev = device_of(path_dirname(home));
    if (home_dev != 0 && st.st_dev == home_dev) {
        trash_dir = home;
        path_value = percent_encode_path(path);
    } else {
        const std::string top = topdir_for(path_dirname(path));
        if (top.empty()) {
            error = "Couldn't find the drive holding " + name + "; it was left alone.";
            return false;
        }
        const std::string uid = uid_str();
        const std::string shared = (top == "/" ? "" : top) + "/.Trash";
        if (shared_trash_ok(shared) && own_dir(shared + "/" + uid, true))
            trash_dir = shared + "/" + uid;
        else
            trash_dir = (top == "/" ? "" : top) + "/.Trash-" + uid;
        const std::string rel = path.substr(top == "/" ? 1 : top.size() + 1);
        path_value = percent_encode_path(rel);
    }
    if (!ensure_trash(trash_dir)) {
        error = "The drive holding " + name + " has no usable trash (" + trash_dir + "); it was left alone.";
        return false;
    }

    // Reserve a name with the note first (O_EXCL), as the spec asks.
    std::string chosen, info;
    int fd = -1;
    for (int n = 1; n < 10000; ++n) {
        const std::string candidate = n == 1 ? name : name + "." + std::to_string(n);
        if (exists_nofollow(trash_dir + "/files/" + candidate))
            continue;
        info = trash_dir + "/info/" + candidate + ".trashinfo";
        fd = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            chosen = candidate;
            break;
        }
        if (errno != EEXIST) {
            error = "Couldn't write to the trash: " + std::string(std::strerror(errno));
            return false;
        }
    }
    if (fd < 0) {
        error = "The trash has no free name for " + name + ".";
        return false;
    }
    const bool written = write_all(fd, format_trashinfo(path_value, now_local())) && ::fsync(fd) == 0;
    ::close(fd);
    if (!written) {
        ::unlink(info.c_str()); // the note this call just created
        error = "Couldn't write the trash note for " + name + ".";
        return false;
    }
    // Last identity check right before the move.
    struct stat again;
    if (::lstat(path.c_str(), &again) != 0 || again.st_ino != st.st_ino || again.st_dev != st.st_dev) {
        ::unlink(info.c_str());
        error = name + " changed while it was being trashed; it was left alone.";
        return false;
    }
    const std::string target = trash_dir + "/files/" + chosen;
    if (rename_noreplace(path, target) != 0) {
        const int e = errno;
        ::unlink(info.c_str());
        error = e == EXDEV ? name + " is on another drive than its trash; it was left alone."
                           : "Couldn't move " + name + " to the trash: " + std::strerror(e);
        return false;
    }
    receipt.trash_dir = trash_dir;
    receipt.name = chosen;
    receipt.original_path = path;
    receipt.id = FileId{st.st_dev, st.st_ino, st.st_mode};
    return true;
}

std::vector<Item> list_dir(const std::string &trash_dir) {
    std::vector<Item> items;
    const std::string topdir = topdir_of(trash_dir);
    DIR *d = ::opendir((trash_dir + "/info").c_str());
    if (!d)
        return items;
    std::size_t budget = 50000;
    while (dirent *e = ::readdir(d)) {
        if (budget-- == 0)
            break;
        const std::string fname = e->d_name;
        if (fname.size() <= 10 || fname.compare(fname.size() - 10, 10, ".trashinfo") != 0)
            continue;
        Item item;
        item.trash_dir = trash_dir;
        item.name = fname.substr(0, fname.size() - 10);
        struct stat st;
        if (::lstat(item.files_path().c_str(), &st) != 0)
            continue; // a note without its item (perhaps on its way in)
        std::string text;
        if (read_file_capped(item.info_path(), 64 * 1024, text, false))
            parse_trashinfo(text, topdir, item.original_path, item.deletion_date);
        item.deleted_at = parse_local(item.deletion_date);
        item.is_link = S_ISLNK(st.st_mode);
        item.is_dir = S_ISDIR(st.st_mode);
        item.size = item.is_dir ? 0 : static_cast<int64_t>(st.st_size);
        items.push_back(std::move(item));
    }
    ::closedir(d);
    return items;
}

std::vector<Item> list() {
    std::vector<Item> all;
    for (const auto &d : trash_dirs()) {
        auto items = list_dir(d);
        std::move(items.begin(), items.end(), std::back_inserter(all));
    }
    std::stable_sort(all.begin(), all.end(), [](const Item &a, const Item &b) { return a.deleted_at > b.deleted_at; });
    return all;
}

static bool known_trash(const std::string &trash_dir) {
    const auto dirs = trash_dirs();
    return std::find(dirs.begin(), dirs.end(), trash_dir) != dirs.end();
}

bool item_for_trashed_path(const std::string &files_path, Item &item) {
    const std::string p = clean_path(absolute_path(files_path));
    const std::string files = path_dirname(p);
    if (path_basename(files) != "files")
        return false;
    const std::string dir = path_dirname(files);
    if (!known_trash(dir))
        return false;
    for (auto &i : list_dir(dir))
        if (i.name == path_basename(p)) {
            item = i;
            return true;
        }
    return false;
}

bool restore(const std::string &trash_dir, const std::string &name, OnConflict policy, std::string &restored_to,
             std::string &error, const FileId *expected) {
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..") {
        error = "Not a trashed item.";
        return false;
    }
    Item item;
    item.trash_dir = trash_dir;
    item.name = name;
    const std::string from = item.files_path();
    struct stat st;
    if (::lstat(from.c_str(), &st) != 0) {
        error = name + " is no longer in the trash.";
        return false;
    }
    if (expected && (st.st_dev != expected->dev || st.st_ino != expected->ino)) {
        error = name + " in the trash is not the item that was trashed; it was left alone.";
        return false;
    }
    std::string text, original, date;
    if (!read_file_capped(item.info_path(), 64 * 1024, text, false) ||
        !parse_trashinfo(text, topdir_of(trash_dir), original, date)) {
        error = name + " has no record of where it came from.";
        return false;
    }
    const std::string folder = path_dirname(original);
    if (!make_dirs(folder, 0755)) {
        error = "Couldn't recreate " + folder + ".";
        return false;
    }
    std::string target_name = path_basename(original);
    if (policy == OnConflict::KeepBoth)
        target_name = free_name(folder, target_name);
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string to = path_join(folder, target_name);
        if (rename_noreplace(from, to) == 0) {
            ::unlink(item.info_path().c_str()); // the note goes once its item is back
            restored_to = to;
            return true;
        }
        const int e = errno;
        if (e == EEXIST && policy == OnConflict::KeepBoth) {
            target_name = free_name(folder, path_basename(original));
            continue;
        }
        if (e == EEXIST)
            error = path_basename(original) + " already exists in " + folder + "; nothing was replaced.";
        else if (e == EXDEV)
            error = name + " is in the trash of another drive than " + folder + "; it was left in the trash.";
        else
            error = "Couldn't restore " + name + ": " + std::strerror(e);
        return false;
    }
    error = "Couldn't find a free name to restore " + name + ".";
    return false;
}

bool restore(const Item &item, OnConflict policy, std::string &restored_to, std::string &error) {
    return restore(item.trash_dir, item.name, policy, restored_to, error, nullptr);
}

namespace {

// Removes the entry `name` in `dirfd` and, for a folder, everything in it:
// never through symlinks, never onto another filesystem, bounded depth.
bool remove_entry_at(int dirfd, const char *name, dev_t dev, int depth) {
    if (depth > 256)
        return false;
    struct stat st;
    if (::fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return ::unlinkat(dirfd, name, 0) == 0 || errno == ENOENT;
    if (st.st_dev != dev)
        return false;
    const int fd = ::openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat opened;
    if (::fstat(fd, &opened) != 0 || opened.st_ino != st.st_ino || opened.st_dev != st.st_dev) {
        ::close(fd);
        return false;
    }
    DIR *d = ::fdopendir(fd);
    if (!d) {
        ::close(fd);
        return false;
    }
    bool ok = true;
    std::vector<std::string> names;
    while (dirent *e = ::readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        names.emplace_back(n);
    }
    for (const auto &n : names)
        ok = remove_entry_at(fd, n.c_str(), dev, depth + 1) && ok;
    ::closedir(d);
    return ok && ::unlinkat(dirfd, name, AT_REMOVEDIR) == 0;
}

} // namespace

bool delete_permanently(const std::string &files_path, bool confirmed, std::string &error) {
    if (!confirmed) {
        error = "Deleting permanently needs confirmation.";
        return false;
    }
    const std::string p = clean_path(absolute_path(files_path));
    const std::string files = path_dirname(p);
    const std::string name = path_basename(p);
    const std::string dir = path_dirname(files);
    if (path_basename(files) != "files" || name.empty() || name == "." || name == ".." || !known_trash(dir)) {
        error = "Only items in the trash can be deleted permanently.";
        return false;
    }
    const int fd = ::open(files.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        error = "Couldn't open the trash.";
        return false;
    }
    struct stat fst;
    const bool ok = ::fstat(fd, &fst) == 0 && fst.st_uid == ::getuid() && remove_entry_at(fd, name.c_str(), fst.st_dev, 0);
    ::close(fd);
    if (!ok) {
        error = "Couldn't delete all of " + name + ".";
        return false;
    }
    ::unlink((dir + "/info/" + name + ".trashinfo").c_str());
    return true;
}

int empty_all(bool confirmed, std::string &error) {
    if (!confirmed) {
        error = "Emptying the trash needs confirmation.";
        return 0;
    }
    int count = 0;
    for (const auto &dir : trash_dirs()) {
        DIR *d = ::opendir((dir + "/files").c_str());
        if (!d)
            continue;
        std::vector<std::string> names;
        while (dirent *e = ::readdir(d)) {
            const char *n = e->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
                continue;
            names.emplace_back(n);
        }
        ::closedir(d);
        for (const auto &n : names) {
            std::string e;
            if (delete_permanently(dir + "/files/" + n, true, e))
                ++count;
            else
                error = e;
        }
        // GIO's size cache would be stale now.
        ::unlink((dir + "/directorysizes").c_str());
    }
    return count;
}

} // namespace flo::trash

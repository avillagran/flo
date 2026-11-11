#include "core/tree_size.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace flo {

TreeSize tree_size(const std::string &root, const std::atomic<bool> &cancel, uint64_t max_entries) {
    TreeSize out;
    int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return out;
    struct stat rst;
    if (fstat(fd, &rst) != 0) {
        close(fd);
        return out;
    }
    const dev_t dev = rst.st_dev;
    // Depth-first with an explicit stack of open directory streams.
    std::vector<DIR *> stack;
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return out;
    }
    stack.push_back(d);
    uint64_t seen = 0;
    bool stopped = false;
    while (!stack.empty()) {
        if (cancel.load(std::memory_order_relaxed) || seen > max_entries) {
            stopped = true;
            break;
        }
        DIR *cur = stack.back();
        struct dirent *de = readdir(cur);
        if (!de) {
            closedir(cur);
            stack.pop_back();
            continue;
        }
        if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, ".."))
            continue;
        ++seen;
        struct stat st;
        if (fstatat(dirfd(cur), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        out.bytes += uint64_t(st.st_size);
        if (S_ISDIR(st.st_mode)) {
            ++out.folders;
            if (st.st_dev != dev)
                continue; // another filesystem: counted, not entered
            int cfd = openat(dirfd(cur), de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd < 0) {
                ++out.unreadable;
                continue;
            }
            DIR *child = fdopendir(cfd);
            if (!child) {
                close(cfd);
                ++out.unreadable;
                continue;
            }
            stack.push_back(child);
        } else {
            ++out.files;
        }
    }
    for (DIR *s : stack)
        closedir(s);
    out.complete = !stopped;
    return out;
}

std::string si_size(uint64_t bytes) {
    if (bytes < 1000)
        return std::to_string(bytes) + " B";
    const char *units[] = {"kB", "MB", "GB", "TB", "PB"};
    double v = double(bytes);
    int u = -1;
    while (v >= 1000 && u < 4) {
        v /= 1000;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, v < 100 ? "%.1f %s" : "%.0f %s", v, units[u]);
    return buf;
}

} // namespace flo

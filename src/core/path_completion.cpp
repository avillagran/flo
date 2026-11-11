#include "core/path_completion.h"

#include "core/fsutil.h"
#include "core/network.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>

namespace flo {

namespace {

std::string lower(const std::string &s) {
    std::string out = s;
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return out;
}

bool starts_with(const std::string &s, const std::string &prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// One UTF-8 sequence's length from its lead byte (1 for anything invalid).
size_t sequence(unsigned char c) {
    if (c >= 0xF0 && c < 0xF8)
        return 4;
    if (c >= 0xE0)
        return c < 0xF0 ? 3 : 1;
    if (c >= 0xC0)
        return 2;
    return 1;
}

} // namespace

std::string PathCompletion::text(int i) const {
    if (i < 0 || i >= int(items.size()))
        return base;
    const PathCandidate &c = items[size_t(i)];
    return base + c.name + (c.dir ? "/" : "");
}

std::string PathCompletion::render() const {
    std::string out = "Tab \xC2\xB7";
    for (size_t i = 0; i < items.size(); ++i) {
        if (out.size() > 400) {
            out += " \xE2\x80\xA6 (" + std::to_string(items.size()) + ")";
            break;
        }
        std::string shown = items[i].name + (items[i].dir ? "/" : "");
        out += int(i) == index ? " [" + shown + "]" : " " + shown;
    }
    return out;
}

void split_component(const std::string &text, std::string &base, std::string &partial) {
    auto slash = text.rfind('/');
    if (slash == std::string::npos) {
        base.clear();
        partial = text;
    } else {
        base = text.substr(0, slash + 1);
        partial = text.substr(slash + 1);
    }
}

std::vector<PathCandidate> completion_matches(std::vector<PathCandidate> names, const std::string &partial) {
    const bool hidden = starts_with(partial, ".");
    names.erase(std::remove_if(names.begin(), names.end(),
                               [&](const PathCandidate &c) { return !hidden && starts_with(c.name, "."); }),
                names.end());
    std::vector<PathCandidate> found;
    for (const auto &c : names)
        if (starts_with(c.name, partial))
            found.push_back(c);
    if (found.empty()) {
        const std::string low = lower(partial);
        for (const auto &c : names)
            if (starts_with(lower(c.name), low))
                found.push_back(c);
    }
    if (std::any_of(found.begin(), found.end(), [](const PathCandidate &c) { return c.dir; }))
        found.erase(std::remove_if(found.begin(), found.end(), [](const PathCandidate &c) { return !c.dir; }),
                    found.end());
    std::sort(found.begin(), found.end(), [](const PathCandidate &a, const PathCandidate &b) {
        std::string la = lower(a.name), lb = lower(b.name);
        if (la != lb)
            return la < lb;
        if (a.name != b.name)
            return a.name < b.name;
        return a.dir < b.dir;
    });
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (found.size() > PathCompletion::kItems)
        found.resize(PathCompletion::kItems);
    return found;
}

std::string common_prefix(const std::vector<PathCandidate> &items, bool insensitive) {
    if (items.empty())
        return {};
    const std::string &first = items[0].name;
    size_t end = first.size();
    for (size_t k = 1; k < items.size(); ++k) {
        const std::string &name = items[k].name;
        size_t shared = 0, i = 0;
        // Whole UTF-8 sequences only, so the prefix never splits a character.
        while (i < first.size() && i < end) {
            size_t n = sequence(static_cast<unsigned char>(first[i]));
            if (i + n > first.size() || i + n > name.size())
                break;
            std::string a = first.substr(i, n), b = name.substr(i, n);
            if (insensitive ? lower(a) != lower(b) : a != b)
                break;
            i += n;
            shared = i;
        }
        end = std::min(end, shared);
    }
    return first.substr(0, end);
}

std::vector<PathCandidate> read_candidates(const std::string &dir, const std::string &partial) {
    std::vector<PathCandidate> out;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return out;
    const std::string low = lower(partial);
    size_t scanned = 0, links = 0;
    while (dirent *de = readdir(d)) {
        if (++scanned > PathCompletion::kScan)
            break;
        std::string name = de->d_name;
        if (name == "." || name == "..")
            continue;
        if (!starts_with(lower(name), low))
            continue;
        bool folder = false;
        unsigned char type = de->d_type;
        if (type == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0)
                type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : DT_REG;
        }
        if (type == DT_DIR) {
            folder = true;
        } else if (type == DT_LNK && links < PathCompletion::kLinks) {
            // A link to a folder browses as one: one stat, nothing opened.
            ++links;
            struct stat st;
            folder = fstatat(dirfd(d), de->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);
        }
        out.push_back({name, folder});
    }
    closedir(d);
    return out;
}

bool network_location(const std::string &dir) {
    const std::string gvfs = gvfs_root();
    if (!gvfs.empty() && path_inside(dir, gvfs))
        return true;
    // The file system of the deepest mount holding `dir`, as the rest of
    // Flo reads it (network and FUSE types, fuseblk drives excepted).
    const MountInfo *best = nullptr;
    const std::vector<MountInfo> mounts = read_mountinfo();
    for (const auto &m : mounts)
        if (path_inside(dir, m.root_path) && (!best || m.root_path.size() > best->root_path.size()))
            best = &m;
    return best && is_remote_fs(best->fs_type);
}

} // namespace flo

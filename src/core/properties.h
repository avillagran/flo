// Files' properties and permissions (file_properties.rs,
// properties_details.rs): what the Alt+Enter dialog shows about one local
// object, and the one change it can make, the r/w/x bits.
//
// Reading never follows a final symbolic link (lstat; the link's target is
// read with readlink and only checked for existence). Changing the bits
// works on a descriptor opened with O_PATH|O_NOFOLLOW, after checking that
// the object is still the one observed (device, inode, owner and mode); it
// is refused for links, special objects, objects the user does not own and
// objects with setuid/setgid/sticky bits. No recursion, no ACLs, no chown.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace flo::props {

enum class Kind { File, Directory, Symlink, Other };

struct Timestamp {
    int64_t sec = 0;
    int64_t nsec = 0;
    bool known = false;
};

struct Info {
    std::string path;
    Kind kind = Kind::Other;
    uint64_t apparent = 0, allocated = 0, hardlinks = 0;
    uint32_t uid = 0, gid = 0;
    uint32_t mode = 0; // the permission bits and setuid/setgid/sticky (07777)
    std::string owner, group; // names, or the numbers when unknown
    uint64_t device = 0, inode = 0;
    Timestamp modified, accessed, changed, birth;
    // Symbolic links: the raw target (never opened) and whether it exists.
    std::string link_target;
    bool link_resolves = false;
    // Files: the content type (by name, then by content) for "Open with".
    std::string mime;
    // The filesystem holding the object.
    std::string fs_type;
    uint64_t free_bytes = 0, total_bytes = 0;
    bool volume = false;
};

// Reads `path` without following a final link. Returns "" or why not.
std::string read(const std::string &path, Info &out);

// Whether Flo may change this object's bits (see the header comment).
bool editable(const Info &info);

// Sets the r/w/x bits (0777 of `mode`) of the object observed as
// `observed`, by descriptor, after checking it is still that object.
// Fills `after` with a fresh read. Returns "" or why not; nothing changes
// when it refuses.
std::string apply_mode(const Info &observed, uint32_t mode, Info &after);

// "drwxr-x---": `ls`-style, setuid/setgid/sticky included.
std::string symbolic_mode(uint32_t mode, char kind);
// "2026-11-11 16:04:05" in local time.
std::string format_time(const Timestamp &t);

// The contents of folders and selections (Files' properties census):
// apparent bytes, files, folders and links below the roots, without
// following links or crossing filesystems.
struct Census {
    uint64_t bytes = 0, files = 0, folders = 0, links = 0, unreadable = 0;
    bool complete = true;
};
Census census(const std::vector<std::string> &roots, const std::atomic<bool> &cancel,
              uint64_t max_entries = 2000000);

} // namespace flo::props

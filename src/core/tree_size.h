// The apparent size of a folder tree, as Files' size helper measures it:
// every entry's st_size summed, without following symbolic links, without
// crossing into other filesystems, and stopping early when asked to.
// Read-only: it opens, reads and stats, nothing else.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace flo {

struct TreeSize {
    uint64_t bytes = 0;
    uint64_t files = 0, folders = 0; // below the root
    uint64_t unreadable = 0;         // folders that could not be opened
    bool complete = false;           // false: cancelled or over the limit
};

// Walks `root` (opened with O_NOFOLLOW: a link is not walked). Stops when
// `cancel` becomes true or after `max_entries` entries.
TreeSize tree_size(const std::string &root, const std::atomic<bool> &cancel, uint64_t max_entries = 2000000);

// Files' size label: "630 B", "4.2 kB", "1.1 MB" (decimal units).
std::string si_size(uint64_t bytes);

} // namespace flo

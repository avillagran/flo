// Files' Tab completion in the location field (files-rust crates/ui
// path_edit.rs): the last component typed is completed like a shell. One
// match is taken whole ("Pictures/"); several extend the text to their
// common prefix and become a list that Tab / Shift+Tab (or Up / Down)
// walks through, shown as one line ("Tab · a/ [b/] c/").
//
// Folders win: when any candidate is a folder only folders are offered.
// Hidden names only when the typed part starts with ".". Matching is
// case-sensitive first and case-insensitive when that finds nothing. A
// folder is read once (at most 5,000 entries, at most 64 symbolic links
// resolved, never followed further), and network locations are not read.
#pragma once

#include <string>
#include <vector>

namespace flo {

struct PathCandidate {
    std::string name;
    bool dir = false;
    bool operator==(const PathCandidate &o) const { return name == o.name && dir == o.dir; }
};

struct PathCompletion {
    static constexpr size_t kScan = 5000, kLinks = 64, kItems = 256;

    std::string base;                  // the text before the completed component
    std::vector<PathCandidate> items;  // empty: "no matches" (or no completion here)
    int index = -1;                    // the candidate shown in the field, -1 none

    // The field text for candidate i: base + name, "/" after a folder.
    std::string text(int i) const;
    // "Tab · a/ [b/] c/" (the current one in brackets), cut after ~400 bytes.
    std::string render() const;
};

// "dir/part" -> ("dir/", "part"); "part" -> ("", "part").
void split_component(const std::string &text, std::string &base, std::string &partial);
// The candidates for `partial` among `names` (see the header comment),
// sorted case-insensitively, at most kItems.
std::vector<PathCandidate> completion_matches(std::vector<PathCandidate> names, const std::string &partial);
// The prefix every candidate shares (case-insensitive when asked), spelled
// as the first one.
std::string common_prefix(const std::vector<PathCandidate> &items, bool insensitive);
// One bounded read of `dir`: names starting with `partial` in any case.
std::vector<PathCandidate> read_candidates(const std::string &dir, const std::string &partial);
// True for folders Files does not complete in: GVfs mounts and network or
// FUSE file systems.
bool network_location(const std::string &dir);

} // namespace flo

// Starred items (Files' starred.rs): files and folders marked with Ctrl+E
// and listed together with Ctrl+Alt+E. Independent of the GTK bookmarks;
// starring never touches the item.
//
// $XDG_DATA_HOME/flo/starred, UTF-8 text written atomically (0600): the
// header line, then one percent-encoded file:// URI per line in the order
// the items were starred. Blank lines, comments, duplicates and non-local
// URIs are skipped; a file that is not UTF-8 text is never rewritten.
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace flo {

class Starred {
public:
    static constexpr std::size_t kMaxItems = 10000, kMaxBytes = 4u << 20;

    static std::string path(); // $XDG_DATA_HOME/flo/starred

    bool load(const std::string &file = path());
    bool save(const std::string &file = path()) const;

    bool contains(const std::string &path) const;
    // Stars every path not starred yet (oldest first); false when that would
    // pass 10,000 items (nothing changes then).
    bool add(const std::vector<std::string> &paths);
    // Unstars these; returns how many were starred.
    int remove(const std::vector<std::string> &paths);
    // Ctrl+E: when every path is starred they are all unstarred, otherwise
    // they are all starred. Returns +n (starred) or -n (unstarred), 0 when
    // nothing could change.
    int toggle(const std::vector<std::string> &paths);

    // In the order they were starred.
    const std::vector<std::string> &items() const { return items_; }
    bool writable() const { return writable_; }
    const std::string &error() const { return error_; }

    std::string serialize() const;
    static bool parse(const std::string &text, std::vector<std::string> &out);

private:
    void reindex();
    std::vector<std::string> items_;
    std::unordered_set<std::string> index_; // contains() is asked for every drawn row
    bool writable_ = true;
    std::string error_;
};

} // namespace flo

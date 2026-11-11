// "/" search: names under a folder. When plocate is installed and its
// database answers, its matches inside the folder come first (Folio's plan:
// search by name with the plocate database Omarchy already maintains);
// then a live breadth-first walk (nearest first) adds what the database
// doesn't know yet (new files, mounts, folders updatedb skips). Both stay
// on the folder's filesystem and never enter symlinked folders. Results
// stream in batches; a new search cancels the previous one.
#pragma once

#include "core/entry.h"
#include "core/event_queue.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace flo {

struct SearchOptions {
    bool show_hidden = false;   // also match (and walk into) hidden entries
    std::size_t limit = 2000;   // stop after this many matches
    int batch_ms = 60;          // deliver at least this often while matching
    std::size_t batch_max = 256;
    // plocate first: the program ("" turns it off) and a database ("" =
    // plocate's default). Overridable for tests with $FLO_PLOCATE and
    // $FLO_PLOCATE_DB.
    std::string locate_program = "plocate";
    std::string locate_db;
    bool walk = true; // the live walk after plocate
};

struct SearchStats {
    std::size_t matches = 0;
    std::size_t located = 0; // of the matches, how many plocate found
    std::size_t folders_walked = 0;
    bool truncated = false; // hit the limit
    bool cancelled = false;
};

// Synchronous walk. `on_batch` receives entries whose name is the path
// relative to `root`. Returns when the walk ends, is cancelled or hits the
// limit.
// `skip` holds relative paths already delivered (by plocate), not repeated.
SearchStats search_walk(const std::string &root, const std::string &query, const SearchOptions &options,
                        const CancelToken *cancel, const std::function<void(std::vector<Entry> &&)> &on_batch,
                        const std::unordered_set<std::string> *skip = nullptr);

// plocate's matches under `root`, checked against the disk (gone, hidden,
// another filesystem, or not really matching: left out), nearest first.
// Fills `found` with their relative paths. Returns false when plocate is
// missing or has no usable database (the walk then does it all).
bool search_locate(const std::string &root, const std::string &query, const SearchOptions &options,
                   const CancelToken *cancel, std::vector<Entry> &out, std::unordered_set<std::string> &found);

// The extended regular expression plocate gets: a name containing `query`
// anywhere below `root`.
std::string locate_pattern(const std::string &root, const std::string &query);

class Searcher {
public:
    explicit Searcher(EventQueue &queue);
    ~Searcher();

    using BatchCallback = std::function<void(std::vector<Entry> &&batch)>;
    using DoneCallback = std::function<void(const SearchStats &)>;
    // Starts a search (cancelling the current one). Callbacks run on the UI
    // thread; nothing from an overtaken search is delivered.
    uint64_t start(const std::string &root, const std::string &query, const SearchOptions &options,
                   BatchCallback on_batch, DoneCallback on_done);
    void cancel();

private:
    EventQueue::Poster poster_;
    Executor exec_;
    CancelToken cancel_;
    uint64_t generation_ = 0;
    AliveGuard alive_;
};

} // namespace flo

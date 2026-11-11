// "/" search: names under a folder, found by a live breadth-first walk
// (nearest first) that stays on the folder's filesystem and never enters
// symlinked folders. Results stream in batches; a new search cancels the
// previous one.
#pragma once

#include "core/entry.h"
#include "core/event_queue.h"

#include <functional>
#include <string>
#include <vector>

namespace flo {

struct SearchOptions {
    bool show_hidden = false;   // also match (and walk into) hidden entries
    std::size_t limit = 2000;   // stop after this many matches
    int batch_ms = 60;          // deliver at least this often while matching
    std::size_t batch_max = 256;
};

struct SearchStats {
    std::size_t matches = 0;
    std::size_t folders_walked = 0;
    bool truncated = false; // hit the limit
    bool cancelled = false;
};

// Synchronous walk. `on_batch` receives entries whose name is the path
// relative to `root`. Returns when the walk ends, is cancelled or hits the
// limit.
SearchStats search_walk(const std::string &root, const std::string &query, const SearchOptions &options,
                        const CancelToken *cancel, const std::function<void(std::vector<Entry> &&)> &on_batch);

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

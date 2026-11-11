// Folder listing off the UI thread, with progressive delivery for slow or
// huge folders and lazily counted folder items.
#pragma once

#include "core/entry.h"
#include "core/event_queue.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo {

struct Listing {
    std::string folder;         // absolute, cleaned
    std::vector<Entry> entries; // sorted per ListOptions
    std::string error;          // a sentence for the window; empty on success
    int error_code = 0;         // errno behind `error`
    bool complete = true;       // false: a progressive snapshot, more to come
    uint64_t generation = 0;    // the request this answers

    std::string path_of(std::size_t i) const;
};
using ListingPtr = std::shared_ptr<const Listing>;

struct ListOptions {
    bool show_hidden = false;
    SortMode sort = SortMode::Name;
    // Sniff content for names without a known type; turned off automatically
    // on network/FUSE filesystems.
    bool sniff = true;
    // A slow listing (a tailnet machine) delivers sorted snapshots: the first
    // after this many ms, then at most every progress_interval_ms.
    int first_progress_ms = 150;
    int progress_interval_ms = 300;
};

// Synchronous listing (the worker side, also used by the CLI). `progress`,
// when given, receives snapshots as described in ListOptions.
Listing list_folder(const std::string &folder, const ListOptions &options, const CancelToken *cancel = nullptr,
                    const std::function<void(const Listing &)> &progress = {});

// Entries in `folder`, or -1 when it can't be read. Hidden ones only when shown.
int count_items(const std::string &folder, bool show_hidden);

class Lister {
public:
    explicit Lister(EventQueue &queue);
    ~Lister();

    using Callback = std::function<void(ListingPtr)>;
    // Starts listing `folder`, cancelling any listing still running for this
    // Lister. `on_update` runs on the UI thread with snapshots (complete =
    // false) and finally the full listing (complete = true). Results for an
    // overtaken request are dropped. Returns the request's generation.
    uint64_t list(const std::string &folder, const ListOptions &options, Callback on_update);
    void cancel();
    uint64_t generation() const { return generation_; }

    // Counts the items inside each folder (visible tiles first: pass them in
    // the order wanted). A new request replaces the pending one. Results
    // arrive in batches as (absolute folder path, count or -1).
    using CountCallback = std::function<void(std::vector<std::pair<std::string, int>>)>;
    void count_items(std::vector<std::string> folders, bool show_hidden, CountCallback on_counts);
    void cancel_counts();

private:
    EventQueue::Poster poster_;
    Executor list_exec_, count_exec_;
    CancelToken list_cancel_, count_cancel_;
    uint64_t generation_ = 0;
    AliveGuard alive_;
};

} // namespace flo

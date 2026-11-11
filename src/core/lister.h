// Folder listing off the UI thread, with progressive delivery for slow or
// huge folders and lazily counted folder items.
#pragma once

#include "core/entry.h"
#include "core/event_queue.h"

#include <functional>
#include <memory>
#include <list>
#include <string>
#include <unordered_map>
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

// A folder's tile: how many entries it holds and a peek of its pictures
// (Folio's FolderModel::summarize): the first `peek` pictures in natural
// name order, by extension (jpg, jpeg, png, webp, gif, heic, avif, jxl),
// stat'ed so the thumbnail cache can check them. No peek on network
// filesystems, where thumbnailing would pull whole files over the wire.
struct FolderSummary {
    std::string folder;      // absolute path asked about
    int count = -1;          // entries, or -1 when unreadable
    std::vector<Entry> peek; // names are relative to `folder`
};
FolderSummary summarize_folder(const std::string &folder, bool show_hidden, int peek = 2);
// Whether a file name is one of the pictures a folder peek shows.
bool is_peek_picture(const char *name);

// Folders just left, so Back, Forward or a folder seen a moment ago shows in
// the same frame (Folio's recentListings), then is re-read in the
// background. A listing is moved in when its window leaves the folder and
// moved out when one comes back, so it never exists twice. Bounded by
// listings and by entries: big folders are simply listed again.
struct CachedListing {
    std::vector<Entry> entries; // with item counts
    std::unordered_map<std::string, std::vector<Entry>> peeks;
};

class ListingCache {
public:
    explicit ListingCache(std::size_t max_listings = 24, std::size_t max_entries = 12000)
        : max_listings_(max_listings), max_entries_(max_entries) {}

    static std::string key(const std::string &folder, bool show_hidden, SortMode sort);
    // Keeps `l` under `key` (replacing what was there); listings over half
    // the entry budget are not kept.
    void put(const std::string &key, CachedListing &&l);
    // Moves the listing out (nullopt-like: false when unknown).
    bool take(const std::string &key, CachedListing &out);
    bool contains(const std::string &key) const { return index_.count(key) != 0; }
    void clear();
    std::size_t listings() const { return lru_.size(); }
    std::size_t entries() const { return entries_; }

private:
    struct Item {
        std::string key;
        CachedListing listing;
    };
    void erase(std::list<Item>::iterator it);
    std::size_t max_listings_, max_entries_, entries_ = 0;
    std::list<Item> lru_; // front = newest
    std::unordered_map<std::string, std::list<Item>::iterator> index_;
};

// Whether two listings show the same items (names, kinds, sizes, times):
// after a remembered listing, an unchanged re-read keeps what is shown.
bool same_listing(const std::vector<Entry> &a, const std::vector<Entry> &b);

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

    // Summarizes each folder (visible tiles first: pass them in the order
    // wanted): its item count and, with `peek` > 0, its first pictures. A new
    // request replaces the pending one. Results arrive in batches.
    using CountCallback = std::function<void(std::vector<FolderSummary>)>;
    void count_items(std::vector<std::string> folders, bool show_hidden, CountCallback on_counts, int peek = 0);
    void cancel_counts();

private:
    EventQueue::Poster poster_;
    Executor list_exec_, count_exec_;
    CancelToken list_cancel_, count_cancel_;
    uint64_t generation_ = 0;
    AliveGuard alive_;
};

} // namespace flo

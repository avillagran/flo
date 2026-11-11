// Folder sizes as Files measures them: the apparent bytes of a folder's
// tree (core/tree_size.h: no links followed, one filesystem), measured one
// folder at a time on a worker thread, newest request first, and kept for
// up to 256 folders. Listing a folder asks for its subfolders' sizes
// (Files' automatic sizes); the inspector's Recalculate asks again.
#pragma once

#include "core/event_queue.h"
#include "core/tree_size.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flo::ui {

class FolderSizes {
public:
    struct Result {
        uint64_t bytes = 0, files = 0, folders = 0;
        bool complete = false;
        int64_t measured_at = 0; // seconds since the epoch
    };
    explicit FolderSizes(EventQueue &queue);
    ~FolderSizes();

    const Result *get(const std::string &path) const;
    bool pending(const std::string &path) const { return queued_.count(path) > 0; }
    // Measures these folders in this order, skipping known ones unless
    // `force`; requests not started yet are dropped.
    void request(const std::vector<std::string> &paths, bool force = false);
    // Stops what is queued and running (leaving a folder).
    void cancel();
    // Called on the FLTK thread after each result.
    void on_result(std::function<void(const std::string &)> fn) { done_ = std::move(fn); }
    size_t known() const { return results_.size(); }

private:
    EventQueue &queue_;
    Executor exec_;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
    std::unordered_map<std::string, std::pair<Result, std::list<std::string>::iterator>> results_;
    std::list<std::string> lru_;
    std::unordered_set<std::string> queued_;
    std::function<void(const std::string &)> done_;
};

} // namespace flo::ui

#include "ui/sizes.h"

#include "core/xdg.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

namespace flo::ui {

namespace {
const size_t kKeep = 256; // Files keeps 256 observations
}

FolderSizes::FolderSizes(EventQueue &queue) : queue_(queue), stop_(std::make_shared<std::atomic<bool>>(false)) {}

FolderSizes::~FolderSizes() {
    stop_->store(true);
    exec_.clear();
}

const FolderSizes::Result *FolderSizes::get(const std::string &path) const {
    auto it = results_.find(path);
    return it == results_.end() ? nullptr : &it->second.first;
}

void FolderSizes::cancel() {
    stop_->store(true);
    stop_ = std::make_shared<std::atomic<bool>>(false);
    exec_.clear();
    queued_.clear();
}

void FolderSizes::request(const std::vector<std::string> &paths, bool force) {
    cancel();
    auto stop = stop_;
    auto poster = queue_.poster();
    std::weak_ptr<char> weak = alive_;
    // Tests and screenshots only (FLO_TEST_RUN): every folder takes this
    // long, so the calculating state can be seen.
    const int delay_ms = std::atoi(xdg::test_hook("FLO_TEST_SIZE_DELAY_MS").c_str());
    for (const auto &p : paths) {
        if (!force && results_.count(p))
            continue;
        queued_.insert(p);
        exec_.run([p, stop, poster, weak, delay_ms, this] {
            if (stop->load())
                return;
            for (int waited = 0; waited < delay_ms && !stop->load(); waited += 20)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            TreeSize t = tree_size(p, *stop);
            if (stop->load() && !t.complete)
                return;
            poster.post([p, t, weak, this] {
                if (weak.expired())
                    return;
                queued_.erase(p);
                Result r;
                r.bytes = t.bytes;
                r.files = t.files;
                r.folders = t.folders;
                r.complete = t.complete;
                r.measured_at = int64_t(std::time(nullptr));
                auto it = results_.find(p);
                if (it != results_.end()) {
                    lru_.erase(it->second.second);
                    results_.erase(it);
                }
                lru_.push_front(p);
                results_[p] = {r, lru_.begin()};
                while (lru_.size() > kKeep) {
                    results_.erase(lru_.back());
                    lru_.pop_back();
                }
                if (done_)
                    done_(p);
            });
        });
    }
}

} // namespace flo::ui

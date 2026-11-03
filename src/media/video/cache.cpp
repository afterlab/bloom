#include <algorithm>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/media/video/session.hpp>
#include <utility>
#include <vector>
namespace bloom::media::video {
DecodedVideoCache::DecodedVideoCache(const std::size_t budget, runtime::MemoryBudgetLedger& ledger)
    : ledger_(ledger), budget_(budget) {
    ledger_.registerCache(
        this, budget, [this] { return residentBytes(); },
        [this](const std::size_t bytes) {
            std::lock_guard lock(mutex_);
            budget_ = bytes;
            while (!entries_.empty() && resident_ > budget_) {
                resident_ -= entries_.back().bytes;
                counters_.evicted(entries_.back().bytes);
                entries_.pop_back();
            }
        });
    ledger_.setPoolProfile(this, {.name = "Decoded video",
                                  .kind = runtime::ProfilerPoolKind::Cache,
                                  .counters = &counters_,
                                  .rows = {},
                                  .entries = [this] {
                                      std::lock_guard lock(mutex_);
                                      return entries_.size();
                                  }});
}
DecodedVideoCache::~DecodedVideoCache() {
    std::shared_ptr<runtime::SourceWorkingSet> window;
    {
        std::lock_guard lock(mutex_);
        window = window_;
    }
    if (window)
        window->removeTrimmer(this);
    ledger_.unregisterCache(this);
}

void DecodedVideoCache::bindSourceWindow(std::shared_ptr<runtime::SourceWorkingSet> window) {
    std::shared_ptr<runtime::SourceWorkingSet> previous;
    {
        std::lock_guard lock(mutex_);
        if (window_ == window)
            return;
        previous = std::exchange(window_, window);
    }
    if (previous)
        previous->removeTrimmer(this);
    if (window)
        window->addTrimmer(this, [this] { releaseOutsideWindow(); });
}

void DecodedVideoCache::releaseOutsideWindow() {
    std::vector<std::shared_ptr<const provider::FrameProduct>> released;
    {
        std::lock_guard lock(mutex_);
        if (!window_)
            return;
        for (auto position = entries_.begin(); position != entries_.end();) {
            if (position->use.tracked && !window_->retains(position->use)) {
                resident_ -= position->bytes;
                counters_.released(position->bytes);
                released.push_back(std::move(position->frame));
                position = entries_.erase(position);
            } else {
                ++position;
            }
        }
    }
    // The planes are freed here, outside the cache lock.
}

std::shared_ptr<const provider::FrameProduct> DecodedVideoCache::find(const FrameKey& key) {
    std::lock_guard lock(mutex_);
    auto found = std::ranges::find(entries_, key, &Entry::key);
    if (found == entries_.end()) {
        counters_.miss();
        return {};
    }
    counters_.hit();
    if (const auto use = runtime::SourceWorkingSet::current(); use.tracked)
        found->use = use;
    entries_.splice(entries_.begin(), entries_, found);
    return entries_.front().frame;
}
void DecodedVideoCache::store(const FrameKey& key,
                              std::shared_ptr<const provider::FrameProduct> frame) {
    // Products reach the cache only after the worker boundary verified their plane digests.
    if (!frame || !provider::validLayout(*frame))
        return;
    std::size_t bytes = 0;
    for (const auto& plane : frame->planes)
        bytes += plane.bytes.size();
    std::lock_guard lock(mutex_);
    if (bytes > budget_) {
        counters_.refused();
        return;
    }
    const auto existing = std::ranges::find(entries_, key, &Entry::key);
    if (existing != entries_.end()) {
        resident_ -= existing->bytes;
        entries_.erase(existing);
    }
    while (!entries_.empty() && resident_ > budget_ - bytes) {
        resident_ -= entries_.back().bytes;
        counters_.evicted(entries_.back().bytes);
        entries_.pop_back();
    }
    entries_.push_front({key, std::move(frame), bytes, runtime::SourceWorkingSet::current()});
    resident_ += bytes;
    counters_.store();
}
void DecodedVideoCache::setByteBudget(const std::size_t budget) {
    static_cast<void>(ledger_.setCacheCeiling(this, budget));
}
void DecodedVideoCache::clear() {
    std::lock_guard lock(mutex_);
    // Only an explicit refresh (a user purge) clears the decoded planes.
    core::telemetry::addEvictedBytes(core::telemetry::EvictionCause::Purge, resident_);
    entries_.clear();
    resident_ = 0;
}
std::size_t DecodedVideoCache::residentBytes() const {
    std::lock_guard lock(mutex_);
    return resident_;
}
std::size_t DecodedVideoCache::byteBudget() const {
    std::lock_guard lock(mutex_);
    return budget_;
}
} // namespace bloom::media::video

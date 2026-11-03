#include <bloom/runtime/source_working_set.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::runtime {
namespace {

thread_local SourceUse tCurrent{};

} // namespace

SourceWorkingSet::Request::Request(std::shared_ptr<SourceWorkingSet> owner,
                                   const std::uint64_t composition, const std::int64_t frame)
    : owner_(std::move(owner)), use_{composition, frame, true} {
    if (!owner_)
        return;
    owner_->begin(use_);
    if (!tCurrent.tracked) {
        tCurrent = use_;
        marksThread_ = true;
    }
}

SourceWorkingSet::Request::Request(Request&& other) noexcept
    : owner_(std::move(other.owner_)), use_(other.use_), marksThread_(other.marksThread_) {
    other.marksThread_ = false;
}

SourceWorkingSet::Request& SourceWorkingSet::Request::operator=(Request&& other) noexcept {
    if (this != &other) {
        end();
        owner_ = std::move(other.owner_);
        use_ = other.use_;
        marksThread_ = std::exchange(other.marksThread_, false);
    }
    return *this;
}

void SourceWorkingSet::Request::end() noexcept {
    if (!owner_)
        return;
    if (marksThread_) {
        tCurrent = {};
        marksThread_ = false;
    }
    const auto owner = std::move(owner_);
    owner_.reset();
    try {
        owner->finish(use_);
        owner->trim();
    } catch (...) {
        // A failed release pass keeps entries; the next request's pass retries.
    }
}

SourceWorkingSet::UseScope::UseScope(const SourceUse use) noexcept : previous_(tCurrent) {
    tCurrent = use;
}

SourceWorkingSet::UseScope::~UseScope() { tCurrent = previous_; }

SourceUse SourceWorkingSet::current() noexcept { return tCurrent; }

std::int64_t SourceWorkingSet::frameAt(const std::int64_t timeNumerator,
                                       const std::int64_t timeDenominator,
                                       const std::uint32_t rateNumerator,
                                       const std::uint32_t rateDenominator) noexcept {
    if (timeDenominator <= 0 || rateDenominator == 0)
        return 0;
    // A window bound, not an identity: long double keeps it exact well past any clip length.
    const auto frames = std::floor(static_cast<long double>(timeNumerator) * rateNumerator /
                                   (static_cast<long double>(timeDenominator) * rateDenominator));
    return static_cast<std::int64_t>(frames);
}

void SourceWorkingSet::begin(const SourceUse& use) {
    const std::lock_guard lock(mutex_);
    // A composition with no request in flight keeps its window only until another composition
    // starts rendering: its entries were released to that window when its last request ended, and
    // the window stays meanwhile for the read-ahead captures still landing after it. Without this
    // every composition ever rendered kept a window for the life of the cache.
    std::erase_if(windows_, [&use](const auto& entry) {
        return entry.first != use.composition && entry.second.inFlight.empty();
    });
    auto& window = windows_[use.composition];
    window.latest = use.frame;
    window.inFlight.insert(use.frame);
}

void SourceWorkingSet::finish(const SourceUse& use) {
    const std::lock_guard lock(mutex_);
    const auto found = windows_.find(use.composition);
    if (found == windows_.end())
        return;
    if (const auto position = found->second.inFlight.find(use.frame);
        position != found->second.inFlight.end())
        found->second.inFlight.erase(position);
}

std::size_t SourceWorkingSet::windowCount() const {
    const std::lock_guard lock(mutex_);
    return windows_.size();
}

bool SourceWorkingSet::retains(const SourceUse& use) const {
    if (!use.tracked)
        return true;
    const std::lock_guard lock(mutex_);
    const auto found = windows_.find(use.composition);
    if (found == windows_.end())
        return true;
    const auto& window = found->second;
    auto lower = window.latest;
    auto upper = window.latest;
    if (!window.inFlight.empty()) {
        lower = std::min(lower, *window.inFlight.begin());
        upper = std::max(upper, *window.inFlight.rbegin());
    }
    return use.frame >= lower && use.frame <= upper + kReadAheadFrames;
}

void SourceWorkingSet::addTrimmer(const void* owner, std::function<void()> trim) {
    const std::lock_guard lock(trimMutex_);
    std::erase_if(trimmers_, [owner](const auto& entry) { return entry.first == owner; });
    trimmers_.emplace_back(owner, std::move(trim));
}

void SourceWorkingSet::removeTrimmer(const void* owner) {
    const std::lock_guard lock(trimMutex_);
    std::erase_if(trimmers_, [owner](const auto& entry) { return entry.first == owner; });
}

void SourceWorkingSet::trim() {
    const std::lock_guard lock(trimMutex_);
    for (const auto& [owner, release] : trimmers_) {
        static_cast<void>(owner);
        if (release)
            release();
    }
}

} // namespace bloom::runtime

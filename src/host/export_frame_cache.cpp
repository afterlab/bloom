#include <bloom/host/export_frame_cache.hpp>

#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <functional>
#include <limits>

namespace bloom::host {
namespace {

[[nodiscard]] std::uint64_t saturatingAdd(const std::uint64_t a, const std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b ? std::numeric_limits<std::uint64_t>::max()
                                                             : a + b;
}

} // namespace

bool operator==(const ExportFrameReuseKeyV1& lhs, const ExportFrameReuseKeyV1& rhs) {
    const auto& a = lhs.frame;
    const auto& b = rhs.frame;
    const bool plans = a.plan == b.plan ||
                       (a.plan != nullptr && b.plan != nullptr && sameRenderedContent(*a.plan, *b.plan));
    return lhs.gpuRoute == rhs.gpuRoute && lhs.outputCommand == rhs.outputCommand &&
           a.time == b.time && a.output == b.output && a.colorIntent == b.colorIntent &&
           a.media == b.media && plans;
}

std::uint64_t ExportCachedFrameV1::bytes() const noexcept {
    std::uint64_t total = frame ? frame->processImage().pixelStorageByteCost() : 0;
    if (mediaDisplay)
        total = saturatingAdd(total, mediaDisplay->byteCost());
    if (display)
        total = saturatingAdd(total, display->byteCost());
    return total;
}

std::shared_ptr<ExportFrameCacheV1>
ExportFrameCacheV1::create(std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy) {
    std::shared_ptr<ExportFrameCacheV1> cache(new ExportFrameCacheV1(policy));
    if (policy) {
        policy->addRetentionReclaimer(
            [weak = std::weak_ptr<ExportFrameCacheV1>(cache)](
                const render::GpuProcessingPhaseDomain domain,
                const std::uint64_t shortfall) -> std::uint64_t {
                // Retained frames are host memory; a device or request-count wait gains nothing.
                if (domain == render::GpuProcessingPhaseDomain::Device ||
                    domain == render::GpuProcessingPhaseDomain::RequestCount)
                    return 0;
                const auto cache = weak.lock();
                return cache ? cache->evict(std::max<std::uint64_t>(shortfall, 1)) : 0;
            });
    }
    return cache;
}

ExportFrameCacheV1::ExportFrameCacheV1(std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy)
    : policy_(std::move(policy)) {}

std::uint64_t ExportFrameCacheV1::beginRun() noexcept {
    const std::lock_guard lock(mutex_);
    return nextRun_++;
}

std::size_t ExportFrameCacheV1::bucketOf(const ExportFrameReuseKeyV1& key) noexcept {
    std::size_t seed = std::hash<std::int64_t>{}(key.frame.time.numerator());
    const auto mix = [&seed](const std::size_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    };
    mix(std::hash<std::int64_t>{}(key.frame.time.denominator()));
    mix(std::hash<std::size_t>{}(static_cast<std::size_t>(key.frame.output.value())));
    mix(std::hash<std::size_t>{}(key.frame.media.size()));
    mix(key.gpuRoute ? 1U : 0U);
    return seed;
}

bool ExportFrameCacheV1::rootedHere(const ExportCachedFrameV1& frame) const noexcept {
    if (!policy_ || !frame.frame)
        return false;
    const auto root = policy_->rootIdentity();
    const auto rooted = [&root](const std::shared_ptr<render::PackedHostStorageAuthority>& authority) {
        return authority != nullptr && authority->rootIdentity() == root;
    };
    if (!rooted(frame.frame->processImage().storageAuthority()))
        return false;
    if (frame.mediaDisplay && !rooted(frame.mediaDisplay->authority()))
        return false;
    if (frame.display && !rooted(frame.display->authority()))
        return false;
    return true;
}

std::shared_ptr<const ExportCachedFrameV1> ExportFrameCacheV1::lookup(const ExportFrameReuseKeyV1& key,
                                                                      const std::uint64_t run) {
    const std::lock_guard lock(mutex_);
    const auto [first, last] = index_.equal_range(bucketOf(key));
    for (auto it = first; it != last; ++it) {
        const auto position = it->second;
        if (!(position->key == key))
            continue;
        position->run = run;
        entries_.splice(entries_.begin(), entries_, position);
        ++stats_.hits;
        return position->frame;
    }
    ++stats_.misses;
    return nullptr;
}

bool ExportFrameCacheV1::insert(ExportFrameReuseKeyV1 key,
                                std::shared_ptr<const ExportCachedFrameV1> frame,
                                const std::uint64_t run) {
    if (!frame || !rootedHere(*frame)) {
        const std::lock_guard lock(mutex_);
        ++stats_.refusals;
        return false;
    }
    const auto bytes = frame->bytes();
    const auto usable = policy_->usableCapacity();
    // Half the usable host memory at most: the rest stays for the work that runs beside the cache.
    const std::uint64_t bound = usable.hostReady ? usable.hostUsableBytes / 2 : 0;
    const std::lock_guard lock(mutex_);
    const auto bucket = bucketOf(key);
    const auto [first, last] = index_.equal_range(bucket);
    for (auto it = first; it != last; ++it) {
        if (it->second->key == key) {
            // An equal frame is already retained; keep the one held (its pixels are the same).
            it->second->run = run;
            entries_.splice(entries_.begin(), entries_, it->second);
            return true;
        }
    }
    if (bytes > bound) {
        ++stats_.refusals;
        return false;
    }
    // Room within the bound, taken only from frames other runs used least recently.
    std::uint64_t room = bound > stats_.bytes ? bound - stats_.bytes : 0;
    if (room < bytes) {
        std::uint64_t reclaimable = 0;
        for (auto it = entries_.rbegin(); it != entries_.rend() && room + reclaimable < bytes; ++it)
            if (it->run != run)
                reclaimable += it->bytes;
        if (room + reclaimable < bytes) {
            ++stats_.refusals;
            return false;
        }
        for (auto it = std::prev(entries_.end()); room < bytes;) {
            const bool atFront = it == entries_.begin();
            const auto previous = atFront ? it : std::prev(it);
            if (it->run != run) {
                room += it->bytes;
                ++stats_.evictions;
                removeLocked(it);
            }
            if (atFront)
                break;
            it = previous;
        }
    }
    entries_.push_front(Entry{.key = std::move(key), .frame = std::move(frame), .bytes = bytes, .run = run});
    index_.emplace(bucket, entries_.begin());
    stats_.bytes += bytes;
    ++stats_.insertions;
    return true;
}

void ExportFrameCacheV1::removeLocked(const Entries::iterator position) {
    const auto [first, last] = index_.equal_range(bucketOf(position->key));
    for (auto it = first; it != last; ++it) {
        if (it->second == position) {
            index_.erase(it);
            break;
        }
    }
    stats_.bytes -= std::min(stats_.bytes, position->bytes);
    entries_.erase(position);
}

std::uint64_t ExportFrameCacheV1::evict(const std::uint64_t bytes) {
    // The frames leave the cache under the lock; their pixels are released (and their bytes return
    // to the root) outside it, when the last reference drops.
    std::vector<std::shared_ptr<const ExportCachedFrameV1>> released;
    std::uint64_t dropped = 0;
    {
        const std::lock_guard lock(mutex_);
        while (!entries_.empty() && dropped < bytes) {
            const auto position = std::prev(entries_.end());
            dropped += position->bytes;
            released.push_back(std::move(position->frame));
            ++stats_.evictions;
            removeLocked(position);
        }
    }
    return dropped;
}

void ExportFrameCacheV1::clear() { (void)evict(std::numeric_limits<std::uint64_t>::max()); }

ExportFrameCacheStatsV1 ExportFrameCacheV1::stats() const {
    const std::lock_guard lock(mutex_);
    auto out = stats_;
    out.entries = entries_.size();
    return out;
}

} // namespace bloom::host

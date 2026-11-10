// #642: see vector_artwork_scene_cache.hpp.

#include <bloom/runtime/vector_artwork_scene_cache.hpp>

#include <bloom/document/data_block.hpp>
#include <bloom/svgartwork/svg_artwork.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/media/svg_scene.hpp>

#include <algorithm>
#include <variant>

namespace bloom::runtime {

VectorArtworkSceneCache::VectorArtworkSceneCache(const std::size_t byteBudget) : byteBudget_(byteBudget) {
    statistics_.byteBudget = byteBudget;
}

std::size_t VectorArtworkSceneCache::estimatedBytes(const media::SvgScene& scene) noexcept {
    // An anchor is a point and two optional handles; an element carries its paint, names and
    // geometry headers. Gradients are a few stops each.
    constexpr std::size_t kAnchorBytes = 64;
    constexpr std::size_t kElementBytes = 512;
    constexpr std::size_t kGradientBytes = 256;
    return sizeof(media::SvgScene) + scene.anchorCount * kAnchorBytes +
           (scene.elementCount + scene.shapeCount + scene.textCount + scene.groupCount) * kElementBytes +
           scene.gradients.size() * kGradientBytes;
}

VectorArtworkSceneCache::Lookup VectorArtworkSceneCache::scene(const document::DataBlockRecord& block,
                                                               const std::function<bool()>& cancelled) {
    Lookup lookup;
    if (!document::isVectorArtworkBlock(block)) {
        lookup.diagnostic = "The data block is not an SVG";
        return lookup;
    }
    const auto& payload = std::get<document::OpaqueExtensionPayload>(block.payload);
    const auto digest = block.provenance.contentDigest;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::ranges::find_if(entries_, [&](const Entry& entry) {
            return entry.digest == digest && entry.payloadBytes == payload.size();
        });
        if (found != entries_.end()) {
            ++statistics_.hits;
            entries_.splice(entries_.begin(), entries_, found);
            lookup.scene = entries_.front().scene;
            return lookup;
        }
        ++statistics_.misses;
    }
    lookup.scene = document::parseVectorArtworkBlock(block, lookup.diagnostic, cancelled);
    if (cancelled && cancelled()) {
        lookup.scene.reset();
        lookup.cancelled = true;
        return lookup;
    }
    if (!lookup.scene)
        return lookup;
    // Kept only under the digest the bytes really have.
    const auto actual = core::Sha256Hasher::hash(payload.bytes());
    const auto bytes = estimatedBytes(*lookup.scene);
    std::lock_guard lock(mutex_);
    ++statistics_.parses;
    if (!actual || *actual != digest)
        return lookup;
    if (bytes > byteBudget_ / 4) {
        ++statistics_.refusedAdmissions;
        return lookup;
    }
    // Another compile may have parsed the same block meanwhile.
    if (std::ranges::none_of(entries_, [&](const Entry& entry) {
            return entry.digest == digest && entry.payloadBytes == payload.size();
        })) {
        entries_.push_front(Entry{digest, payload.size(), bytes, lookup.scene});
        statistics_.retainedBytes += bytes;
        ++statistics_.entries;
        trimLocked();
    }
    return lookup;
}

void VectorArtworkSceneCache::trimLocked() {
    while (statistics_.retainedBytes > byteBudget_ && !entries_.empty()) {
        statistics_.retainedBytes -= entries_.back().bytes;
        --statistics_.entries;
        ++statistics_.retirements;
        entries_.pop_back();
    }
}

void VectorArtworkSceneCache::retire() {
    std::lock_guard lock(mutex_);
    statistics_.retirements += entries_.size();
    entries_.clear();
    statistics_.entries = 0;
    statistics_.retainedBytes = 0;
}

void VectorArtworkSceneCache::setByteBudget(const std::size_t bytes) {
    std::lock_guard lock(mutex_);
    byteBudget_ = bytes;
    statistics_.byteBudget = bytes;
    trimLocked();
}

VectorArtworkSceneCacheStatistics VectorArtworkSceneCache::statistics() const {
    std::lock_guard lock(mutex_);
    return statistics_;
}

VectorArtworkSceneCache& processVectorArtworkSceneCache() {
    static VectorArtworkSceneCache cache;
    return cache;
}

} // namespace bloom::runtime

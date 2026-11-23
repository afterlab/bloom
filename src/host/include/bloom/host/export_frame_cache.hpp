#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/runtime/composite_frame_cache.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace bloom::runtime {
class GpuProcessingPhasePolicy;
}

// #785: the frames an export evaluated, kept so that a later export of the same frames takes them
// instead of evaluating again: another output of the same job, the true-peak re-render of an
// output (#771), or the same export run again. The output's "Use cached frames" setting turns
// lookups and insertions on for its frames.
//
// An entry is the exact evaluated frame -- the scene-referred float process image and, when the GPU
// ran the export's display/view in the same readback, that display-referred payload -- never a
// rounded or display-mapped copy. A reused frame therefore reaches the analysis, the approval digest
// and the encoder as the same pixels a fresh evaluation gives, and the delivered file is the same.
// Viewer and RAM Preview frames are display-referred 8-bit products for the Viewer's own display and
// are never a source.
//
// The key is the full render identity (runtime::CompositeFrameKey: the deep compiled plan, which
// carries the #760 export background and every parameter, the time and output, the colour intent
// and the verified content of every media file the plan reads), plus the route a fresh evaluation
// of the export would take now: GPU or CPU reference, and the identity of the output colour command
// the GPU would run with the frame. Bypassed looks, proxies and ROIs are never cached.
//
// Memory: an entry holds frames whose pixels are charged to the export provider's processing root
// (their rooted backing), so retained frames stay visible to every admission. Frames whose backing
// is not rooted in that root are not retained. Retention is bounded to half the root's usable host
// memory, and a reclaimer registered on the root drops entries, least recently used first, whenever
// any work waits for host memory; dropping the last reference returns the bytes to the root.
// Insertions never evict frames the inserting export has used itself (so a range longer than the
// bound keeps its first frames for the next export rather than cycling them out). Thread-safe.
namespace bloom::host {

struct ExportFrameReuseKeyV1 final {
    runtime::CompositeFrameKey frame;
    // The route a fresh evaluation takes: true when the export's GPU device evaluates frames.
    bool gpuRoute = false;
    // The output colour command the GPU evaluation runs with the frame (zero: none, or the CPU route).
    core::Sha256Digest outputCommand{};

    // The plans compare by rendered content (runtime::sameRenderedContent): an edit elsewhere in the
    // document (a new export job, another composition) leaves this composition's frames reusable.
    friend bool operator==(const ExportFrameReuseKeyV1&, const ExportFrameReuseKeyV1&);
};

struct ExportCachedFrameV1 final {
    std::shared_ptr<const runtime::ProcessFrame> frame;
    // The device evaluated the frame (otherwise the CPU reference did).
    bool gpuEvaluated = false;
    // The display-referred media payload (non-PNG presets) and the RGBA8 display payload (PNG) the
    // GPU transferred with the frame; null when the frame carried none.
    std::shared_ptr<const render::FinalRgba32fStorage> mediaDisplay;
    std::uint32_t mediaDisplayWidth = 0;
    std::uint32_t mediaDisplayHeight = 0;
    core::Sha256Digest mediaDisplayCommand{};
    core::Sha256Digest mediaDisplayDigest{};
    std::shared_ptr<const render::FinalRgba8Storage> display;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    core::Sha256Digest displayCommand{};

    // Pixel bytes the entry keeps alive.
    [[nodiscard]] std::uint64_t bytes() const noexcept;
};

struct ExportFrameCacheStatsV1 final {
    std::size_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t insertions = 0;
    // Frames not retained: not rooted in the cache's root, or no room within the bound.
    std::uint64_t refusals = 0;
    std::uint64_t evictions = 0;
};

class ExportFrameCacheV1 final {
  public:
    // Registers the cache's reclaimer on `policy` (held weakly by the reclaimer).
    [[nodiscard]] static std::shared_ptr<ExportFrameCacheV1>
    create(std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy);
    ExportFrameCacheV1(const ExportFrameCacheV1&) = delete;
    ExportFrameCacheV1& operator=(const ExportFrameCacheV1&) = delete;

    // A new export's run serial: frames it looks up or inserts are its own for insertion eviction.
    [[nodiscard]] std::uint64_t beginRun() noexcept;
    // The entry for `key`, or null. A hit becomes the most recently used and belongs to `run`.
    [[nodiscard]] std::shared_ptr<const ExportCachedFrameV1> lookup(const ExportFrameReuseKeyV1& key,
                                                                    std::uint64_t run);
    // Retains `frame` under `key` when its pixels are rooted in this cache's root and it fits the
    // bound, evicting least-recently-used frames of other runs if needed. False when not retained.
    bool insert(ExportFrameReuseKeyV1 key, std::shared_ptr<const ExportCachedFrameV1> frame,
                std::uint64_t run);
    // Drops least-recently-used entries until `bytes` were dropped or none is left; returns the
    // bytes dropped.
    std::uint64_t evict(std::uint64_t bytes);
    void clear();
    [[nodiscard]] ExportFrameCacheStatsV1 stats() const;
    [[nodiscard]] const std::shared_ptr<runtime::GpuProcessingPhasePolicy>& policy() const noexcept {
        return policy_;
    }

  private:
    explicit ExportFrameCacheV1(std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy);
    struct Entry final {
        ExportFrameReuseKeyV1 key;
        std::shared_ptr<const ExportCachedFrameV1> frame;
        std::uint64_t bytes = 0;
        std::uint64_t run = 0;
    };
    using Entries = std::list<Entry>;
    [[nodiscard]] static std::size_t bucketOf(const ExportFrameReuseKeyV1& key) noexcept;
    [[nodiscard]] bool rootedHere(const ExportCachedFrameV1& frame) const noexcept;
    void removeLocked(Entries::iterator position);

    std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy_;
    mutable std::mutex mutex_;
    // Most recently used first.
    Entries entries_;
    std::unordered_multimap<std::size_t, Entries::iterator> index_;
    ExportFrameCacheStatsV1 stats_;
    std::uint64_t nextRun_ = 1;
};

// What one attempt needs to take part in reuse: the cache and the export's run serial.
struct ExportFrameReuseV1 final {
    std::shared_ptr<ExportFrameCacheV1> cache;
    std::uint64_t run = 0;
};

} // namespace bloom::host

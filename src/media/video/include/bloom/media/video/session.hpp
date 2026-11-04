#pragma once
#include <bloom/media/provider/worker_pool.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/source_working_set.hpp>
#include <chrono>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
namespace bloom::media::video {
[[nodiscard]] bool isVideoExtension(const std::filesystem::path& path);
using Cancel = std::function<bool()>;
// Revalidate: observe the original's identity (size, times, volume/file id) and reuse the verified
// snapshot while it matches the observation that bracketed its full hash; a changed or unknown
// identity captures and rehashes, so changed bytes report SourceChanged. Retained: reuse this
// session's last verified snapshot without touching the original.
enum class SourceRevisionPolicy : std::uint8_t { Revalidate, Retained };
// Live verified snapshots are shared by digest between sessions (non-owning index).
[[nodiscard]] std::size_t sharedSourceSnapshotCount();
// Recently used snapshots outside any composition root (thumbnails, probes, audio) are also kept
// alive for a short while after their last session ends, so a new session on an unchanged file
// adopts the copy instead of copying and hashing the whole file again. The retention is bounded
// (kRetainedSourceSnapshots files, kRetainedSourceSnapshotBytes, kRetainedSourceSnapshotIdle) and
// gives way at once to a capture that needs the space. Rooted snapshots are never retained.
inline constexpr std::size_t kRetainedSourceSnapshots = 4;
inline constexpr std::uint64_t kRetainedSourceSnapshotBytes = 4ULL << 30;
inline constexpr std::chrono::seconds kRetainedSourceSnapshotIdle{120};
[[nodiscard]] std::size_t retainedSourceSnapshotCount();
// Drops every retained snapshot (cache purge, tests). Live sessions keep their own.
void releaseRetainedSourceSnapshots();
struct SourceRevisionAccess final {
    SourceRevisionPolicy policy = SourceRevisionPolicy::Revalidate;
    bool allowRetention = true;
    platform::FileSnapshotStatistics* work = nullptr;
    bool* retained = nullptr;
    platform::ReadOnlyFileSnapshot::Progress progress{};
    std::size_t hostByteBudget = std::numeric_limits<std::size_t>::max();
};
struct FrameKey {
    provider::Digest content;
    std::uint32_t stream = 0;
    std::uint64_t frame = 0;
    std::uint32_t interpretation = 0;
    std::string inputColorSpaceId;
    std::string workingColorSpaceId;
    provider::Digest configRevision;
    // Preview hardware frames never satisfy a software (final-render) lookup, or vice versa.
    bool hardware = false;
    friend bool operator==(const FrameKey&, const FrameKey&) = default;
};
// Conservative resident estimate for one retained decode worker of a stream: decoder base plus
// 24 frames of 16-bit 4:4:4 planes (reference set, frame threads and the product), capped at the
// stream's media-v1 worker address space (provider::workerAddressSpaceBytes, issue #352). Used to
// admit retained sessions by memory.
[[nodiscard]] std::size_t estimatedDecodeWorkerBytes(std::uint32_t width,
                                                     std::uint32_t height) noexcept;
// Issue #352: the resident byte quota for source snapshots, derived from the free space of the
// snapshot spool volume instead of a fixed 32 GiB: three quarters of the space available there,
// never below 32 GiB (a capture beyond the real free space still fails with its I/O error) and
// never above four 1 TiB sources. `available` is the spool volume's free bytes.
[[nodiscard]] std::uint64_t sourceSnapshotQuotaBytes(std::uint64_t available) noexcept;
// The same for the default spool directory (the user cache), measured now.
[[nodiscard]] std::uint64_t defaultSourceSnapshotQuotaBytes();
// Which decoder produced a frame. Software is the reference; hardware serves preview and final
// rendering once its first frame proved byte-identical to it (ADR 0027, amended 2026-11-04).
struct FrameDecodeProvenance final {
    provider::Implementation implementation = provider::Implementation::Software;
    // True when hardware was requested and this frame came from the software fallback.
    bool fallback = false;
    // "software", "vaapi", "nvdec", or "software (fallback: <typed reason>)".
    std::string description = "software";
    // The serving provider (ADR 0035): "ffmpeg", "ffmpeg-vaapi", "ffmpeg-nvdec" or the platform
    // framework's id.
    // When the platform framework was retired for this source, `nativeFallback` names why.
    std::string provider = "ffmpeg";
    std::string nativeFallback;
};
struct FrameDecodeRoute final {
    // Preview and final rendering may use the host's hardware decoder (VA-API or NVDEC) for
    // H.264, HEVC and AV1 (ADR 0027, amended 2026-11-04). The worker proves its first hardware
    // frame byte-identical to the software reference; any refusal falls back to software for the
    // source permanently. Decoded-cache keys keep the route.
    bool preferHardware = false;
    FrameDecodeProvenance* provenance = nullptr;
};
// Independent resident byte budget. Callers reserve this budget through their memory ledger.
class DecodedVideoCache final {
  public:
    explicit DecodedVideoCache(std::size_t budget, runtime::MemoryBudgetLedger& ledger =
                                                       runtime::processMemoryBudgetLedger());
    ~DecodedVideoCache();
    [[nodiscard]] std::shared_ptr<const provider::FrameProduct> find(const FrameKey& key);
    void store(const FrameKey& key, std::shared_ptr<const provider::FrameProduct> frame);
    void setByteBudget(std::size_t budget);
    // "Purge media cache": drops every decoded video frame this cache retains. It only releases the
    // cache's own references; a frame already handed to a caller stays valid until that caller
    // releases it. Safe to call from any thread.
    void clear();
    [[nodiscard]] std::size_t residentBytes() const;
    [[nodiscard]] std::size_t byteBudget() const;
    // Follows the playback working set (ADR 0037, rule 3): a frame is tagged with the composition
    // frame of the request that last used it, and frames outside the window are released after
    // each request. Frames used outside any request keep the ordinary budget retention.
    void bindSourceWindow(std::shared_ptr<runtime::SourceWorkingSet> window);
    void releaseOutsideWindow();

  private:
    struct Entry {
        FrameKey key;
        std::shared_ptr<const provider::FrameProduct> frame;
        std::size_t bytes;
        runtime::SourceUse use{};
    };
    runtime::MemoryBudgetLedger& ledger_;
    mutable std::mutex mutex_;
    std::list<Entry> entries_;
    std::size_t budget_ = 0, resident_ = 0;
    runtime::MemoryPoolCounters counters_;
    std::shared_ptr<runtime::SourceWorkingSet> window_;
};
// Issue #402: a caller waiting for a session's one retained decoder. Concurrent preparation asks
// for neighbouring frames at once; serving them in wake-up order made a long-GOP decoder seek back
// to the previous keyframe and decode the group again for almost every frame.
struct FrameWaiter final {
    std::uint32_t stream = 0;
    std::uint64_t frame = 0;
    std::uint64_t ticket = 0;     // unique, increasing in arrival order
    std::uint64_t enrolledAt = 0; // grants made before this caller started waiting
};
inline constexpr std::uint64_t kFrameWaiterMaximumBypass = 16;
// The ticket of the waiter the decoder serves next: the nearest frame at or after the last granted
// frame of the same stream (where the decoder continues without a seek), else the lowest frame. A
// waiter passed over kFrameWaiterMaximumBypass times goes first (oldest first), so a backward
// request such as a loop restart waits a bounded number of frames. Empty when nobody waits.
[[nodiscard]] std::optional<std::uint64_t>
nextAdmittedFrameWaiter(std::span<const FrameWaiter> waiters,
                        std::optional<std::pair<std::uint32_t, std::uint64_t>> lastGranted,
                        std::uint64_t grants) noexcept;
// A decoder's frames in one stream as a request for `frame` finds them (#474): the last frame it
// was given and the frames queued for it. Both empty when it has none.
struct DecoderReach final {
    std::optional<std::uint64_t> nearest; // the greatest of them at or before `frame`
    std::optional<std::uint64_t> lowest;  // the lowest of them
};
// Blocking off-UI facade. One worker request per asset at a time: identical frame requests share
// the in-flight result and other requests wait (cancellably) rather than fail Busy. Waiting
// requests are admitted by nextAdmittedFrameWaiter. The caller's
// shared task owns admission and completion.
class VideoDecodeSession final {
  public:
    explicit VideoDecodeSession(
        std::filesystem::path source, std::string worker = defaultWorker(),
        std::shared_ptr<platform::FileSnapshotQuota> quota = {},
        std::filesystem::path spoolDirectory = {},
        std::shared_ptr<render::detail::PackedHostRetirementState> retirement = {},
        std::function<void(std::int64_t)> launched = {});
    ~VideoDecodeSession();
    VideoDecodeSession(const VideoDecodeSession&) = delete;
    VideoDecodeSession& operator=(const VideoDecodeSession&) = delete;
    [[nodiscard]] std::optional<provider::Unavailable>
    verifySource(const provider::Digest& expected, const Cancel& cancel = {}) const;
    [[nodiscard]] provider::Result<provider::ProbeResult> probe(const Cancel& cancel = {});
    [[nodiscard]] provider::Result<std::shared_ptr<const provider::FrameProduct>>
    frame(const provider::ProbeResult& source, std::uint32_t stream, std::uint64_t index,
          std::uint32_t interpretation, DecodedVideoCache* cache, const Cancel& cancel = {});
    [[nodiscard]] provider::Result<std::shared_ptr<const provider::FrameProduct>>
    frame(const provider::ProbeResult& source, std::uint32_t stream, std::uint64_t index,
          std::uint32_t interpretation, DecodedVideoCache* cache,
          std::string_view inputColorSpaceId, std::string_view workingColorSpaceId,
          const provider::Digest& configRevision, const Cancel& cancel = {},
          const SourceRevisionAccess& revision = {}, const FrameDecodeRoute& route = {});
    [[nodiscard]] provider::Result<provider::AudioBlock>
    audio(const provider::ProbeResult& source, std::uint32_t stream, std::uint64_t sample,
          std::uint32_t count, const Cancel& cancel = {});
    [[nodiscard]] provider::Result<provider::DemuxIndex>
    index(const provider::ProbeResult& source, const Cancel& cancel = {},
          const SourceRevisionAccess& revision = {});
    // How this session would serve `frame` of `stream` now (#474): Joins when that frame is the
    // one being decoded, Idle when nothing holds or waits for the decoder, Busy otherwise. A
    // snapshot for choosing among sessions of an intra-only source; it may change at once.
    enum class Occupancy : std::uint8_t { Idle, Joins, Busy };
    [[nodiscard]] Occupancy occupancy(std::uint32_t stream, std::uint64_t frame);
    // Where this session's decoder will be in `stream` (#474): the frames of `stream` it was last
    // given or has queued. A request at or after one of them is decoded on the way forward from
    // it; one before all of them sends the decoder back to a keyframe. A snapshot, like
    // occupancy().
    [[nodiscard]] DecoderReach reach(std::uint32_t stream, std::uint64_t frame);
    [[nodiscard]] static std::string defaultWorker();
    // The host's hardware decode route (Linux): NVDEC through FFmpeg's CUDA hwaccel when the
    // NVIDIA kernel driver is loaded, else VA-API when a DRM render node exists, else None.
    // BLOOM_VIDEO_HARDWARE_DECODE "vaapi" or "nvdec" names the route explicitly. macOS keeps its
    // VideoToolbox provider and Windows has no hardware decode route yet (both None).
    enum class HardwareRoute : std::uint8_t { None, Vaapi, Nvdec };
    [[nodiscard]] static HardwareRoute hardwareDecodeRoute();
    // Host-side capability screen and preference. A hardware decode worker is attempted only when
    // a route exists and BLOOM_VIDEO_HARDWARE_DECODE is "1" (or names the route), or
    // automatically: always for NVDEC, which measured faster than threaded software decode, and
    // for VA-API only when the host has at most kHardwareDecodeMaximumThreads hardware threads
    // (where threaded software decode stops winning). "0" disables hardware decode.
    [[nodiscard]] static bool hardwareDecodePossible();
    static constexpr unsigned kHardwareDecodeMaximumThreads = 4;

  private:
    provider::WorkerReply
    call(provider::Role role, const provider::ProbeResult* source, std::uint32_t stream,
         std::uint64_t position, std::uint32_t count, const Cancel& cancel,
         const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot = {},
         bool retainWorker = false, bool hardware = false);
    provider::WorkerReply
    callWorker(provider::MediaWorkerPool& workers, const provider::ProviderDeclaration& declaration,
               const provider::ProbeResult* source, std::uint32_t stream, std::uint64_t position,
               std::uint32_t count, const Cancel& cancel,
               const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
               bool retainWorker);
    [[nodiscard]] provider::Result<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>
    sourceRevision(std::optional<provider::Digest> expected, const Cancel& cancel,
                   const SourceRevisionAccess& access) const;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bloom::media::video

#pragma once
#include "footage_interpretation_pass.hpp"
#include <atomic>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/video_source_report.hpp>
#include <list>
#include <optional>
#include <string_view>
#include <utility>
namespace bloom::runtime::detail {
struct VideoSourceSelection {
    std::shared_ptr<const media::provider::FrameProduct> frame;
    std::shared_ptr<const color::CpuColorSpaceProcessor> inputProcessor;
    std::string cacheKey, warning;
    // Decode-only identity: asset content digest + video stream id + selected frame index. It
    // excludes the input/working colour space, config revision, display and proxy, because the raw
    // codec-side preparation does not depend on them. The GPU media colour split keys its raw video
    // upload on this identity, so a changed working space or display reuses the decoded frame.
    std::string decodeKey;
    std::string inputColorSpaceId;
    std::string resolvedInputColorSpaceName, inputColorSpaceWarning, workingColorSpaceId;
    core::Sha256Digest configRevision;
    std::uint32_t interpretation = 0;
    bool inputColorSpaceAutomatic = false;
    bool cancelled = false;
    platform::FileSnapshotStatistics sourceWork;
    bool retainedRevision = false;
    // "software", "vaapi" or "software (fallback: <reason>)" for the decoded frame.
    std::string decodeProvenance;
    // #381: the footage interpretation pass (alpha, deinterlace) the frame runs.
    std::optional<FootagePass> footagePass;
};
// What a request asks a source's sessions for (#474). A source decoded in order (long-GOP) has one
// session: its decoder keeps its position between requests, which the requests queue for. An
// intra-only source (every frame decodes on its own) is served by parallel sessions instead, each
// with its own decoder, so concurrent requests for different frames do not queue on one decoder.
struct VideoSessionUse final {
    bool independentFrames = false;
    std::uint32_t stream = 0;
    std::uint64_t frame = 0;
};
// True for the codecs whose every frame is a random-access point, matching the decode worker's
// intra-only list (slice-threaded decoders).
[[nodiscard]] bool videoFramesDecodeIndependently(std::string_view codec) noexcept;
class VideoSourceContext final {
  public:
    media::video::DecodedVideoCache cache{0};
    // An intra-only use is served by the session already decoding its frame, else an idle one,
    // else a new one while the source has fewer than one per hardware thread, else the least
    // occupied. Any other use gets the source's one session.
    [[nodiscard]] std::shared_ptr<media::video::VideoDecodeSession>
    session(const std::string& key, const std::filesystem::path& path, bool allowInsertion,
            std::size_t workerBytes = 0, VideoSessionUse use = {});
    // A retained session of `key` chosen as above, or null when there is none (or an intra-only
    // use should start another).
    [[nodiscard]] std::shared_ptr<media::video::VideoDecodeSession>
    retainedSession(const std::string& key, VideoSessionUse use = {});
    void discardFailedSession(const std::string& key,
                              const std::shared_ptr<media::video::VideoDecodeSession>& session);
    void bindRetirement(std::shared_ptr<render::detail::PackedHostRetirementState> retirement);
    // Removing the owning sessions also invalidates retained source authority. In-flight callers
    // may finish their immutable revision but cannot reinsert into this list after the boundary.
    void clear();
    void invalidateSources();
    // Signal-only for a rooted caller: closes source admission and queues session destruction.
    void beginShutdown();
    [[nodiscard]] std::uint64_t sourceSnapshotBytes() const { return quota_->residentBytes(); }
    [[nodiscard]] std::size_t sourceSnapshotCount() const { return quota_->residentFiles(); }
    // Retained sessions are admitted by memory, not a fixed count: their estimated worker bytes
    // must fit this budget (default usable memory, the ledger's automatic total: no fixed fraction,
    // ADR 0037). At least two sources are kept so two sources can alternate, and never more
    // sources than the source-snapshot file quota can retain (the parallel sessions of one
    // intra-only source share its snapshot).
    void setSessionByteBudget(std::size_t bytes);
    [[nodiscard]] std::size_t retainedSessionCount();
    // Decode route and colour note of the most recently selected sources (most recent first, at
    // most kReportedSources). Status surfaces read this; it is not evaluation output.
    void report(VideoSourceReport value);
    [[nodiscard]] std::vector<VideoSourceReport> reports() const;
    static constexpr std::size_t kReportedSources = 32;
    static constexpr std::size_t kSnapshotFiles = 8;
    static constexpr std::size_t kMinimumRetainedSessions = 2;

  private:
    std::mutex mutex_;
    mutable std::mutex reportMutex_;
    std::vector<VideoSourceReport> reports_;
    std::uint64_t reportSequence_ = 0;
    struct SessionEntry {
        std::string key;
        std::filesystem::path path;
        std::shared_ptr<media::video::VideoDecodeSession> session;
        std::size_t workerBytes = 0;
        // Intra-only use: the requests holding this session now, and the frame it was last
        // handed out for, so a request for the same frame joins it rather than decoding it twice.
        std::shared_ptr<std::atomic<std::size_t>> users =
            std::make_shared<std::atomic<std::size_t>>(0);
        std::optional<std::pair<std::uint32_t, std::uint64_t>> leasedFrame;
    };
    // Called with mutex_ held; moves evicted entries into `detached` for destruction unlocked.
    void admitLocked(std::list<SessionEntry>& detached);
    // Called with mutex_ held: the entry to serve `use`, or end() when there is none or an
    // intra-only use should start another session.
    std::list<SessionEntry>::iterator chooseLocked(const std::string& key,
                                                   const std::filesystem::path* path,
                                                   const VideoSessionUse& use);
    // Called with mutex_ held: moves `entry` to the front and hands its session out.
    std::shared_ptr<media::video::VideoDecodeSession>
    handOutLocked(std::list<SessionEntry>::iterator entry, const VideoSessionUse& use);
    std::size_t sessionByteBudget_ = 0;
    std::list<SessionEntry> sessions_;
    std::shared_ptr<platform::FileSnapshotQuota> quota_ =
        std::make_shared<platform::FileSnapshotQuota>(
            media::video::defaultSourceSnapshotQuotaBytes(), kSnapshotFiles);
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement_;
    std::uint64_t generation_ = 0;
    bool closed_ = false;
};
[[nodiscard]] VideoSourceSelection selectVideoSource(
    const CompiledVideoSource& source, core::RationalTime time, document::FrameRate rate,
    const std::filesystem::path& base, VideoSourceContext& context, bool useCache,
    const CancellationToken& cancel,
    const EvaluationColorIntent& colorIntent = EvaluationColorIntent::LinearRec709Scene,
    MediaSourceRevisionPolicy revisionPolicy = MediaSourceRevisionPolicy::Revalidate,
    bool allowInsertion = true,
    std::size_t hostByteBudget = std::numeric_limits<std::size_t>::max());
// The selected frame in the working space at the request's proxy scales: videoToSceneLinear, or
// with a footage pass the stored samples, the pass, the input colour and the proxy resample. The
// CPU reference for every route.
[[nodiscard]] media::provider::Result<render::Rgba32fImage>
convertSelectedVideo(const VideoSourceSelection& selection,
                     render::Rgba32fImageDescriptor composition, double horizontalScale,
                     double verticalScale, std::size_t budget, const std::function<bool()>& cancel);
} // namespace bloom::runtime::detail

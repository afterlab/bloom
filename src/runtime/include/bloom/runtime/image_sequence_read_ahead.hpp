#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/media/image.hpp>
#include <bloom/platform/source_store.hpp>
#include <bloom/runtime/source_working_set.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

class OperationCache;
struct ImageSequenceReadAheadState;

// Captures upcoming image-sequence files into an evaluator's operation cache on its own I/O
// workers. A frame's source read then overlaps earlier frames instead of running inside that
// frame's scene preparation, and several reads stay outstanding so a network share overlaps its
// latency. Captures are the same source revisions selection makes (exact bytes admitted by the file
// identity, ADR 0025), stored under the same keys, so a later selection finds them like any
// other cached revision. A request may also decode the member's native samples (#386), so the
// frame's own preparation neither reads nor decodes it; a decode whose frame the transport has
// passed is not run. Read-ahead never evicts: a file or decode that would not fit the cache's free
// budget is skipped. Thread-safe; request() never blocks.
class ImageSequenceReadAhead final {
  public:
    struct Request final {
        std::filesystem::path path;
        core::Sha256Digest digest;
        // #473: a member import stamped without reading it is captured against its stamp.
        std::optional<media::ImageSourceStamp> stamp;
        // The member's recorded digest (absent while it is pending its background digest).
        std::optional<core::Sha256Digest> recordedDigest;
        std::string revisionKey;
        std::size_t byteBudget = 0;
        // #718: the asset's override of local copies. A capture asks the cache's source store
        // (the local mirror) before the source, and so fills the mirror ahead of the playhead.
        platform::LocalCopyOverride localCopies = platform::LocalCopyOverride::Inherit;
        // The volume id of the sequence's files when the frame that asks already observed one
        // (FileIdentity::volume): the storage class is then known without touching the volume.
        std::optional<std::uint64_t> volume{};
        // The composition frame the capture is for (ADR 0037): its entry is tagged with it, so it
        // stays while that frame is in the playback window.
        SourceUse use{};
        // #386: also decode the captured member's native samples into the cache, with the raw
        // interpretation the frame's GPU upload decodes with. `layer` is the requested layer; it is
        // resolved against the member's own part, as selection resolves it.
        std::optional<media::ImageInterpretation> decode;
        // #549: with `decode`, the channels the frame's channel reads (a Cryptomatte's ranks, a
        // depth channel) take from the member's part, sorted: decoded in the same read as the
        // samples and stored under the part's channel-set key.
        std::vector<std::string> channels;
    };
    struct Statistics final {
        std::uint64_t queued = 0;
        // Captures a worker began.
        std::uint64_t started = 0;
        std::uint64_t captured = 0;
        std::uint64_t skipped = 0;
        std::uint64_t failed = 0;
        // Captures a frame took over before they started, and captures a frame waited for.
        std::uint64_t withdrawn = 0;
        std::uint64_t awaited = 0;
        // Times a worker with queued captures waited for frames' own source reads to finish.
        std::uint64_t deferred = 0;
        // #386: native-sample decodes stored ahead of their frame, and decodes not run because
        // the transport had already passed (or jumped away from) their frame.
        std::uint64_t decoded = 0;
        // #549: channel sets stored ahead of their frame.
        std::uint64_t channelsDecoded = 0;
        std::uint64_t decodesSkipped = 0;
        // Decodes not run because frames' own decodes already used the cores.
        std::uint64_t decodesYielded = 0;
        // #535: captures not queued because the sequence is on local storage.
        std::uint64_t localSkipped = 0;
    };

    // #535: read-ahead follows the storage. On a network or remote volume, overlapping reads hide
    // latency and were measured to help. On local storage they compete with the playhead's own
    // cold reads, so by default (Skip) a request for files on a local volume queues nothing and
    // the frames read their own sources. A volume that cannot be classified is treated as remote.
    enum class LocalStorage : unsigned char { Skip, Capture };
    void setLocalStorage(LocalStorage policy) noexcept;

    // A frame reading its own source file (#477). While any is held, workers start no capture
    // (a capture already reading continues), so the frames being prepared, the playhead frame
    // among them, read from storage first and the read-ahead uses the storage they leave idle.
    class ForegroundRead final {
      public:
        ForegroundRead() = default;
        ~ForegroundRead();
        ForegroundRead(ForegroundRead&& other) noexcept;
        ForegroundRead& operator=(ForegroundRead&& other) noexcept;
        ForegroundRead(const ForegroundRead&) = delete;
        ForegroundRead& operator=(const ForegroundRead&) = delete;

      private:
        friend class ImageSequenceReadAhead;
        explicit ForegroundRead(std::shared_ptr<struct ImageSequenceReadAheadState> state);
        void release() noexcept;
        std::shared_ptr<struct ImageSequenceReadAheadState> state_;
    };

    // A frame decoding its own source (#386). Decodes ahead of the playhead use only the cores the
    // frames being prepared leave: a worker runs no decode while the frames' decodes and its own
    // would exceed one per physical core (half the hardware threads), so it never slows the frames
    // the transport needs next.
    class ForegroundDecode final {
      public:
        ForegroundDecode() = default;
        ~ForegroundDecode();
        ForegroundDecode(ForegroundDecode&& other) noexcept;
        ForegroundDecode& operator=(ForegroundDecode&& other) noexcept;
        ForegroundDecode(const ForegroundDecode&) = delete;
        ForegroundDecode& operator=(const ForegroundDecode&) = delete;

      private:
        friend class ImageSequenceReadAhead;
        explicit ForegroundDecode(std::shared_ptr<struct ImageSequenceReadAheadState> state);
        void release() noexcept;
        std::shared_ptr<struct ImageSequenceReadAheadState> state_;
    };

    explicit ImageSequenceReadAhead(std::size_t workers = 4, std::size_t queueLimit = 32);
    // Cancels outstanding captures and joins the workers.
    ~ImageSequenceReadAhead();
    ImageSequenceReadAhead(const ImageSequenceReadAhead&) = delete;
    ImageSequenceReadAhead& operator=(const ImageSequenceReadAhead&) = delete;

    // Queues captures into `cache`, skipping files already cached, queued or in flight, and any
    // beyond the queue bound. The cache is held weakly; a cache that is gone skips its captures.
    // The calling thread's tracked frame (SourceWorkingSet::current()) is the transport's position:
    // a queued decode for a frame at or before it, or beyond its read-ahead window, is not run.
    void request(const std::shared_ptr<OperationCache>& cache, std::vector<Request> requests);
    // A frame that needs `revisionKey` now must not read it a second time beside a capture: a
    // queued capture of it is withdrawn (the frame reads the file itself, now), and a capture in
    // flight (with its decode) is waited for, so the file is read once and that read finishes
    // first. Returns true
    // after waiting for a capture (its revision is then in the cache unless it failed).
    // `cancelled` is polled while waiting.
    [[nodiscard]] bool claim(const std::string& revisionKey,
                             const std::function<bool()>& cancelled);
    // Holds back capture starts until the returned read is destroyed.
    [[nodiscard]] ForegroundRead foregroundRead();
    // Counts a frame's own decode until the returned scope is destroyed.
    [[nodiscard]] ForegroundDecode foregroundDecode();
    [[nodiscard]] Statistics statistics() const;

  private:
    using State = ImageSequenceReadAheadState;
    std::shared_ptr<State> state_;
};

} // namespace bloom::runtime

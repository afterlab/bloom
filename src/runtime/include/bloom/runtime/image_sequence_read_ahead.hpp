#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/source_working_set.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

class OperationCache;

// Captures upcoming image-sequence files into an evaluator's operation cache on its own I/O
// workers. A frame's source read then overlaps earlier frames instead of running inside that
// frame's scene preparation, and several reads stay outstanding so a network share overlaps its
// latency. Captures are the same source revisions selection makes (exact bytes admitted by the file
// identity, ADR 0025), stored under the same keys, so a later selection finds them like any
// other cached revision. Read-ahead never evicts: a file that would not fit the cache's free
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
        // The composition frame the capture is for (ADR 0037): its entry is tagged with it, so it
        // stays while that frame is in the playback window.
        SourceUse use{};
    };
    struct Statistics final {
        std::uint64_t queued = 0;
        std::uint64_t captured = 0;
        std::uint64_t skipped = 0;
        std::uint64_t failed = 0;
    };

    explicit ImageSequenceReadAhead(std::size_t workers = 4, std::size_t queueLimit = 32);
    // Cancels outstanding captures and joins the workers.
    ~ImageSequenceReadAhead();
    ImageSequenceReadAhead(const ImageSequenceReadAhead&) = delete;
    ImageSequenceReadAhead& operator=(const ImageSequenceReadAhead&) = delete;

    // Queues captures into `cache`, skipping files already cached, queued or in flight, and any
    // beyond the queue bound. The cache is held weakly; a cache that is gone skips its captures.
    void request(const std::shared_ptr<OperationCache>& cache, std::vector<Request> requests);
    [[nodiscard]] Statistics statistics() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace bloom::runtime

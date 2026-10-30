#pragma once

#include <bloom/media/provider/encode.hpp>
#include <bloom/output/output_analysis_attempt.hpp>

#include <filesystem>

namespace bloom::output {

enum class DpxExportWriteErrorCodeV1 : std::uint8_t {
    None,
    ProviderMissing,
    ProviderFailed,
    Cancelled,
    // The reopened file declared different colour tags than the encoder was asked to write.
    TagsDiffer,
};

struct DpxExportWriteResultV1 final {
    core::Sha256Digest artifactDigest{}, semanticDigest{};
    std::uint64_t artifactBytes = 0;
    bool written = false;
    bool cancelled = false;
    DpxExportWriteErrorCodeV1 error = DpxExportWriteErrorCodeV1::None;
    // What the reopen read back from the file's header.
    media::provider::ColourTags observedTags;

    [[nodiscard]] static DpxExportWriteResultV1 cancelledResult() noexcept {
        DpxExportWriteResultV1 result;
        result.cancelled = true;
        result.error = DpxExportWriteErrorCodeV1::Cancelled;
        return result;
    }
    [[nodiscard]] static DpxExportWriteResultV1
    failed(const DpxExportWriteErrorCodeV1 diagnostic) noexcept {
        DpxExportWriteResultV1 result;
        result.error = diagnostic;
        return result;
    }
};

// The tags a DPX header can carry for pixels in `outputColorSpaceId` (empty: the working space).
// The converted pixels are scene-linear, so the transfer is always linear; the header can name
// only a few primaries sets, and every other set is declared unspecified rather than guessed.
[[nodiscard]] media::provider::ColourTags dpxColourTagsV1(std::string_view outputColorSpaceId,
                                                          std::string_view workingColorSpaceId);

// Whether the media worker's FFmpeg carries the DPX encoder. A dependency prefix older than the
// lock does not; the worker then refuses a DPX session as Unavailable ("Encoder is not in the
// intake"), and tests report a skip with that reason.
[[nodiscard]] bool dpxEncoderAvailableV1() noexcept;

// DPX encoding and the reopen check run in the isolated media worker.
class DpxExportWriterV1 final {
  public:
    [[nodiscard]] DpxExportWriteResultV1
    run(const OutputAnalysisAttemptV1& attempt, const std::filesystem::path& destination,
        const runtime::CancellationToken& cancellation = {}) const noexcept;
};

} // namespace bloom::output

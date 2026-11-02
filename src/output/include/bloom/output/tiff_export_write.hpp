#pragma once

#include <bloom/media/image.hpp>
#include <bloom/media/provider/encode.hpp>
#include <bloom/output/output_analysis_attempt.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace bloom::output {

enum class TiffExportWriteErrorCodeV1 : std::uint8_t {
    None,
    ProviderMissing,
    ProviderFailed,
    Cancelled,
};

struct TiffExportWriteResultV1 final {
    core::Sha256Digest artifactDigest{}, semanticDigest{};
    std::uint64_t artifactBytes = 0;
    bool written = false;
    bool cancelled = false;
    TiffExportWriteErrorCodeV1 error = TiffExportWriteErrorCodeV1::None;

    [[nodiscard]] static TiffExportWriteResultV1 writtenResult() noexcept {
        return {.written = true, .cancelled = false, .error = TiffExportWriteErrorCodeV1::None};
    }
    [[nodiscard]] static TiffExportWriteResultV1 cancelledResult() noexcept {
        return {
            .written = false, .cancelled = true, .error = TiffExportWriteErrorCodeV1::Cancelled};
    }
    [[nodiscard]] static TiffExportWriteResultV1
    failed(const TiffExportWriteErrorCodeV1 diagnostic) noexcept {
        return {.written = false, .cancelled = false, .error = diagnostic};
    }
};

// The worker settings of one TIFF export. The pixels are display-referred RGB at full range, so
// the declared tags are the display's primaries and transfer with the identity (RGB) matrix and
// full range, never the encoder default BT.709 limited range (issue #465).
[[nodiscard]] media::provider::EncodeSettingsV1
tiffExportEncodeSettingsV1(std::uint32_t width, std::uint32_t height, std::string_view display);

// TIFF encoding and exact reopen verification run in the isolated media worker.
class TiffExportWriterV1 final {
  public:
    [[nodiscard]] TiffExportWriteResultV1
    run(const OutputAnalysisAttemptV1& attempt, const std::filesystem::path& destination,
        const media::ImageProvider* provider = nullptr,
        const runtime::CancellationToken& cancellation = {}) const noexcept;
};

} // namespace bloom::output

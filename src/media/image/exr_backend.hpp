#pragma once

#include <bloom/media/image.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bloom::media::detail {

[[nodiscard]] ImageResult<ImageProbe> probeExr(const std::filesystem::path& path,
                                               core::Sha256Digest contentDigest,
                                               const CancelImageWork& cancel);
[[nodiscard]] ImageResult<ImageProbe> probeExr(std::span<const std::byte> bytes,
                                               core::Sha256Digest contentDigest,
                                               const CancelImageWork& cancel);

[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::filesystem::path& path, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, std::size_t pixelBudget);
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(std::span<const std::byte> bytes, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, std::size_t pixelBudget,
          ImageChannelsAlongside* alongside = nullptr);

[[nodiscard]] ImageResult<std::shared_ptr<const render::ChannelSetImage>>
decodeExrChannels(std::span<const std::byte> bytes, const ImageChannelSelection& selection,
                  const CancelImageWork& cancel, const ImageProgress& progress,
                  std::size_t budget);

// #422: an all-HALF colour layer's samples, interleaved R, G, B, A binary16 (Y in lane 0 for a
// luminance layer; lane 3 unused without alpha), with the geometry and alpha association the
// float decode uses and the number of pixels its ingest policy would repair.
struct ExrHalfLayout final {
    std::int64_t dataX = 0;
    std::int64_t dataY = 0;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::int64_t displayX = 0;
    std::int64_t displayY = 0;
    std::uint64_t displayWidth = 0;
    std::uint64_t displayHeight = 0;
    bool luminance = false;
    bool hasAlpha = false;
    bool premultiplied = true;
};
struct ExrHalfSamples final {
    ExrHalfLayout layout;
    std::shared_ptr<const std::vector<std::byte>> bytes;
    std::uint64_t repairedPixels = 0;
};
// NativeSamplesUnavailable when a colour channel is not HALF or is subsampled.
[[nodiscard]] ImageResult<ExrHalfSamples>
decodeExrHalfSamples(std::span<const std::byte> bytes, const ImageInterpretation& interpretation,
                     const CancelImageWork& cancel, std::size_t budget,
                     ImageChannelsAlongside* alongside = nullptr);
// The pixels the float decode's ingest policy would repair, counted from the bits.
[[nodiscard]] std::uint64_t countExrHalfRepairs(std::span<const std::byte> bytes,
                                                const ExrHalfLayout& layout,
                                                bool storedSamples) noexcept;

} // namespace bloom::media::detail

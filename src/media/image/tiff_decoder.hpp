#pragma once

#include "stb_image_adapter.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bloom::media::detail {
// Classic or large-offset TIFF signature in either byte order.
[[nodiscard]] inline bool isTiffMagic(const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < 4)
        return false;
    const auto byte = [&](const std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    return (byte(0) == 'I' && byte(1) == 'I' &&
            ((byte(2) == 42U && byte(3) == 0U) || (byte(2) == 43U && byte(3) == 0U))) ||
           (byte(0) == 'M' && byte(1) == 'M' &&
            ((byte(2) == 0U && byte(3) == 42U) || (byte(2) == 0U && byte(3) == 43U)));
}

// Classic TIFF stills as 16-bit RGBA samples, the same product the PNG and JPEG adapter returns.
// Supported: 8- and 16-bit unsigned integer grey, grey with alpha, RGB and RGBA; either byte order;
// strips or tiles; uncompressed, Lempel-Ziv-Welch, Deflate and PackBits compression; no predictor
// or horizontal differencing. The first image of the file is read. Anything else is refused with a
// plain-language reason in `info.refusal`; no input, however malformed, reads outside `bytes`. Only the header and
// the first image's directory are read; no pixel data is decoded.
[[nodiscard]] bool tiffInfo(std::span<const std::byte> bytes, ImageInfo& info);

// Decodes the first image. Empty on malformed, truncated or over-budget data. Layout refusals are
// already reported by tiffInfo. `pixelBudget` is the same peak-admission budget the PNG adapter
// uses: final RGBA32F, this function's RGBA16 samples and its largest unit scratch must fit.
[[nodiscard]] std::vector<std::uint16_t>
tiffSamples(std::span<const std::byte> bytes, const ImageInfo& info, std::size_t pixelBudget);
} // namespace bloom::media::detail

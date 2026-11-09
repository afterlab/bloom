#pragma once

#include "stb_image_adapter.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bloom::media::detail {
// SMPTE ST 268-1 file magic, big-endian ("SDPX") or little-endian ("XPDS").
[[nodiscard]] inline bool isDpxMagic(const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < 4)
        return false;
    const auto byte = [&](const std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    return (byte(0) == 'S' && byte(1) == 'D' && byte(2) == 'P' && byte(3) == 'X') ||
           (byte(0) == 'X' && byte(1) == 'P' && byte(2) == 'D' && byte(3) == 'S');
}

// DPX stills as 16-bit RGBA samples, the same product the PNG, JPEG and TIFF adapters return.
// Supported: unsigned integer samples of 8, 10 (filled method A or B), 12 (filled method A or B) and
// 16 bits; RGB, RGBA, ABGR, luma and single-channel R, G, B or A elements, alone or as several
// image elements that together name each of R, G, B and A at most once (luma stands for all three
// colour channels); either byte order; top-to-bottom, left-to-right orientation. Samples are scaled
// to the 16-bit range by their own full code range and never reinterpreted: the transfer and
// colorimetric fields are returned in `info.dpx` and decide the colour space elsewhere. Anything
// else is refused with a plain-language reason in `info.refusal`, and no input, however malformed,
// reads outside `bytes`. Only the header is read; no pixel data is decoded.
[[nodiscard]] bool dpxInfo(std::span<const std::byte> bytes, ImageInfo& info);

// Decodes the image. Empty on malformed, truncated or over-budget data. Layout refusals are already
// reported by dpxInfo. `pixelBudget` is the PNG adapter's peak-admission budget: final RGBA32F and
// these RGBA16 samples must fit before anything is allocated.
[[nodiscard]] std::vector<std::uint16_t>
dpxSamples(std::span<const std::byte> bytes, const ImageInfo& info, std::size_t pixelBudget);
} // namespace bloom::media::detail

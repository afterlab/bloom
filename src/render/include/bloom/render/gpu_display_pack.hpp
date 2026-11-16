#pragma once

// #733: what a device display pack is asked and what it reports. The device packs a display frame
// in one of the two 4-byte layouts of display_pixel_packing.hpp and, in the same dispatch, reads
// two things no packed frame can carry: whether every pixel allows the 10:10:10:2 layout, and the
// frame counters (under, over and not finite, counted from the float display result before the
// pack clamps it). Both are a few words in the kernel's status block; no pixel is read back for
// them. docs/architecture/scopes.md ("The cached display frame") is the specification.
//
// One layout holds for a whole frame and the device learns which only by packing it. A pack asked
// for Rgb10A2 whose frame holds a pixel that does not allow it fails with a typed refusal and its
// words are never published; the caller packs that frame again as Straight8888. A pack asked for
// Straight8888 always yields valid words, and its report says whether the frame would have fit.

#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/image_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace bloom::render {

struct GpuDisplayPackRequest final {
    DisplayPixelLayout layout = DisplayPixelLayout::Straight8888;
    // The display's luma coefficients for the luma counters. Empty: the luma pair is absent.
    std::optional<DisplayLumaCoefficients> luma;

    friend constexpr bool operator==(const GpuDisplayPackRequest&,
                                     const GpuDisplayPackRequest&) noexcept = default;
};

struct GpuDisplayPackReport final {
    // The layout the words were packed in.
    DisplayPixelLayout layout = DisplayPixelLayout::Straight8888;
    // Every pixel of the frame allows Rgb10A2 (displayAlphaAllowsRgb10A2).
    bool allowsRgb10A2 = false;
    DisplayFrameCounters counters;

    // The layout the frame takes under the rule.
    [[nodiscard]] constexpr DisplayPixelLayout frameLayout() const noexcept {
        return allowsRgb10A2 ? DisplayPixelLayout::Rgb10A2 : DisplayPixelLayout::Straight8888;
    }
    // The packed words are the frame's pixels: always for Straight8888, and for Rgb10A2 only
    // when every pixel allowed it.
    [[nodiscard]] constexpr bool wordsValid() const noexcept {
        return layout == DisplayPixelLayout::Straight8888 || allowsRgb10A2;
    }

    friend constexpr bool operator==(const GpuDisplayPackReport&,
                                     const GpuDisplayPackReport&) noexcept = default;
};

// The status block every display pack kernel writes, as 32-bit words. The host zeroes it before
// the dispatch. Word 0 keeps its long-standing meaning: nonzero rejects the frame.
inline constexpr std::size_t kGpuDisplayPackStatusWords = 12;
inline constexpr std::size_t kGpuDisplayPackStatusBytes =
    kGpuDisplayPackStatusWords * sizeof(std::uint32_t);
inline constexpr std::size_t kGpuDisplayPackStatusErrors = 0;   // error bits of the kernel
inline constexpr std::size_t kGpuDisplayPackStatusMisfit = 1;   // nonzero: a pixel refuses Rgb10A2
inline constexpr std::size_t kGpuDisplayPackStatusUnder = 2;    // red, green, blue, luma
inline constexpr std::size_t kGpuDisplayPackStatusOver = 6;     // red, green, blue, luma
inline constexpr std::size_t kGpuDisplayPackStatusNotFinite = 10;
// Word 11 is reserved and stays zero.

// The layout word a kernel is pushed.
[[nodiscard]] constexpr std::uint32_t
gpuDisplayPackLayoutWord(const DisplayPixelLayout layout) noexcept {
    return layout == DisplayPixelLayout::Rgb10A2 ? 1U : 0U;
}

// Reads a finished pack's status block. `lumaCounted`: the pack was given luma coefficients.
[[nodiscard]] constexpr GpuDisplayPackReport
gpuDisplayPackReport(const std::span<const std::uint32_t, kGpuDisplayPackStatusWords> status,
                     const DisplayPixelLayout layout, const bool lumaCounted) noexcept {
    GpuDisplayPackReport report;
    report.layout = layout;
    report.allowsRgb10A2 = status[kGpuDisplayPackStatusMisfit] == 0U;
    for (std::size_t channel = 0; channel < kDisplayCounterChannels; ++channel) {
        report.counters.under[channel] = status[kGpuDisplayPackStatusUnder + channel];
        report.counters.over[channel] = status[kGpuDisplayPackStatusOver + channel];
    }
    report.counters.notFinite = status[kGpuDisplayPackStatusNotFinite];
    if (!lumaCounted) {
        report.counters.markLumaAbsent();
    }
    return report;
}

// The device image format of a layout is VK_FORMAT_R8G8B8A8_UNORM for Straight8888 and
// VK_FORMAT_A2B10G10R10_UNORM_PACK32 for Rgb10A2: the packed words are the image's texels as they
// are, so a transfer to the host copies them unchanged.

} // namespace bloom::render

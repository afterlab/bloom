#pragma once

// #733: the two 4-byte layouts of a packed display pixel and the bit-exact rules between them.
// docs/architecture/scopes.md ("The cached display frame") is the specification.
//
// A display frame is the display transform's result packed for the Viewer and the preview cache.
// Every pixel is one render::Rgba8 cell of four bytes, in one of two layouts for the whole frame:
//
//   Straight8888  red, green, blue and alpha, 8 bits each, straight (not premultiplied) color.
//   Rgb10A2       one 32-bit word, least significant byte first: red in bits 0..9, green in
//                 10..19, blue in 20..29, alpha in 30..31. A pixel is opaque (alpha 3) or
//                 transparent black (the word 0), so the color is straight and premultiplied at
//                 once. On a little-endian host this is VK_FORMAT_A2B10G10R10_UNORM_PACK32.
//
// A frame takes Rgb10A2 when every pixel allows it (displayAlphaAllowsRgb10A2), and Straight8888
// otherwise: a frame with soft edges or partial opacity keeps its 8-bit alpha, which two bits
// cannot hold. Both layouts cost the same four bytes per pixel.
//
// All arithmetic is binary32 in the order written (the project builds without contraction),
// except the 8-bit rule, which keeps the binary64 arithmetic it always had.

#include <bloom/render/image_types.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace bloom::render {

static_assert(std::endian::native == std::endian::little,
              "the packed display word is read and presented as a little-endian 32-bit value");

inline constexpr std::uint32_t kDisplayCode10Maximum = 1023;
inline constexpr std::uint32_t kDisplayAlpha2Opaque = 3;

// The packing's own identity strings: the 8-bit one is the long-standing "straight-rgba8" of the
// export and output packers, which this header does not change.
[[nodiscard]] constexpr std::string_view
displayPixelLayoutId(const DisplayPixelLayout layout) noexcept {
    switch (layout) {
    case DisplayPixelLayout::Straight8888:
        return "straight-rgba8";
    case DisplayPixelLayout::Rgb10A2:
        return "preview-rgb10-a2";
    }
    return {};
}

// The 32-bit word of a cell and back.
[[nodiscard]] constexpr std::uint32_t displayWord(const Rgba8 cell) noexcept {
    return static_cast<std::uint32_t>(cell.red) | (static_cast<std::uint32_t>(cell.green) << 8U) |
           (static_cast<std::uint32_t>(cell.blue) << 16U) |
           (static_cast<std::uint32_t>(cell.alpha) << 24U);
}
[[nodiscard]] constexpr Rgba8 displayCell(const std::uint32_t word) noexcept {
    return Rgba8{static_cast<std::uint8_t>(word & 0xFFU),
                 static_cast<std::uint8_t>((word >> 8U) & 0xFFU),
                 static_cast<std::uint8_t>((word >> 16U) & 0xFFU),
                 static_cast<std::uint8_t>((word >> 24U) & 0xFFU)};
}

// The fields of an Rgb10A2 cell.
struct Rgb10A2Codes final {
    std::uint32_t red = 0;   // 0..1023
    std::uint32_t green = 0; // 0..1023
    std::uint32_t blue = 0;  // 0..1023
    std::uint32_t alpha = 0; // 0..3

    friend constexpr bool operator==(const Rgb10A2Codes&, const Rgb10A2Codes&) noexcept = default;
};
[[nodiscard]] constexpr Rgb10A2Codes rgb10A2Codes(const Rgba8 cell) noexcept {
    const auto word = displayWord(cell);
    return Rgb10A2Codes{word & 0x3FFU, (word >> 10U) & 0x3FFU, (word >> 20U) & 0x3FFU,
                        (word >> 30U) & 0x3U};
}
[[nodiscard]] constexpr Rgba8 rgb10A2Cell(const Rgb10A2Codes codes) noexcept {
    return displayCell((codes.red & 0x3FFU) | ((codes.green & 0x3FFU) << 10U) |
                       ((codes.blue & 0x3FFU) << 20U) | ((codes.alpha & 0x3U) << 30U));
}

// The 10-bit pack rule: code = floor(clamp(v, 0, 1) * 1023 + 0.5). It is the scope code rule
// (runtime/scope_measure.hpp) inside the axis, so a stored code is the code a scope counts. A
// value that is not a number packs to 0.
[[nodiscard]] inline std::uint32_t displayCode10(const float value) noexcept {
    if (!(value > 0.0F)) {
        return 0;
    }
    if (!(value < 1.0F)) {
        return kDisplayCode10Maximum;
    }
    const float coded = value * 1023.0F;
    const float rounded = coded + 0.5F;
    return static_cast<std::uint32_t>(std::floor(rounded));
}

// The float a scope counts for a stored code: the correctly rounded code / 1023. Through the
// scope code rule it gives the same code back, for every code (tested).
[[nodiscard]] inline float displayCode10Value(const std::uint32_t code) noexcept {
    return static_cast<float>(code) / 1023.0F;
}

// The long-standing 8-bit pack rule ("straight-rgba8"): clamp, exact binary64, times 255,
// floor(value + 0.5). Unchanged; here so the two layouts' packers sit side by side.
[[nodiscard]] inline std::uint8_t displayCode8(const float value) noexcept {
    const double wide = static_cast<double>(value);
    const double clamped = !(wide > 0.0) ? 0.0 : (!(wide < 1.0) ? 1.0 : wide);
    return static_cast<std::uint8_t>(std::floor(clamped * 255.0 + 0.5));
}

// The layout rule, per pixel: a pixel fits Rgb10A2 when its alpha is exactly 0 (its color is then
// exact zero by the display flow) or packs to the 8-bit alpha 255. A frame is Rgb10A2 when every
// pixel fits.
[[nodiscard]] inline bool displayAlphaAllowsRgb10A2(const float alpha) noexcept {
    return alpha == 0.0F || displayCode8(alpha) == 255U;
}

// One pixel of each layout from the display transform's straight float result and the alpha.
[[nodiscard]] inline Rgba8 packStraight8888(const float red, const float green, const float blue,
                                            const float alpha) noexcept {
    return Rgba8{displayCode8(red), displayCode8(green), displayCode8(blue), displayCode8(alpha)};
}
// Requires displayAlphaAllowsRgb10A2(alpha).
[[nodiscard]] inline Rgba8 packRgb10A2(const float red, const float green, const float blue,
                                       const float alpha) noexcept {
    if (alpha == 0.0F) {
        return Rgba8{0, 0, 0, 0};
    }
    return rgb10A2Cell(Rgb10A2Codes{displayCode10(red), displayCode10(green), displayCode10(blue),
                                    kDisplayAlpha2Opaque});
}

// The Display signal a scope counts from a stored pixel: its color over black, as a 10-bit code.
// Rgb10A2: the stored code (0 for a transparent pixel). Straight8888: the 8-bit color times the
// 8-bit alpha on the 10-bit axis, round(c8 * a8 * 1023 / 65025), in whole numbers.
[[nodiscard]] constexpr std::array<std::uint32_t, 3>
displaySignalCodes(const DisplayPixelLayout layout, const Rgba8 cell) noexcept {
    if (layout == DisplayPixelLayout::Rgb10A2) {
        const auto codes = rgb10A2Codes(cell);
        if (codes.alpha != kDisplayAlpha2Opaque) {
            return {0U, 0U, 0U};
        }
        return {codes.red, codes.green, codes.blue};
    }
    const auto over = [alpha = static_cast<std::uint32_t>(cell.alpha)](const std::uint8_t color) {
        return (static_cast<std::uint32_t>(color) * alpha * 1023U * 2U + 65025U) / (2U * 65025U);
    };
    return {over(cell.red), over(cell.green), over(cell.blue)};
}
// The alpha a scope reads beside that signal (for Ignore Transparent Pixels): 0..1.
[[nodiscard]] inline float displaySignalAlpha(const DisplayPixelLayout layout,
                                              const Rgba8 cell) noexcept {
    if (layout == DisplayPixelLayout::Rgb10A2) {
        return rgb10A2Codes(cell).alpha == kDisplayAlpha2Opaque ? 1.0F : 0.0F;
    }
    return static_cast<float>(cell.alpha) / 255.0F;
}

// The float pixel of the signal picture a scope measure counts for a stored pixel: red, green and
// blue are the Display codes above as the correctly rounded code / 1023, alpha the stored alpha.
// The device computes the same floats (DisplayUnpackV1).
[[nodiscard]] inline std::array<float, 4> displaySignalPixel(const DisplayPixelLayout layout,
                                                             const Rgba8 cell) noexcept {
    const auto codes = displaySignalCodes(layout, cell);
    return {displayCode10Value(codes[0]), displayCode10Value(codes[1]),
            displayCode10Value(codes[2]), displaySignalAlpha(layout, cell)};
}

// Presentation on an 8-bit output: the 8-bit code shown for a stored 10-bit code is its top 8
// bits, code / 4 in whole numbers. A 10-bit output shows the code unchanged. The rule is the one
// the Viewer's painter applies to a 30-bit image it draws without a copy, so both presenters and
// every reader of a frame show the same code; rounding to nearest would need a converted copy of
// each frame on the interface thread. A value on an 8-bit step is shown as that step, and any
// other value is within one 8-bit code of its nearest.
[[nodiscard]] constexpr std::uint8_t presentedCode8(const std::uint32_t code10) noexcept {
    return static_cast<std::uint8_t>((code10 & 0x3FFU) >> 2U);
}
// The straight 8-bit pixel a Viewer shows on an 8-bit output for a stored pixel of either layout.
[[nodiscard]] constexpr Rgba8 presentedRgba8(const DisplayPixelLayout layout,
                                             const Rgba8 cell) noexcept {
    if (layout != DisplayPixelLayout::Rgb10A2) {
        return cell;
    }
    const auto codes = rgb10A2Codes(cell);
    if (codes.alpha != kDisplayAlpha2Opaque) {
        return Rgba8{0, 0, 0, 0};
    }
    return Rgba8{presentedCode8(codes.red), presentedCode8(codes.green),
                 presentedCode8(codes.blue), 255};
}

// What a stored frame cannot carry: how many pixels of the whole frame had a Display signal
// below 0 (under) or above 1 (over) before packing, per channel, and how many were not finite.
// Counted from the display transform's float result by every frame job, on both routes, and kept
// with the frame: nine 32-bit counts. The signal is the straight display color times alpha, and
// under and over are the scope code rule's: floor(v * 1023 + 0.5) below 0 (or not a number) and
// above 1023. Luma is (Kr r + Kg g) + Kb b of that signal with the display's coefficients; when
// they could not be resolved the luma pair holds kDisplayCounterAbsent.
inline constexpr std::uint32_t kDisplayCounterAbsent = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t kDisplayCounterChannels = 4; // red, green, blue, luma

struct DisplayFrameCounters final {
    std::array<std::uint32_t, kDisplayCounterChannels> under{};
    std::array<std::uint32_t, kDisplayCounterChannels> over{};
    std::uint32_t notFinite = 0;

    [[nodiscard]] constexpr bool lumaCounted() const noexcept {
        return under[3] != kDisplayCounterAbsent && over[3] != kDisplayCounterAbsent;
    }
    constexpr void markLumaAbsent() noexcept {
        under[3] = kDisplayCounterAbsent;
        over[3] = kDisplayCounterAbsent;
    }
    // Adds another part of the same frame (a chunk or a row band). Counts stop one short of the
    // absent marker; a luma pair absent on either side stays absent.
    constexpr void add(const DisplayFrameCounters& other) noexcept {
        const bool luma = lumaCounted() && other.lumaCounted();
        const auto sum = [](const std::uint32_t left, const std::uint32_t right) {
            constexpr auto limit = kDisplayCounterAbsent - 1U;
            return left > limit - std::min(right, limit) ? limit : left + right;
        };
        for (std::size_t channel = 0; channel < kDisplayCounterChannels; ++channel) {
            under[channel] = sum(under[channel], other.under[channel]);
            over[channel] = sum(over[channel], other.over[channel]);
        }
        notFinite = sum(notFinite, other.notFinite);
        if (!luma) {
            markLumaAbsent();
        }
    }

    friend constexpr bool operator==(const DisplayFrameCounters&,
                                     const DisplayFrameCounters&) noexcept = default;
};
static_assert(sizeof(DisplayFrameCounters) == 9 * sizeof(std::uint32_t),
              "the frame counters are nine 32-bit counts");

// The luma coefficients of the display's color space (the scope's own: runtime::ScopeLuma).
struct DisplayLumaCoefficients final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;

    friend constexpr bool operator==(const DisplayLumaCoefficients&,
                                     const DisplayLumaCoefficients&) noexcept = default;
};

namespace detail {
enum class DisplaySignalRange : std::uint8_t { Inside, Under, Over };
[[nodiscard]] inline DisplaySignalRange displaySignalRange(const float value) noexcept {
    const float coded = value * 1023.0F;
    const float rounded = coded + 0.5F;
    const float whole = std::floor(rounded);
    if (!(whole >= 0.0F)) {
        return DisplaySignalRange::Under;
    }
    if (!(whole <= 1023.0F)) {
        return DisplaySignalRange::Over;
    }
    return DisplaySignalRange::Inside;
}
[[nodiscard]] inline bool displayFiniteBits(const float value) noexcept {
    return (std::bit_cast<std::uint32_t>(value) & 0x7F800000U) != 0x7F800000U;
}
} // namespace detail

// Counts one pixel: `red`, `green` and `blue` are the display transform's straight float result
// (not clamped), `alpha` the pixel's alpha. Without `luma`, mark the luma pair absent before the
// frame is published. A frame has far fewer than 2^32 - 2 pixels, so no count here overflows.
inline void countDisplayPixel(DisplayFrameCounters& counters, const float red, const float green,
                              const float blue, const float alpha,
                              const std::optional<DisplayLumaCoefficients>& luma) noexcept {
    const float signalRed = red * alpha;
    const float signalGreen = green * alpha;
    const float signalBlue = blue * alpha;
    if (!detail::displayFiniteBits(signalRed) || !detail::displayFiniteBits(signalGreen) ||
        !detail::displayFiniteBits(signalBlue) || !detail::displayFiniteBits(alpha)) {
        ++counters.notFinite;
        return;
    }
    const auto count = [&counters](const std::size_t channel, const float value) {
        switch (detail::displaySignalRange(value)) {
        case detail::DisplaySignalRange::Under:
            ++counters.under[channel];
            break;
        case detail::DisplaySignalRange::Over:
            ++counters.over[channel];
            break;
        case detail::DisplaySignalRange::Inside:
            break;
        }
    };
    count(0, signalRed);
    count(1, signalGreen);
    count(2, signalBlue);
    if (luma.has_value() && counters.lumaCounted()) {
        const float redPart = luma->red * signalRed;
        const float greenPart = luma->green * signalGreen;
        const float sum = redPart + greenPart;
        const float bluePart = luma->blue * signalBlue;
        count(3, sum + bluePart);
    }
}

} // namespace bloom::render

#pragma once

// #733: what the device display pack qualifications share. A device pack is qualified against the
// CPU packers of display_pixel_packing.hpp in both four-byte layouts: its cells within the display
// transform's long-standing float allowance (one code of the layout's own scale) with alpha exact,
// its layout flag by the layout rule, and its frame counters equal. This header holds the cell
// comparison, the frame those qualifications pack and the luma coefficients they count with, so
// the neutral kernel, the resident display and the general display program are held to one rule.

#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/image_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bloom::render {

// Rec. 709 luma, the coefficients of the Bloom Neutral display.
inline constexpr DisplayLumaCoefficients kDisplayPackParityLuma{0.2126F, 0.7152F, 0.0722F};

// Packed cells of one layout agree: every color code within `toleranceCodes` (codes of 255 for
// Straight8888, codes of 1023 for Rgb10A2) and every alpha exact.
[[nodiscard]] constexpr bool displayCellsWithin(const DisplayPixelLayout layout,
                                                const std::span<const Rgba8> left,
                                                const std::span<const Rgba8> right,
                                                const std::uint32_t toleranceCodes) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    const auto near = [toleranceCodes](const std::uint32_t a, const std::uint32_t b) {
        return (a > b ? a - b : b - a) <= toleranceCodes;
    };
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (layout == DisplayPixelLayout::Rgb10A2) {
            const auto a = rgb10A2Codes(left[index]);
            const auto b = rgb10A2Codes(right[index]);
            if (a.alpha != b.alpha || !near(a.red, b.red) || !near(a.green, b.green) ||
                !near(a.blue, b.blue)) {
                return false;
            }
            continue;
        }
        if (left[index].alpha != right[index].alpha || !near(left[index].red, right[index].red) ||
            !near(left[index].green, right[index].green) ||
            !near(left[index].blue, right[index].blue)) {
            return false;
        }
    }
    return true;
}

// Two frames of the same picture agree: the same layout, every alpha exact, and every color code
// within the comparison's allowance. A comparison keeps the allowance it always had for a straight
// frame (`straightToleranceCodes`, in codes of 255: 0 where the two were held equal, 1 where the
// display transform's float difference was allowed); a 10:10:10:2 frame is held to one code of
// 1023, the same float difference at that scale and narrower than a code of 255.
inline constexpr std::uint32_t kDisplayRgb10A2ParityCodes = 1;
[[nodiscard]] constexpr bool displayFramesAgree(const DisplayPixelLayout leftLayout,
                                                const std::span<const Rgba8> left,
                                                const DisplayPixelLayout rightLayout,
                                                const std::span<const Rgba8> right,
                                                const std::uint32_t straightToleranceCodes) noexcept {
    return leftLayout == rightLayout &&
           displayCellsWithin(leftLayout, left, right,
                              leftLayout == DisplayPixelLayout::Rgb10A2 ? kDisplayRgb10A2ParityCodes
                                                                        : straightToleranceCodes);
}

// The frame a pack qualification packs: opaque grays below, inside and above the display axis, a
// few saturated colors, and exactly transparent pixels, so every counter counts something. Under
// the Bloom Neutral display no value lies near an end of the axis, so the float difference between
// a device and the CPU cannot move a pixel across a counter's edge and the counters must be equal.
// `soft` adds one half-transparent pixel, which no 10:10:10:2 frame can hold: the frame's layout
// is then Straight8888 and a 10:10:10:2 pack of it is refused.
[[nodiscard]] inline std::vector<Rgba32f> makeDisplayPackParityPixels(const std::size_t count,
                                                                      const bool soft) {
    constexpr std::array<float, 8> levels{-0.2F, -0.05F, 0.02F, 0.18F, 0.5F, 0.9F, 1.3F, 2.5F};
    const auto straight = [](const float red, const float green, const float blue,
                             const float alpha) {
        const auto value =
            Rgba32f::fromPremultiplied(red * alpha, green * alpha, blue * alpha, alpha);
        return value ? *value.value() : Rgba32f::transparent();
    };
    std::vector<Rgba32f> pixels;
    pixels.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (index % 5U == 3U) {
            pixels.push_back(Rgba32f::transparent());
            continue;
        }
        const float level = levels[index % levels.size()];
        pixels.push_back(straight(level, level, level, 1.0F));
    }
    const auto set = [&pixels](const std::size_t index, const Rgba32f value) {
        if (index < pixels.size()) {
            pixels[index] = value;
        }
    };
    set(1, straight(2.5F, 0.0F, 0.0F, 1.0F));
    set(2, straight(0.0F, 0.6F, 0.0F, 1.0F));
    set(4, straight(0.05F, 0.05F, 3.0F, 1.0F));
    set(5, straight(0.9F, 0.4F, 0.1F, 1.0F));
    if (soft) {
        set(7, straight(0.5F, 0.25F, 0.125F, 0.5F));
    }
    return pixels;
}

// The size the qualifications pack that frame at: an odd width and a few rows.
inline constexpr std::uint32_t kDisplayPackParityWidth = 43;
inline constexpr std::uint32_t kDisplayPackParityHeight = 3;

} // namespace bloom::render

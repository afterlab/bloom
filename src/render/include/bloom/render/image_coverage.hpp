#pragma once

#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace bloom::render {

// Flat RGBA matte extraction in scene-linear light. RGB selectors read straight colour; Luma reads
// the Rec. 709 luma of the premultiplied colour, so a matte's alpha feathers its luma coverage as
// a track matte does (ADR 0028). Transparent pixels have zero coverage. The result is always a
// finite unit coverage, including for negative and HDR source colour. These values also define the
// resident GPU kernel's channel selector and are deliberately independent of UI labels.
enum class ImageCoverageChannel : std::uint8_t { Alpha, Red, Green, Blue, Luma };

[[nodiscard]] inline float imageCoverage(const Rgba32f pixel,
                                         const ImageCoverageChannel channel) noexcept {
    if (channel == ImageCoverageChannel::Alpha)
        return pixel.alpha();
    if (pixel.alpha() <= 0.0F)
        return 0.0F;
    const double inverseAlpha = 1.0 / static_cast<double>(pixel.alpha());
    double value = 0.0;
    switch (channel) {
    case ImageCoverageChannel::Red:
        value = static_cast<double>(pixel.red()) * inverseAlpha;
        break;
    case ImageCoverageChannel::Green:
        value = static_cast<double>(pixel.green()) * inverseAlpha;
        break;
    case ImageCoverageChannel::Blue:
        value = static_cast<double>(pixel.blue()) * inverseAlpha;
        break;
    case ImageCoverageChannel::Luma:
        value = 0.2126 * static_cast<double>(pixel.red()) +
                0.7152 * static_cast<double>(pixel.green()) +
                0.0722 * static_cast<double>(pixel.blue());
        break;
    case ImageCoverageChannel::Alpha:
        return pixel.alpha();
    }
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

// The durable matte selector of Set Alpha and Multiply Alpha is one closed Integer enumeration in
// the convention of a compositor's track-matte modes: 0-4 select Alpha, Red, Green, Blue and Luma,
// and 5-9 select the same channels inverted. Every other value is refused.
struct MatteSelection final {
    ImageCoverageChannel channel = ImageCoverageChannel::Alpha;
    bool invert = false;
    friend bool operator==(const MatteSelection&, const MatteSelection&) = default;
};
inline constexpr std::int64_t kMatteSelectionStoredValueCount = 10;

[[nodiscard]] constexpr std::optional<MatteSelection>
matteSelectionFromStoredValue(const std::int64_t value) noexcept {
    if (value < 0 || value >= kMatteSelectionStoredValueCount)
        return std::nullopt;
    return MatteSelection{static_cast<ImageCoverageChannel>(value % 5), value >= 5};
}

// An inverted matte is the exact Float32 complement of the unit coverage, so an absent matte
// (zero coverage) inverts to full coverage. CPU and GPU both compute this one subtraction.
[[nodiscard]] inline float matteCoverage(const Rgba32f pixel,
                                         const MatteSelection selection) noexcept {
    const float coverage = imageCoverage(pixel, selection.channel);
    return selection.invert ? 1.0F - coverage : coverage;
}

// #532 Multiply Alpha's per-pixel amount: the matte coverage times the unit factor, one Float32
// product. Each premultiplied component is then multiplied by this amount and rounded once more,
// so the CPU reference and the resident ImageCombineV1 shader follow the same two roundings and
// alpha is bit-exact for every factor, not only a unit one.
[[nodiscard]] inline float multiplyAlphaAmount(const float coverage, const float factor) noexcept {
    const float amount = coverage * factor;
    return amount;
}

// #479 Separate Channels and Combine Channels: the STORED value of one channel, exactly as the
// premultiplied pixel holds it -- never divided by alpha, never clamped, with no colour
// conversion -- so a Raw data pass (depth, normals, motion vectors, IDs) and an HDR or signed value
// move between channels unchanged. Luma is the Rec. 709 luma of the stored (premultiplied) colour,
// the same Float64 products as the Luma matte selector before its clamp, rounded once.
[[nodiscard]] inline float storedImageChannel(const Rgba32f pixel,
                                              const ImageCoverageChannel channel) noexcept {
    switch (channel) {
    case ImageCoverageChannel::Alpha:
        return pixel.alpha();
    case ImageCoverageChannel::Red:
        return pixel.red();
    case ImageCoverageChannel::Green:
        return pixel.green();
    case ImageCoverageChannel::Blue:
        return pixel.blue();
    case ImageCoverageChannel::Luma:
        return static_cast<float>(0.2126 * static_cast<double>(pixel.red()) +
                                  0.7152 * static_cast<double>(pixel.green()) +
                                  0.0722 * static_cast<double>(pixel.blue()));
    }
    return 0.0F;
}

// #479 Combine Channels: `base` with its component `slot` (0 red, 1 green, 2 blue, 3 alpha) set to
// `value`. Alpha is limited to [0, 1] like every Rgba32f; red, green and blue are unbounded.
// `premultiply` then multiplies red, green and blue by the alpha (a Straight declaration). A zero
// alpha is transparent black, as Rgba32f::fromPremultiplied publishes it.
[[nodiscard]] inline ImageResult<Rgba32f> setImageChannel(const Rgba32f base, const float value,
                                                          const std::uint8_t slot,
                                                          const bool premultiply) noexcept {
    if (slot > 3 || !std::isfinite(value))
        return ImageResult<Rgba32f>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    std::array<float, 4> components{base.red(), base.green(), base.blue(), base.alpha()};
    components[slot] = value;
    components[3] = std::min(std::max(components[3], 0.0F), 1.0F);
    if (premultiply)
        for (std::size_t index = 0; index < 3; ++index)
            components[index] = components[index] * components[3];
    return Rgba32f::fromPremultiplied(components[0], components[1], components[2], components[3]);
}

// #486 Sharpen, one channel of the unsharp mask: the premultiplied `value` plus `amount` times its
// difference from `scale` times `blurred`, the difference first moved `threshold` towards zero. The
// result stays at or above min(value, 0), so sharpening never takes a colour below black, and a
// subnormal result is zero on every device. The device program performs the same binary32 operations.
[[nodiscard]] inline float unsharpImageChannel(const float value, const float blurred,
                                               const float scale, const float amount,
                                               const float threshold) noexcept {
    const float detail = std::fma(-scale, blurred, value);
    float cored = 0.0F;
    if (detail > threshold)
        cored = detail - threshold;
    else if (detail < -threshold)
        cored = detail + threshold;
    const float sharpened = std::fma(amount, cored, value);
    const float result = std::max(sharpened, std::min(value, 0.0F));
    return std::fpclassify(result) == FP_SUBNORMAL ? 0.0F : result;
}

// #486 Sharpen: `base` sharpened against `blurred`, its blur at the same pixel (transparent outside
// the blur's data window). Colour is compared straight, each image's own alpha taken out, so a flat
// colour stays flat through an alpha edge and only real detail is added, in proportion to alpha:
//   rgb' = rgb + amount * thresholded(rgb - a * blurred.rgb / blurred.a), threshold scaled by a.
// Alpha never changes, and a transparent pixel stays transparent. A non-finite result fails.
[[nodiscard]] inline ImageResult<Rgba32f> unsharpImagePixel(const Rgba32f base, const Rgba32f blurred,
                                                            const float amount,
                                                            const float threshold) noexcept {
    const float alpha = base.alpha();
    if (alpha <= 0.0F || blurred.alpha() <= 0.0F)
        return ImageResult<Rgba32f>::success(base);
    const float scale = alpha / blurred.alpha();
    const float scaled = alpha * threshold;
    return Rgba32f::fromPremultiplied(
        unsharpImageChannel(base.red(), blurred.red(), scale, amount, scaled),
        unsharpImageChannel(base.green(), blurred.green(), scale, amount, scaled),
        unsharpImageChannel(base.blue(), blurred.blue(), scale, amount, scaled), alpha);
}

// All operations use exact composition-space integer pixel centres. A matte tap outside its
// data window is transparent black and thus has zero coverage. The caller performs the window
// lookup; this helper never guesses alignment from array indices or resamples a matte.
[[nodiscard]] inline ImageResult<Rgba32f>
setImageAlpha(const Rgba32f image, const float coverage) noexcept {
    if (!std::isfinite(coverage) || coverage < 0.0F || coverage > 1.0F)
        return ImageResult<Rgba32f>::failure(ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    if (coverage == 0.0F)
        return ImageResult<Rgba32f>::success(Rgba32f::transparent());
    if (image.alpha() == 0.0F)
        return Rgba32f::fromPremultiplied(0.0F, 0.0F, 0.0F, coverage);
    const double scale = static_cast<double>(coverage) / static_cast<double>(image.alpha());
    const double red = static_cast<double>(image.red()) * scale;
    const double green = static_cast<double>(image.green()) * scale;
    const double blue = static_cast<double>(image.blue()) * scale;
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue) ||
        std::abs(red) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(green) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(blue) > static_cast<double>(std::numeric_limits<float>::max()))
        return ImageResult<Rgba32f>::failure(ImageError::codeOnly(ImageErrorCode::NonFiniteResult));
    return Rgba32f::fromPremultiplied(static_cast<float>(red), static_cast<float>(green),
                                     static_cast<float>(blue), coverage);
}

} // namespace bloom::render

// #484 Motion blur: the shared shutter model. docs/architecture/motion-blur.md, "The shutter".

#include "motion_shutter.hpp"

#include <bloom/document/motion_blur_parameters.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] std::int64_t powerOfTwoAtLeast(const std::int64_t value) noexcept {
    std::int64_t p = 1;
    while (p < value && p < kMotionMaxSamples)
        p *= 2;
    return p;
}

// The shutter curve's weight at u in [0, 1] of the open time.
[[nodiscard]] double curveWeight(const CompiledShutter& shutter, const double u) {
    switch (shutter.curve) {
    case document::kShutterCurveTriangle:
        return std::max(0.0, 1.0 - std::abs(2.0 * u - 1.0));
    case document::kShutterCurveGaussian: {
        const double z = (u - 0.5) * 6.0;
        return std::exp(-0.5 * z * z);
    }
    case document::kShutterCurveCustom: {
        // Linear between the points (x the open time, y the weight), held past the ends.
        const auto& anchors = shutter.customCurve.firstContour().anchors;
        if (anchors.empty())
            return 1.0;
        if (u <= anchors.front().point.x)
            return std::max(0.0, anchors.front().point.y);
        for (std::size_t i = 1; i < anchors.size(); ++i) {
            const auto a = anchors[i - 1].point;
            const auto b = anchors[i].point;
            if (u <= b.x) {
                const double t = b.x > a.x ? (u - a.x) / (b.x - a.x) : 1.0;
                return std::max(0.0, a.y + (b.y - a.y) * t);
            }
        }
        return std::max(0.0, anchors.back().point.y);
    }
    default:
        return 1.0;
    }
}

} // namespace

std::int64_t motionQualitySamples(const std::int64_t quality) noexcept {
    switch (quality) {
    case document::kMotionQualityDraft:
        return 4;
    case document::kMotionQualityStandard:
        return 16;
    case document::kMotionQualityHigh:
        return 64;
    default:
        return kMotionMaxSamples;
    }
}

std::array<double, 2> shutterInterval(const CompiledShutter& shutter) noexcept {
    const double s = shutter.shutter;
    switch (shutter.offset) {
    case document::kShutterOffsetStart:
        return {0.0, s};
    case document::kShutterOffsetEnd:
        return {-s, 0.0};
    case document::kShutterOffsetCustom:
        return {shutter.phase, shutter.phase + s};
    default:
        return {-0.5 * s, 0.5 * s};
    }
}

std::int64_t shutterSampleCount(const CompiledShutter& shutter, const double widestPixels) noexcept {
    if (shutter.quality != document::kMotionQualityFinal)
        return motionQualitySamples(shutter.quality);
    const auto low = powerOfTwoAtLeast(std::max<std::int64_t>(shutter.minSamples, 1));
    const auto high = std::max(low, powerOfTwoAtLeast(shutter.maxSamples));
    const double widest = std::isfinite(widestPixels) ? std::max(widestPixels, 0.0) : 0.0;
    const auto wanted = widest >= static_cast<double>(kMotionMaxSamples)
                            ? kMotionMaxSamples
                            : powerOfTwoAtLeast(static_cast<std::int64_t>(std::ceil(widest)));
    return std::clamp(wanted, low, high);
}

ShutterSamples shutterSamples(const CompiledShutter& shutter, const std::int64_t count) {
    ShutterSamples samples;
    const auto [open, close] = shutterInterval(shutter);
    samples.open = open;
    samples.close = close;
    const auto n = std::max<std::int64_t>(count, 1);
    double total = 0.0;
    for (std::int64_t k = 0; k < n; ++k) {
        const double u0 = static_cast<double>(k) / static_cast<double>(n);
        const double u1 = static_cast<double>(k + 1) / static_cast<double>(n);
        samples.start.push_back(open + (close - open) * u0);
        samples.end.push_back(open + (close - open) * u1);
        samples.centre.push_back(open + (close - open) * (u0 + u1) * 0.5);
        // The curve's mean over the stratum (16-point midpoint rule).
        double w = 0.0;
        for (int i = 0; i < 16; ++i)
            w += curveWeight(shutter, u0 + (u1 - u0) * (static_cast<double>(i) + 0.5) / 16.0);
        w /= 16.0;
        samples.weight.push_back(w);
        total += w;
    }
    for (auto& w : samples.weight)
        w = total > 0.0 ? w / total : 1.0 / static_cast<double>(n);
    return samples;
}

} // namespace bloom::runtime::detail

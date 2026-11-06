#pragma once

// #484 Motion blur: the shutter model every motion-blur node shares (the compiler's time samples
// and the planner's path and vector samples). docs/architecture/motion-blur.md, "The shutter".

#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace bloom::runtime::detail {

// The most time samples a shutter takes, and the sample counts of Draft, Standard and High.
inline constexpr std::int64_t kMotionMaxSamples = 64;
[[nodiscard]] std::int64_t motionQualitySamples(std::int64_t quality) noexcept;

// The shutter's samples over its open time, in frames relative to the frame: per stratum its start,
// centre and end and its weight (the shutter curve's mean over the stratum, normalised to sum 1).
struct ShutterSamples final {
    double open = 0.0;
    double close = 0.0;
    std::vector<double> start;
    std::vector<double> centre;
    std::vector<double> end;
    std::vector<double> weight;
};
[[nodiscard]] ShutterSamples shutterSamples(const CompiledShutter& shutter, std::int64_t count);
// The shutter's open and close times in frames.
[[nodiscard]] std::array<double, 2> shutterInterval(const CompiledShutter& shutter) noexcept;
// Final's adaptive count: the smallest power of two at least the widest displacement in pixels,
// within the shutter's minimum and maximum (each rounded to a power of two); the fixed count of the
// other qualities.
[[nodiscard]] std::int64_t shutterSampleCount(const CompiledShutter& shutter,
                                              double widestPixels) noexcept;

} // namespace bloom::runtime::detail

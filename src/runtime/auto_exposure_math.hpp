#pragma once

// Auto Exposure's logarithm and power, the same bits on every platform.
//
// The C library's float log2 and exp2 are free in their last bit, and the platforms' libraries do
// not all agree there, which made the CPU reference frame differ between them. Each is therefore
// worked in double and rounded to float once: a double result is far closer to the true value than
// half a float step, so the rounding lands on the nearest float on each platform.

#include <cmath>

namespace bloom::runtime::detail {

[[nodiscard]] inline float autoExposureLog2(const float value) noexcept {
    return static_cast<float>(std::log2(static_cast<double>(value)));
}

[[nodiscard]] inline float autoExposureExp2(const float value) noexcept {
    return static_cast<float>(std::exp2(static_cast<double>(value)));
}

} // namespace bloom::runtime::detail

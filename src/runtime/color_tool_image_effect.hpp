#pragma once

// #518 Tone Map (and, in later changes, #517 Color Matrix and #523 HSL Qualifier): a grade kind whose
// prepared table drives one per-pixel function of the input's straight color. The CPU reference
// (colorToolPixel) and the GPU program (colorToolProgramText) run the same binary32 operations in
// the same order over the same table, and every division is the correctly rounded one, so the two
// agree to within the last bits of the functions each backend supplies (none for Tone Map).
// docs/architecture/image-filters.md, "Tone Map", is the specification.

#include "color_grade_image_effect.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

// The packed table, a whole number of vec4s, the same size for every operation:
//   vec4 0  [operation, choice 0, choice 1, choice 2]
//   Tone Map  vec4 1 [knee, ceiling, ceiling - knee, 0]   vec4 2 [shadow recovery, shadow range, 0, 0]
//             vec4 3 [gamut threshold, gamut scale, 0, 0]
// choice 0 is the method (0 each channel, 1 brightest channel) and choice 1 whether gamut
// compression is on. The gamut scale is (limit - threshold) * (1 - threshold) / (limit - 1), the
// scale of the rational curve that maps a distance of `limit` to the edge.
inline constexpr std::size_t kColorToolFloats = 32;

// The kernel's operands sampled by sampleColorGradeOperands, in forEachColorGradeOperand order. Null
// when a value is outside its document domain or the kernel is malformed.
[[nodiscard]] std::optional<PreparedColorGrade> prepareColorToolGrade(const ColorToolKernel& kernel,
                                                                      std::span<const double> values);
// One premultiplied pixel, in place. False when the result is not finite.
[[nodiscard]] bool colorToolPixel(const std::vector<float>& table,
                                  std::array<float, 4>& pixel) noexcept;
[[nodiscard]] bool validColorToolProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& colorToolProgramText();

} // namespace bloom::runtime::detail

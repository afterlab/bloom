#pragma once

// #481 Image Math, Image Map Range and Image Clamp: a grade kind whose prepared table drives one
// per-pixel function of the input and, for Image Math, an operand that is a number or the pixel of a
// second image at the same position. The CPU reference (pixelMathPixel) and the GPU program
// (pixelMathProgramText) run the same binary32 operations in the same order over the same table;
// the power's logarithm and exponential are each backend's own, so the two agree to within their
// last bits. docs/architecture/image-filters.md, "Image Math, Map Range and Clamp", is the
// specification.

#include "color_grade_image_effect.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ImageEffectResult;

// The packed table, nine vec4s:
//   [operation, flags, channel mask, 0]   operation: PixelMathOperation; flags: 1 straight color,
//                                         2 clamp the result to the output range, 4 reads a second
//                                         image; channel mask: 1 red, 2 green, 4 blue, 8 alpha
// then, for red, green, blue and alpha in turn,
//   [p0, p1, p2, p3] [low, high, 0, 0]
// where an arithmetic operation reads its operand in p0, a Map Range its input minimum, the scale
// (the output span over the input span, 0 for an empty input range) and its output minimum in
// p0..p2 and the ordered output range in low and high, and a Clamp its limits in low and high.
// Image Compare (operation 10) keeps its operands in the first two channels' p0..p3: [mode, gain,
// the split normal's x, y] and [split position, cell size in pixels, 0, 0].
inline constexpr std::size_t kPixelMathFloats = 36;
// With a second image, its geometry follows: the width of the output window, which is also the
// offset in the dispatched atlas at which the second image's window begins (0: the second image has
// no pixels).
inline constexpr std::size_t kPixelMathProgramFloats = 40;
// #349 Image Compare, which lays Split and Checkerboard on the display window, also carries where
// the dispatch lies in it: the display-relative position of the dispatch window's first pixel (x
// and y) and the display window's size.
inline constexpr std::size_t kPixelMathCompareProgramFloats = 44;

inline constexpr float kPixelMathStraight = 1.0F;
inline constexpr float kPixelMathClampFlag = 2.0F;
inline constexpr float kPixelMathSecondFlag = 4.0F;

// The kernel's operands sampled by sampleColorGradeOperands, in forEachColorGradeOperand order.
// Null when a value is outside its document domain or the kernel is malformed.
[[nodiscard]] std::optional<PreparedColorGrade> preparePixelMathGrade(const PixelMathKernel& kernel,
                                                                      std::span<const double> values);
// Whether the table reads a second image.
[[nodiscard]] bool pixelMathReadsSecond(const std::vector<float>& table) noexcept;
// Where a pixel lies in the display window, for Image Compare: its display-relative position and the
// display window's size. Other operations ignore it.
struct PixelMathPlace final {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 1;
    std::int64_t height = 1;
};
// One premultiplied pixel, in place; `second` is the second image's pixel (null: none, transparent
// black) and is read only when the table reads one. Always succeeds for a finite pixel.
[[nodiscard]] bool pixelMathPixel(const std::vector<float>& table, std::array<float, 4>& pixel,
                                  const std::array<float, 4>* second,
                                  const PixelMathPlace* place = nullptr) noexcept;
// The table with the second image's atlas geometry appended; `place` is the position of the dispatch
// window's first pixel in the display window (width and height: the display window's size), which
// an Image Compare needs and the other operations do not read.
[[nodiscard]] std::optional<std::vector<float>>
pixelMathProgramData(const PreparedColorGrade& grade, std::int64_t outputWidth,
                     const PixelMathPlace* place = nullptr);
[[nodiscard]] bool validPixelMathProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& pixelMathProgramText();

// The CPU reference over a whole image. The second image (null: none) is read by global pixel
// position and is transparent black outside its data window.
[[nodiscard]] ImageEffectResult
applyPixelMath(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
               const render::Rgba32fImage* second, std::size_t budget, const CancellationToken&,
               OperationIndex, const EvaluationProgressCallback&);

} // namespace bloom::runtime::detail

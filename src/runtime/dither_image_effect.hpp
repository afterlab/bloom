#pragma once

// #527 Dither: a grade kind whose table gains the input's geometry where it is applied. The CPU
// reference (ditherPixel) and the GPU program (ditherProgramText) run the one dither kernel
// (dither_kernel.glsl) over the same table, so they agree within 2e-6 on RGB with exact alpha, as the
// native harness holds them. The table holds the amplitude, the domain, the pattern's ranks and each
// channel's placement (from the seed and, Every Frame, the frame), so a frame's dither has its own
// cache identity and a held one is shared by every frame. docs/architecture/image-filters.md,
// "Dither", is the specification.

#include "color_grade_image_effect.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ImageEffectResult;

// The prepared table's floats before the ranks, and the program's (with the geometry).
inline constexpr std::size_t kDitherHeaderFloats = 16;
inline constexpr std::size_t kDitherProgramHeaderFloats = 20;

// `values`: the amplitude, then the frame the dither is drawn for (sampleColorGradeOperands; 0 when
// it is held).
[[nodiscard]] std::optional<PreparedColorGrade> prepareDitherGrade(const DitherKernel& kernel,
                                                                   std::span<const double> values);
// The table with the geometry of an image whose data window is `dataWindow`: a pixel's threshold is
// the full-resolution pixel's under its centre, wherever the data window, a region of interest or a
// proxy scale puts it. Null when that position cannot be represented.
[[nodiscard]] std::optional<std::vector<float>>
ditherProgramData(const PreparedColorGrade& grade, const render::ImageWindow& dataWindow,
                  double horizontalScale, double verticalScale);
[[nodiscard]] bool validDitherProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& ditherProgramText();
// One premultiplied pixel at the data window's index (x, y), in place. False when not finite.
[[nodiscard]] bool ditherPixel(std::span<const float> data, std::int64_t x, std::int64_t y,
                               std::array<float, 4>& pixel) noexcept;
// The CPU reference over the input's data window.
[[nodiscard]] ImageEffectResult
applyDither(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
            double horizontalScale, double verticalScale, std::size_t budget,
            const CancellationToken&, OperationIndex, const EvaluationProgressCallback&,
            CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail

#pragma once

// #486 Film Grain: a grade kind whose table gains the input's geometry where it is applied. The CPU
// reference (filmGrainPixel) and the GPU program (filmGrainProgramText) run the same integer hash
// and the same binary32 operations over the same 20 floats, so they agree within 2e-6 on RGB with
// exact alpha, as the native harness holds them. Every value in the table but the stream is a
// parameter; the stream comes from the seed and the frame, so a frame's grain has its own cache
// identity. docs/architecture/image-filters.md, "Film Grain", is the specification.

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

inline constexpr std::size_t kFilmGrainTableFloats = 16;
inline constexpr std::size_t kFilmGrainProgramFloats = 20;

// `values`: the kernel's Scalars in table order, then the frame the grain is drawn for
// (sampleColorGradeOperands; 0 when it is held).
[[nodiscard]] std::optional<PreparedColorGrade>
prepareFilmGrainGrade(const FilmGrainKernel& kernel, std::span<const double> values);
// The table with the geometry of an image whose data window is `dataWindow`: a pixel's grain is read
// where the pixel lies in full-resolution square pixels, wherever the data window, a region of
// interest or a proxy scale puts it. Null when that position cannot be represented.
[[nodiscard]] std::optional<std::vector<float>>
filmGrainProgramData(const PreparedColorGrade& grade, const render::ImageWindow& dataWindow,
                     double pixelAspect, double horizontalScale, double verticalScale);
[[nodiscard]] bool validFilmGrainProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& filmGrainProgramText();
// One premultiplied pixel at the data window's index (x, y), in place. False when not finite.
[[nodiscard]] bool filmGrainPixel(const float* data, std::int64_t x, std::int64_t y,
                                  std::array<float, 4>& pixel) noexcept;
// The CPU reference over the input's data window.
[[nodiscard]] ImageEffectResult
applyFilmGrain(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
               double horizontalScale, double verticalScale, std::size_t budget,
               const CancellationToken&, OperationIndex, const EvaluationProgressCallback&,
               CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail

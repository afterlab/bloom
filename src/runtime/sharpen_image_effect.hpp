#pragma once

// #486 Sharpen: an unsharp mask. The node lowers to a Gaussian blur of its input with the frame edge
// clamped (the separable blur primitive) and this kernel over the input and that blur. Its CPU
// reference is render::unsharpImagePixel and its GPU program the resident ImageCombineV1 Unsharp mode,
// the same binary32 operations on both, so a changed amount or threshold keeps the blur.
// docs/architecture/image-filters.md, "Sharpen", is the specification.

#include "color_grade_image_effect.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace bloom::runtime::detail {

struct ImageEffectResult;

// `values`: the kernel's amount, threshold and radius (forEachColorGradeOperand). No amount or no
// radius is an identity. The prepared table holds the amount, then the threshold.
[[nodiscard]] std::optional<PreparedColorGrade>
prepareSharpenGrade(const SharpenKernel& kernel, std::span<const double> values);
[[nodiscard]] float sharpenAmount(const PreparedColorGrade& grade) noexcept;
[[nodiscard]] float sharpenThreshold(const PreparedColorGrade& grade) noexcept;
// The CPU reference over the input's data window. `blurred` (null: no image) is read at the same
// global pixel, transparent outside its data window; the input's windows are kept.
[[nodiscard]] ImageEffectResult
applySharpen(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
             const render::Rgba32fImage* blurred, std::size_t budget, const CancellationToken&,
             OperationIndex, const EvaluationProgressCallback&, CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail

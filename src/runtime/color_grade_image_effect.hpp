#pragma once

// #340 Levels, Curves and Hue/Saturation: the one CPU reference of the three grades, shared by the
// CPU evaluator and the resident GPU program generator. The host prepares one packed binary32 table
// per grade; the CPU applies it here and the GPU program reads the identical table from its
// storage buffer and runs the same binary32 operations, so a keyed value changes only data, never
// the shader. No grade performs a colour transform: a Raw data pass is graded as stored.

#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ResolvedEvaluation;

enum class ColorGradeKind : std::uint8_t { Levels, Curves, HueSaturation };

// The packed table, always a whole number of vec4s:
//   Levels:  [clamp, 0, 0, 0], then per channel (master, red, green, blue, alpha)
//            [pivot, scale, inverse gamma, output black] [output range, step, identity, 0]
//   Curves:  [clamp, 0, 0, 0], then per channel
//            [knot count, identity, 0, 0] [k0, k1, k2, k3] [k4, 0, 0, 0] and kCurveSegments
//            segment polynomials [a, b, c, d]
//   Hue/Saturation: the three rows of the RGB matrix [m0, m1, m2, 0]
// A Levels channel maps x to t = (x - pivot) * scale (or the step x >= pivot when the input range
// is empty), raises a positive t to the inverse gamma, and returns fma(t, range, black). A Curves
// channel selects segment s = the number of knots <= x, measures dx from knot max(s - 1, 0) and
// evaluates fma(dx, fma(dx, fma(dx, d, c), b), a); the first and last segments are the linear
// extrapolations along the end tangents.
inline constexpr std::size_t kCurveSegments = 6;
struct PreparedColorGrade final {
    ColorGradeKind kind = ColorGradeKind::Levels;
    std::vector<float> data;
    // Every channel is the exact identity and nothing clamps: the input passes through unchanged.
    bool identity = false;
    friend bool operator==(const PreparedColorGrade&, const PreparedColorGrade&) = default;
};

// The kernel's operands in forEachColorGradeOperand order, a Vector 2 as (x, y). Null when one is
// not evaluable this frame.
[[nodiscard]] std::optional<std::vector<double>>
sampleColorGradeOperands(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                         const ResolvedEvaluation& resolved);
// Null for a non-grade kernel, a value outside its document domain (non-finite, |v| > 1e6, gamma
// outside [0.01, 100], saturation outside [0, 1000]) or Curves points too close to represent.
[[nodiscard]] std::optional<PreparedColorGrade> prepareColorGrade(const ImageEffectKernel& kernel,
                                                                  std::span<const double> values);
// One premultiplied pixel, in place. False when the result (or the straight colour Levels and
// Curves grade) is not finite.
[[nodiscard]] bool colorGradePixel(const PreparedColorGrade& grade,
                                   std::array<float, 4>& pixel) noexcept;
// The prepared table as cache identity bytes.
[[nodiscard]] std::string colorGradeIdentity(const PreparedColorGrade& grade);

struct ImageEffectResult;
// Grades every pixel of the input's data window; the windows and pixel aspect are preserved.
[[nodiscard]] ImageEffectResult applyColorGrade(const PreparedColorGrade&,
                                                const render::Rgba32fImage&, std::size_t budget,
                                                const CancellationToken&, OperationIndex,
                                                const EvaluationProgressCallback&);
// One fixed neighbourhood program per kind; the table is its storage-buffer data.
[[nodiscard]] render::OcioGpuProgramResult buildColorGradeProgram(ColorGradeKind kind,
                                                                  const std::vector<float>& data);
} // namespace bloom::runtime::detail

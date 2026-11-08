#pragma once

// #525 Fields: the host side of a field program (bloom/core/field_program.hpp). A compiled
// FieldProgram names each instruction's operation, registers and constant choices; its animatable
// operands are sampled per frame (forEachFieldOperand order, then the clock time in seconds) and
// encoded here into the one binary32 table the CPU reference and the GPU program both interpret.
// docs/architecture/fields.md is the specification; each operation's operand order:
//
//   Noise     choices [type, fractal, seed, flow, loop, invert, overflow, bipolar, stream]
//             scalars [rotation, octaves, lacunarity, gain, octave rotation, ridge offset,
//                      ridge gain, evolution, loop length, contrast, brightness]
//             vectors [size, offset, octave offset]
//   Voronoi   choices [mode, metric, seed, octaves, selector, edges]
//             scalars [size, jitter, smoothness, evolution]
//   Random    choices [seed, smooth, gaussian, selector]; scalars [evolution]; vectors [cell size]
//   Pattern   choices [shape, waveform]; scalars [size, width, rotation, phase, speed, softness];
//             vectors [centre]
//   Gradient  choices [mode, repeat]; scalars [cycle]; vectors [start, end]
//   Shape     choices [source, primitive, falloff curve, invert, sides]
//             scalars [rotation, corner radius, inner ratio, falloff]; vectors [centre, size]
//   Image     choices [filter, edge, selector, channel, pair first, pair second]
//   Force     choices [mode, falloff, clockwise]; scalars [strength, radius, angle];
//             vectors [centre]; the curve when the falloff is Curve
//   Math      choices [operation]; scalars [smoothness, angle]
//   Remap     choices [clamp, invert, steps, vector]; scalars [in low, in high, out low, out high,
//             bias, gain]; the curve when it is not the identity
//   Transform choices [mode, segments]; scalars [rotation, warp amount];
//             vectors [translate, scale, pivot, tile]
//   Swizzle   choices [four lanes]; scalars [four scales]
//   Combine   scalars [the four default lanes]
//   Offset    choices [sign x, sign y]; scalars [step]
//   Derivative choices [mode]; scalars [step, strength]
//   Cell Centre scalars [spacing]; Arrow scalars [spacing, length per unit, half width];
//   Streak    scalars [step, noise cell]; Ramp scalars [low, high] and the ramp
//   Lens Warp choices [reverse]; scalars [k1, k2, p1, p2, scale]; vectors [centre, y down]
//   Constant  scalars [the four lanes]; Coordinate nothing

#include "color_grade_image_effect.hpp"

#include <bloom/core/field_program.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

// The table of `program` from its sampled operands; `values` ends with the clock time in seconds.
// Null when an operand is outside the domain its operation can represent.
[[nodiscard]] std::optional<std::vector<float>>
prepareFieldTable(const FieldProgram& program, std::span<const double> values,
                  std::uint32_t samples);
[[nodiscard]] std::optional<PreparedColorGrade> prepareFieldRasterGrade(const FieldRasterKernel&,
                                                                        std::span<const double>);
// A structural check of the full program data (core::field::validFieldTable).
[[nodiscard]] bool validFieldProgramData(const std::vector<float>& data) noexcept;
// The fixed GPU program text: the kernel, its GLSL dialect and the entry point.
[[nodiscard]] const std::string& fieldRasterProgramText();

// Where a raster's target and image slots lie in the image it is evaluated over.
struct FieldRasterPlacement final {
    // The target's data window origin and extent on the evaluation canvas.
    std::int64_t originX = 0;
    std::int64_t originY = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    // The target's position in the dispatched image (the GPU atlas; 0 on the CPU).
    std::int64_t targetX = 0;
    std::int64_t targetY = 0;
    double compositionWidth = 0.0;
    double compositionHeight = 0.0;
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    struct Slot final {
        std::int64_t originX = 0;
        std::int64_t originY = 0;
        std::int64_t width = 0;
        std::int64_t height = 0;
        std::int64_t atlasX = 0;
        std::int64_t atlasY = 0;
    };
    // Absent slots have width 0.
    std::array<Slot, core::field::kMaxFieldImages> slots{};
};
// The prepared table with its geometry, the data the CPU reference and the GPU program both read.
[[nodiscard]] std::optional<std::vector<float>>
fieldRasterProgramData(const PreparedColorGrade& grade, const FieldRasterPlacement& placement);

// The CPU reference: every pixel of `canvas`'s data window replaced by the raster; `slots` are the
// image slots (null: no image).
[[nodiscard]] ImageEffectResult
applyFieldRaster(const PreparedColorGrade& grade, const render::Rgba32fImage& canvas,
                 std::span<const render::Rgba32fImage* const> slots, double compositionWidth,
                 double compositionHeight, double horizontalScale, double verticalScale,
                 std::size_t budget, const CancellationToken&, OperationIndex,
                 const EvaluationProgressCallback&, CpuRowBandExecutor* rowBands);

// #525 Field Sample: the program's value at field position (x, y), the operands sampled as above.
[[nodiscard]] std::optional<std::array<float, 4>>
sampleFieldProgram(const FieldProgram& program, std::span<const double> values, double x,
                   double y);

// --- Mask distance ----------------------------------------------------------------------------

// The reach in evaluation pixels, at most kMaximumMaskDistanceReach.
inline constexpr std::int64_t kMaximumMaskDistanceReach = 512;
[[nodiscard]] std::int64_t maskDistanceReach(const MaskDistanceKernel&, double horizontalScale,
                                             double verticalScale) noexcept;
// The output window: the input's grown by the reach on every side.
[[nodiscard]] std::optional<render::ImageWindow> maskDistanceWindow(render::ImageWindow input,
                                                                    std::int64_t reach);
[[nodiscard]] ImageEffectResult applyMaskDistance(const MaskDistanceKernel&,
                                                  const render::Rgba32fImage& mask,
                                                  double horizontalScale, double verticalScale,
                                                  std::size_t budget, const CancellationToken&,
                                                  OperationIndex, const EvaluationProgressCallback&);
// The two GPU passes over the input padded to the output window: pass 0 the squared distance to the
// nearest inside (red) and outside (green) texel along each column, pass 1 along each row, then the
// signed distance. Data: [reach, cap, pass, 0].
[[nodiscard]] render::OcioGpuProgramResult buildMaskDistanceProgram(std::int64_t reach, int pass);

} // namespace bloom::runtime::detail

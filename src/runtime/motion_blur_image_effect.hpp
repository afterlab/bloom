#pragma once

// #484 Motion blur: the one temporal-sampling engine's pixel work, shared by the CPU evaluator and
// the resident GPU program generator. Every motion kernel (an accumulator of time samples, the path
// sampler, the motion-vector stages, the optical flow solver) is a grade kind (ColorGradeKind::
// Motion): the host prepares the sampled operands, and where it is applied the planner below turns
// the kernel, those operands and the input windows into a backend-neutral pass plan. A plan is a
// list of steps over numbered buffers, each a window of RGBA binary32 pixels:
//   Place: a zero image over the step's window with each source moved exactly by its shift;
//   Run:   the motion kernel (motion_kernel.glsl) over every pixel of the source with the table;
//   Crop:  the source cut to the step's window.
// The CPU interpreter (applyMotion) and the GPU emission (gpu_scene_motion_emission.ipp: moves and
// Normal blends, one fixed program per Run, crops) execute the same plan, so both evaluate the same
// binary32 operations over identical data. docs/architecture/motion-blur.md is the specification.

#include "color_grade_image_effect.hpp"
#include "motion_shutter.hpp"

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
struct ResolvedEvaluation;

// The largest table a pass reads: the path sampler's 64 samples of four vec4s after its header.
inline constexpr std::size_t kMotionTableFloats = 4 * (20 + 4 * 64);
// The operands of a motion kernel at the operation's clock time `time`: [time in seconds, frame
// index], the kernel's operands in forEachColorGradeOperand order, then for a Transform path the
// number of shutter rows and per row the transform's seven operands (translate, pivot, scale,
// rotation) at that row's time. Null when one is not evaluable.
[[nodiscard]] std::optional<std::vector<double>>
sampleMotionOperands(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                     const ResolvedEvaluation& resolved, std::optional<core::RationalTime> time);
// The prepared table: the operands and every constant of the kernel (its cache identity).
[[nodiscard]] std::optional<PreparedColorGrade> prepareMotionGrade(const ImageEffectKernel& kernel,
                                                                   std::span<const double> values);
[[nodiscard]] bool validMotionProgramData(const std::vector<float>& data) noexcept;
// The GPU program of one pass (the pass table's vec4 0 x): the kernel with only that pass reachable.
[[nodiscard]] const std::string& motionProgramText(int pass);

// --- The pass plan --------------------------------------------------------------------------------

struct MotionRect final {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
    friend bool operator==(const MotionRect&, const MotionRect&) = default;
};
struct MotionPlacement final {
    std::uint32_t buffer = 0;
    std::int64_t shiftX = 0;
    std::int64_t shiftY = 0;
};
enum class MotionStepKind : std::uint8_t { Place, Run, Crop };
struct MotionStep final {
    MotionStepKind kind = MotionStepKind::Run;
    std::uint32_t target = 0;
    MotionRect window{};
    std::vector<MotionPlacement> sources{};
    std::uint32_t source = 0;
    std::vector<float> table{};
};
struct MotionPassPlan final {
    // Buffers 0 .. inputs - 1 are the inputs (an absent input is never read).
    std::uint32_t inputs = 0;
    std::uint32_t buffers = 0;
    std::vector<MotionStep> steps;
    // The result buffer; absent when the effect publishes no image.
    std::optional<std::uint32_t> output;
    std::vector<MotionRect> windows;
};
// Where the effect is applied: the display window of the colour input and its pixel aspect at the
// evaluation resolution, the evaluation scales, and the full-resolution centre of the input's
// content bounds (Transform 2D's pivot origin).
struct MotionGeometry final {
    MotionRect display{};
    double pixelAspect = 1.0;
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    double centreX = 0.0;
    double centreY = 0.0;
};
// The images each kernel reads, in plan input order: Accumulate [accumulator, sample]; the path
// sampler [input]; a vector stage [input, then VectorBlurKernel::slotImages]; optical flow
// [input, then OpticalFlowKernel::slotImages]. An absent image has an empty window.
[[nodiscard]] std::size_t motionInputCount(const ImageEffectKernel& kernel) noexcept;
[[nodiscard]] std::optional<MotionPassPlan> planMotion(const ImageEffectKernel& kernel,
                                                       const PreparedColorGrade& grade,
                                                       std::span<const MotionRect> inputs,
                                                       const MotionGeometry& geometry);

// The CPU reference: the plan executed over `inputs` (null: absent), the output with the colour
// input's display window and pixel aspect.
[[nodiscard]] ImageEffectResult applyMotion(const ImageEffectKernel& kernel,
                                            const PreparedColorGrade& grade,
                                            std::span<const render::Rgba32fImage* const> inputs,
                                            const MotionGeometry& geometry, std::size_t budget,
                                            const CancellationToken&, OperationIndex,
                                            const EvaluationProgressCallback&,
                                            CpuRowBandExecutor* rowBands);

// One pixel of the motion kernel at atlas pixel (x, y) over `atlas` (row-major RGBA, width x
// height), for the tests.
[[nodiscard]] std::array<float, 4> motionKernelPixel(std::span<const float> table,
                                                     std::span<const float> atlas,
                                                     std::int64_t width, std::int64_t height,
                                                     int x, int y);

} // namespace bloom::runtime::detail

#pragma once

// Private to src/runtime. #484 Motion blur as chains of the one temporal-sampling engine's kernels,
// shared by the snapshot compiler (the production caller), the GPU coverage fixtures, the native
// parity harness and the tests, so the evidence covers the production chains. A time-sampled
// shutter is TimeSample operations (Offset clocks) summed by TemporalAccumulate; Motion Blur
// (Optical Flow) is four flow solves, their forward-backward consistency and the layered splat.
// docs/architecture/motion-blur.md is the specification.

#include "compositing_recipes.hpp"

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/motion_blur_parameters.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

// `frames` of a `rate` frame grid as an exact time (to a millionth of a frame). Null beyond a
// million frames or when not finite.
[[nodiscard]] std::optional<core::RationalTime> motionFramesToTime(document::FrameRate rate,
                                                                   double frames);

// `input` time-sampled at each of `offsets` (frames of `rate`) and combined by the accumulator
// with `weights` (the first sample's weight on the first accumulation).
[[nodiscard]] std::optional<OperationIndex>
appendTimeSamples(RecipeEmitter& emitter, document::FrameRate rate, OperationIndex input,
                  const std::vector<double>& offsets, const std::vector<double>& weights,
                  AccumulateOperator op, bool holdFootage, std::int64_t posterizeNumerator,
                  std::int64_t posterizeDenominator);

// `root` copied with every operation on a path to a substituted operation re-pointed at the
// substitutes (the operations nothing substituted reaches are shared).
[[nodiscard]] OperationIndex
cloneWithSubstitutes(OperationIndex root, const std::map<std::size_t, OperationIndex>& substitutes,
                     std::vector<CompiledOperation>& operations);

// The vectors of an Image input: forward RG as [x, y, alpha, 1] (alpha marking valid vectors),
// backward BA as [x, y, 0, 1].
[[nodiscard]] std::array<OperationIndex, 2> splitVectorImage(RecipeEmitter& emitter,
                                                             OperationIndex image);

// A vector stage reading `slots` (VectorBlurKernel slot -> image).
[[nodiscard]] OperationIndex
appendVectorStage(RecipeEmitter& emitter, OperationIndex input, VectorBlurKernel kernel,
                  const std::vector<std::pair<std::size_t, OperationIndex>>& slots);

// The optical flow from `from` to `to` (the flow's layer reads `mask` as its matte).
[[nodiscard]] OperationIndex appendFlowSolve(RecipeEmitter& emitter, OperationIndex from,
                                             OperationIndex to, OpticalFlowKernel kernel,
                                             std::optional<OperationIndex> mask = {});

// The optical flow kernel a node's method, quality and border choose (Automatic: DIS at Draft,
// TV-L1 otherwise).
[[nodiscard]] OpticalFlowKernel flowKernel(std::int64_t method, std::int64_t quality,
                                           std::int64_t border);

struct FlowBlurOptions final {
    CompiledShutter shutter;
    OpticalFlowKernel flow;
    std::int64_t border = document::kFlowBorderExtend;
    std::optional<OperationIndex> guide;
    std::optional<OperationIndex> matte;
    std::int64_t holeFill = document::kHoleFillOutsideIn;
    double matteShrink = 0.0;
    double alphaBoost = 1.0;
    std::uint32_t seed = 0;
};

// One layer's four solved flows: from the frame to its neighbours and back.
struct SolvedFlows final {
    OperationIndex forward = OperationIndex::fromRaw(0);
    OperationIndex backward = OperationIndex::fromRaw(0);
    OperationIndex forwardBack = OperationIndex::fromRaw(0);
    OperationIndex backwardBack = OperationIndex::fromRaw(0);
};
struct FlowBlurResult final {
    OperationIndex image = OperationIndex::fromRaw(0);
    // The flows the node publishes (with a matte, the foreground's).
    SolvedFlows flows{};
};
// Motion Blur (Optical Flow): the input splatted along its own optical flow, the neighbour frames
// blended in by visibility; with a matte each layer solves its own flow (Wang and Adelson), and the
// foreground is composited over the filled background.
[[nodiscard]] std::optional<FlowBlurResult>
appendFlowMotionBlur(RecipeEmitter& emitter, document::FrameRate rate, OperationIndex input,
                     const FlowBlurOptions& options);

} // namespace bloom::runtime::detail

#include "motion_blur_recipes.hpp"

#include "plan_time_scopes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

std::optional<core::RationalTime> motionFramesToTime(const document::FrameRate rate,
                                                     const double frames) {
    if (!std::isfinite(frames) || std::abs(frames) > 1.0e6)
        return std::nullopt;
    const auto micro = static_cast<std::int64_t>(std::llround(frames * 1.0e6));
    return core::RationalTime::create(micro * static_cast<std::int64_t>(rate.denominator()),
                                      1000000LL * static_cast<std::int64_t>(rate.numerator()));
}

// `input` time-sampled at each of `offsets` (frames) and summed by the accumulator with `weights`.
std::optional<OperationIndex>
appendTimeSamples(RecipeEmitter& emitter, const document::FrameRate rate, const OperationIndex input,
                  const std::vector<double>& offsets, const std::vector<double>& weights,
                  const AccumulateOperator op, const bool holdFootage,
                  const std::int64_t posterizeNumerator, const std::int64_t posterizeDenominator) {
    std::optional<OperationIndex> accumulator;
    std::optional<OperationIndex> first;
    for (std::size_t k = 0; k < offsets.size(); ++k) {
        const auto offset = motionFramesToTime(rate, offsets[k]);
        if (!offset)
            return std::nullopt;
        OperationIndex sample = input;
        if (*offset != core::RationalTime{} || posterizeNumerator > 0 || holdFootage)
            sample = emitter.effect(input, TimeSampleKernel{*offset, posterizeNumerator,
                                                                     posterizeDenominator,
                                                                     holdFootage});
        if (k == 0) {
            first = sample;
            if (offsets.size() == 1)
                accumulator = emitter.effect(sample, TemporalAccumulateKernel{
                                                         op, weights[0], 0.0},
                                             sample);
            continue;
        }
        if (k == 1)
            accumulator = emitter.effect(
                *first, TemporalAccumulateKernel{op, weights[0], weights[1]}, sample);
        else
            accumulator = emitter.effect(*accumulator,
                                         TemporalAccumulateKernel{op, 1.0, weights[k]},
                                         sample);
    }
    return accumulator;
}

// `root` copied with every operation on a path to a substituted operation re-pointed at the
// substitutes (the operations nothing substituted reaches are shared).
OperationIndex
cloneWithSubstitutes(const OperationIndex root,
                     const std::map<std::size_t, OperationIndex>& substitutes,
                     std::vector<CompiledOperation>& operations) {
    const auto limit = root.value() + 1;
    std::vector<bool> reaches(limit, false);
    for (std::size_t index = 0; index < limit; ++index) {
        if (substitutes.contains(index))
            continue;
        forEachOperationInput(operations[index], [&](const auto input) {
            if (input.value() < limit && (substitutes.contains(input.value()) || reaches[input.value()]))
                reaches[index] = true;
        });
    }
    if (substitutes.contains(root.value()))
        return substitutes.at(root.value());
    if (!reaches[root.value()])
        return root;
    std::map<std::size_t, OperationIndex> clones;
    for (std::size_t index = 0; index < limit; ++index) {
        if (!reaches[index])
            continue;
        auto copy = operations[index];
        remapOperationInputs(copy, [&](const OperationIndex input) {
            if (const auto found = substitutes.find(input.value()); found != substitutes.end())
                return found->second;
            if (const auto found = clones.find(input.value()); found != clones.end())
                return found->second;
            return input;
        });
        clones.emplace(index, OperationIndex::fromRaw(operations.size()));
        operations.push_back(std::move(copy));
    }
    return clones.at(root.value());
}

// The vectors of an Image input: forward RG, backward BA, each [x, y, 0, 1].
std::array<OperationIndex, 2>
splitVectorImage(RecipeEmitter& emitter, const OperationIndex image) {
    using S = ImageChannelSource;
    return {emitter.effect(image, ChannelSelectKernel{{S::Red, S::Green, S::Alpha, S::One}}),
            emitter.effect(image, ChannelSelectKernel{{S::Blue, S::Alpha, S::Zero, S::One}})};
}

// A vector stage reading `slots` (VectorBlurKernel slot -> image).
OperationIndex
appendVectorStage(RecipeEmitter& emitter, const OperationIndex input,
                  VectorBlurKernel kernel,
                  const std::vector<std::pair<std::size_t, OperationIndex>>& slots) {
    std::vector<OperationIndex> images;
    for (const auto& [slot, image] : slots) {
        kernel.slotImages[slot] = static_cast<std::int8_t>(images.size());
        images.push_back(image);
    }
    const auto effect = emitter.effect(input, kernel);
    emitter.setImages(effect, std::move(images));
    return effect;
}

OperationIndex
appendFlowSolve(RecipeEmitter& emitter, const OperationIndex from,
                const OperationIndex to, OpticalFlowKernel kernel,
                const std::optional<OperationIndex> mask) {
    std::vector<OperationIndex> images{to};
    kernel.slotImages = {0, -1, -1};
    if (mask && kernel.layer != 0) {
        kernel.slotImages[1] = 1;
        images.push_back(*mask);
    }
    const auto effect = emitter.effect(from, kernel);
    emitter.setImages(effect, std::move(images));
    return effect;
}

// The optical flow kernel a node's method, quality and border choose.
OpticalFlowKernel flowKernel(const std::int64_t method,
                                                           const std::int64_t quality,
                                                           const std::int64_t border) {
    OpticalFlowKernel kernel;
    kernel.method = method == document::kFlowMethodDis ? 0
                    : method == document::kFlowMethodTvl1
                        ? 1
                        : (quality == document::kMotionQualityDraft ? 0 : 1);
    kernel.quality = quality;
    kernel.border = border;
    return kernel;
}

std::optional<FlowBlurResult> appendFlowMotionBlur(RecipeEmitter& emitter,
                                                   const document::FrameRate rate,
                                                   const OperationIndex input,
                                                   const FlowBlurOptions& options) {
    const auto next = motionFramesToTime(rate, 1.0);
    const auto previous = motionFramesToTime(rate, -1.0);
    if (!next || !previous)
        return std::nullopt;
    const auto sample = [&](const OperationIndex image, const core::RationalTime at) {
        return emitter.effect(image, TimeSampleKernel{at, 0, 1, false});
    };
    const auto guide = options.guide.value_or(input);
    const auto guideNext = sample(guide, *next);
    const auto guidePrevious = sample(guide, *previous);
    const auto colourNext = options.guide ? sample(input, *next) : guideNext;
    const auto colourPrevious = options.guide ? sample(input, *previous) : guidePrevious;
    // Wang and Adelson's layers: the matte (shrunk) weights each layer's own flow.
    std::optional<OperationIndex> matte = options.matte;
    if (matte && options.matteShrink > 0.0)
        matte = emitter.effect(
            *matte, SeparableBlurKernel{SeparableBlurKind::Erode,
                                        RecipeEmitter::constant(options.matteShrink),
                                        BlurEdgeMode::Clamp, BlurChannels::Coverage});
    const auto matteNext = matte ? std::optional(sample(*matte, *next)) : std::nullopt;
    const auto mattePrevious = matte ? std::optional(sample(*matte, *previous)) : std::nullopt;
    // Layer 0 every pixel, 1 the matte's foreground, 2 its background.
    const auto solve = [&](const std::int64_t layer) {
        auto kernel = options.flow;
        kernel.border = options.border;
        kernel.layer = layer;
        SolvedFlows flows;
        flows.forward = appendFlowSolve(emitter, guide, guideNext, kernel, matte);
        flows.backward = appendFlowSolve(emitter, guide, guidePrevious, kernel, matte);
        flows.forwardBack = appendFlowSolve(emitter, guideNext, guide, kernel, matteNext);
        flows.backwardBack = appendFlowSolve(emitter, guidePrevious, guide, kernel, mattePrevious);
        return flows;
    };
    VectorBlurKernel check;
    check.stage = VectorBlurStage::Consistency;
    check.border = options.border;
    check.shutter = options.shutter;
    VectorBlurKernel splat;
    splat.stage = VectorBlurStage::Splat;
    splat.decode.layout = document::kVectorLayoutForwardBackward;
    splat.decode.units = 2;
    splat.shutter = options.shutter;
    splat.border = options.border;
    splat.zMode = 2;
    splat.seed = options.seed;
    // Linear per interval; at High and Final the quadratic through the three frames (Xu et al.).
    splat.curved = options.shutter.quality >= document::kMotionQualityHigh;
    const auto splatOf = [&](const OperationIndex colour, const std::int64_t layer,
                             const SolvedFlows& flows) {
        const auto pair = appendVectorStage(emitter, flows.forward, check,
                                            {{kVectorForwardSlot, flows.forwardBack},
                                             {kVectorBackwardSlot, flows.backward},
                                             {kVectorPreviousFlowSlot, flows.backwardBack}});
        const auto nextConsistency = appendVectorStage(
            emitter, flows.forwardBack, check, {{kVectorForwardSlot, flows.forward}});
        const auto previousConsistency = appendVectorStage(
            emitter, flows.backwardBack, check, {{kVectorForwardSlot, flows.backward}});
        auto kernel = splat;
        kernel.layer = layer;
        std::vector<std::pair<std::size_t, OperationIndex>> slots{
            {kVectorForwardSlot, flows.forward},
            {kVectorBackwardSlot, flows.backward},
            {kVectorNextSlot, colourNext},
            {kVectorNextFlowSlot, flows.forwardBack},
            {kVectorPreviousSlot, colourPrevious},
            {kVectorPreviousFlowSlot, flows.backwardBack},
            {kVectorConsistencySlot, pair},
            {kVectorNextConsistencySlot, nextConsistency},
            {kVectorPreviousConsistencySlot, previousConsistency}};
        if (layer != 0)
            slots.emplace_back(kVectorMatteSlot, *matte);
        return appendVectorStage(emitter, colour, kernel, slots);
    };
    if (!matte) {
        const auto flows = solve(0);
        return FlowBlurResult{splatOf(input, 0, flows), flows};
    }
    const auto foregroundFlows = solve(1);
    const auto backgroundFlows = solve(2);
    VectorBlurKernel fill;
    fill.stage = VectorBlurStage::Fill;
    fill.threshold = 0.5;
    fill.shutter = options.shutter;
    OperationIndex background = input;
    if (options.holeFill == document::kHoleFillTemporal) {
        // The background behind the foreground from the neighbour frames first, along the
        // background's own flow; Baker et al.'s outside-in fill takes the pixels they cannot see.
        VectorBlurKernel temporal;
        temporal.stage = VectorBlurStage::TemporalFill;
        temporal.threshold = 0.5;
        temporal.border = options.border;
        temporal.shutter = options.shutter;
        const std::vector<std::pair<std::size_t, OperationIndex>> slots{
            {kVectorMatteSlot, *matte},
            {kVectorForwardSlot, backgroundFlows.forward},
            {kVectorBackwardSlot, backgroundFlows.backward},
            {kVectorNextSlot, colourNext},
            {kVectorPreviousSlot, colourPrevious},
            {kVectorNextMatteSlot, *matteNext},
            {kVectorPreviousMatteSlot, *mattePrevious}};
        background = appendVectorStage(emitter, input, temporal, slots);
        auto mask = temporal;
        mask.stage = VectorBlurStage::TemporalFillMask;
        const auto unfilled = appendVectorStage(emitter, input, mask, slots);
        background = appendVectorStage(emitter, background, fill, {{kVectorMatteSlot, unfilled}});
    } else {
        background = appendVectorStage(emitter, input, fill, {{kVectorMatteSlot, *matte}});
    }
    const auto blurredBackground = splatOf(background, 0, backgroundFlows);
    auto foreground = splatOf(input, 1, foregroundFlows);
    if (options.alphaBoost != 1.0)
        foreground = emitter.effect(
            foreground, TemporalAccumulateKernel{AccumulateOperator::Add, options.alphaBoost, 0.0},
            foreground);
    return FlowBlurResult{emitter.merge({{foreground, core::BlendMode::Normal},
                                         {blurredBackground, core::BlendMode::Normal}}),
                          foregroundFlows};
}

} // namespace bloom::runtime::detail

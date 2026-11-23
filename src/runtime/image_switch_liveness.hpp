#pragma once

// Private to src/runtime. #531 Image Switch: which input a switch shows this frame, and the
// operations a frame reads when only each switch's chosen input is evaluated. The CPU reference
// (cpu_composition_evaluator.cpp) and GPU scene preparation (cpu_gpu_scene_preparation.cpp) both
// narrow their work with liveOperations(), so an unselected branch is neither evaluated nor
// prepared, decoded or dispatched, on either route.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

// The input the switch at operation `at` shows this frame: its index sampled on the switch's own
// clock. nullopt when that clock is absent or the index cannot be resolved (the switch then shows
// nothing, as an operation on an absent clock does).
[[nodiscard]] inline std::optional<OperationIndex>
selectedSwitchInput(const CompiledImageSwitch& switcher, const CompiledCompositionPlan& plan,
                    const ResolvedEvaluation& frameResolved, const core::RationalTime requestTime,
                    const OperationIndex at) {
    const auto clock = plan.operationClock(at);
    if (!timeForClock(frameResolved, requestTime, clock))
        return std::nullopt;
    const auto index =
        resolveParameter(switcher.index, plan, resolvedForClock(frameResolved, clock));
    if (!index)
        return std::nullopt;
    const auto position = imageSwitchPosition(switcher.inputs.size(), index->value, switcher.wrap);
    if (!position || switcher.inputs[*position].value() >= plan.operations().size())
        return std::nullopt;
    return switcher.inputs[*position];
}

// The operation whose image operation `index` is this frame: `index` itself, or for an Image Switch
// the operation its chosen input shows (through switches of switches). A layer whose vector leaf is
// this operation draws the leaf's own image on the integer grid exactly as when wired to the leaf.
[[nodiscard]] inline std::size_t shownOperation(const CompiledCompositionPlan& plan,
                                                const ResolvedEvaluation& frameResolved,
                                                const core::RationalTime requestTime,
                                                std::size_t index) {
    const auto& operations = plan.operations();
    while (index < operations.size()) {
        const auto* switcher = std::get_if<CompiledImageSwitch>(&operations[index]);
        if (switcher == nullptr)
            break;
        const auto chosen = selectedSwitchInput(*switcher, plan, frameResolved, requestTime,
                                                OperationIndex::fromRaw(index));
        if (!chosen || chosen->value() >= index)
            break;
        index = chosen->value();
    }
    return index;
}

// Whether `plan` holds an Image Switch at all (a plan without one reads every reachable operation).
[[nodiscard]] inline bool planHasImageSwitch(const CompiledCompositionPlan& plan) noexcept {
    for (const auto& operation : plan.operations())
        if (std::holds_alternative<CompiledImageSwitch>(operation))
            return true;
    return false;
}

// Whether a frame's reads depend on more than the plan: an Image Switch narrows them to its chosen
// input, and a video source's layer outside its range at the frame's time reads no movie (#772).
[[nodiscard]] inline bool planNeedsLiveness(const CompiledCompositionPlan& plan) noexcept {
    for (const auto& operation : plan.operations())
        if (std::holds_alternative<CompiledImageSwitch>(operation) ||
            std::holds_alternative<CompiledVideoSource>(operation))
            return true;
    return false;
}

// The operations this frame reads, walked from the output (and from every bounds readout's image,
// which the value graph reads) through every input, but through only the chosen input of each
// Image Switch. #772: a Layer Output outside [inPoint, outPoint) at its placement time shows
// nothing, so its content is not read (no movie is opened for it) unless it is the transform
// parent of a layer that is read, whose transform may depend on its content. Everything else is
// reachable in the plan and unread this frame.
[[nodiscard]] inline std::vector<bool> liveOperations(const CompiledCompositionPlan& plan,
                                                      const ResolvedEvaluation& frameResolved,
                                                      const core::RationalTime requestTime,
                                                      const std::size_t outputIndex) {
    const auto& operations = plan.operations();
    std::vector<bool> live(operations.size(), false), expanded(operations.size(), false);
    struct Visit final {
        std::size_t index;
        bool parentOfLiveLayer;
    };
    std::vector<Visit> pending{{outputIndex, false}};
    for (const auto& value : plan.valueOperations())
        if (const auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel))
            pending.push_back({bounds->operationIndex.value(), false});
    while (!pending.empty()) {
        const auto [index, parentOfLiveLayer] = pending.back();
        pending.pop_back();
        if (index >= operations.size() || expanded[index])
            continue;
        const bool seen = live[index];
        live[index] = true;
        if (const auto* switcher = std::get_if<CompiledImageSwitch>(&operations[index])) {
            expanded[index] = true;
            if (const auto chosen = selectedSwitchInput(*switcher, plan, frameResolved, requestTime,
                                                        OperationIndex::fromRaw(index)))
                pending.push_back({chosen->value(), false});
            continue;
        }
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operations[index])) {
            if (seen && !parentOfLiveLayer)
                continue;
            const auto placement = timeForClock(frameResolved, requestTime,
                                                plan.placementClock(OperationIndex::fromRaw(index)));
            const bool shown = placement && !(*placement < layer->inPoint) &&
                               !(layer->outPoint && !(*placement < *layer->outPoint));
            if (shown || parentOfLiveLayer) {
                expanded[index] = true;
                pending.push_back({layer->input.value(), false});
            }
            if (layer->parent)
                pending.push_back({layer->parent->value(), true});
            continue;
        }
        expanded[index] = true;
        forEachInput(operations[index], [&pending](const OperationIndex input) {
            pending.push_back({input.value(), false});
        });
    }
    return live;
}

} // namespace bloom::runtime::detail

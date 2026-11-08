#include "plan_time_scopes.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {
namespace {

template <class... Ts> struct Visit : Ts... {
    using Ts::operator()...;
};
template <class... Ts> Visit(Ts...) -> Visit<Ts...>;

[[nodiscard]] const CompiledLayerTime* retimed(const CompiledOperation& operation) {
    const auto* layer = std::get_if<CompiledLayerOutput>(&operation);
    return layer != nullptr && layer->time ? &*layer->time : nullptr;
}

// #484: a time sample evaluates its input on its own (Offset) clock.
[[nodiscard]] const TimeSampleKernel* timeSample(const CompiledOperation& operation) {
    const auto* effect = std::get_if<CompiledImageEffect>(&operation);
    return effect != nullptr ? std::get_if<TimeSampleKernel>(&effect->kernel) : nullptr;
}

// #484: a leaf that reads footage frames (Hold keeps them at the frame's own time).
[[nodiscard]] bool footageLeaf(const CompiledOperation& operation) {
    return std::holds_alternative<CompiledVideoSource>(operation) ||
           std::holds_alternative<CompiledImageSource>(operation) ||
           std::holds_alternative<CompiledChannelShuffle>(operation) ||
           std::holds_alternative<CompiledCryptomatte>(operation);
}

// Calls `function(input, clockOfInput)` for every input of `operation` placed on `clock`, where a
// retimed Layer Output feeds its image input from its content clock.
template <typename Function>
void forEachScopedInput(const CompiledOperation& operation, const std::uint32_t clock,
                        const std::uint32_t contentClock, Function&& function) {
    std::visit(
        Visit{[](const CompiledShape&) {}, [](const CompiledSolid&) {},
              [](const CompiledChecker&) {}, [](const CompiledText&) {},
              [](const CompiledImageSource&) {}, [](const CompiledVideoSource&) {},
              [](const CompiledCompositionSource&) {}, [](const CompiledChannelShuffle&) {},
              [](const CompiledCryptomatte&) {},
              [&](const CompiledImageEffect& effect) {
                  // #484: a time sample's input is on its Offset clock (`contentClock`).
                  function(effect.input,
                           std::holds_alternative<TimeSampleKernel>(effect.kernel) ? contentClock
                                                                                  : clock);
                  if (effect.secondaryInput)
                      function(*effect.secondaryInput, clock);
                  if (effect.maskInput)
                      function(*effect.maskInput, clock);
                  for (const auto image : effect.fieldImages)
                      function(image, clock);
              },
              [&](const CompiledLayerOutput& layer) {
                  function(layer.input, layer.time ? contentClock : clock);
                  if (layer.parent)
                      function(*layer.parent, clock);
              },
              [&](const CompiledMerge& merge) {
                  for (const auto& entry : merge.entries)
                      function(entry.input, clock);
              },
              [&](const CompiledCompositionOutput& output) {
                  function(output.input, clock);
                  // #480: the images of Image to Channel Set layers written beside RGBA.
                  if (output.channelLayers)
                      for (const auto& source : output.channelLayers->sources)
                          if (source.image)
                              function(*source.image, clock);
              }},
        operation);
}

// #480: a leaf's Image to Channel Set sources name their channels statically and never read their
// image, so a scoped copy keeps the image only when it was emitted on the same clock.
template <typename Find> void remapLeafImages(CompiledChannelExpression& expression, Find&& find) {
    for (auto& source : expression.sources)
        if (source.image)
            source.image = find(*source.image);
}

template <typename Function>
void remapInputs(CompiledOperation& operation, Function&& function) {
    std::visit(Visit{[](CompiledShape&) {}, [](CompiledSolid&) {}, [](CompiledChecker&) {},
                     [](CompiledText&) {}, [](CompiledImageSource&) {}, [](CompiledVideoSource&) {},
                     [](CompiledCompositionSource&) {}, [](CompiledChannelShuffle&) {},
                     [](CompiledCryptomatte&) {},
                     [&](CompiledImageEffect& effect) {
                         effect.input = function(
                             effect.input, std::holds_alternative<TimeSampleKernel>(effect.kernel));
                         if (effect.secondaryInput)
                             effect.secondaryInput = function(*effect.secondaryInput, false);
                         if (effect.maskInput)
                             effect.maskInput = function(*effect.maskInput, false);
                         for (auto& image : effect.fieldImages)
                             image = function(image, false);
                     },
                     [&](CompiledLayerOutput& layer) {
                         layer.input = function(layer.input, true);
                         if (layer.parent)
                             layer.parent = function(*layer.parent, false);
                     },
                     [&](CompiledMerge& merge) {
                         for (auto& entry : merge.entries)
                             entry.input = function(entry.input, false);
                     },
                     [&](CompiledCompositionOutput& output) {
                         output.input = function(output.input, false);
                         if (output.channelLayers)
                             for (auto& source : output.channelLayers->sources)
                                 if (source.image)
                                     source.image = function(*source.image, false);
                     }},
               operation);
}

} // namespace

namespace {

// Drops the operations nothing reaches from the output or a bounds readout, remapping every
// reference (inputs, clocks, readouts and channel expression images).
void pruneUnreachable(CompiledCompositionPlanDefinition& definition) {
    auto& operations = definition.operations;
    std::vector<bool> reached(operations.size(), false);
    std::vector<std::size_t> work{definition.output.value()};
    for (const auto& value : definition.valueOperations)
        if (const auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel))
            work.push_back(bounds->operationIndex.value());
    // The clocks a kept operation evaluates or is placed on, with their parents. A used clock keeps
    // the Layer Output that defines it (its time mapping), so reaching is repeated until no used
    // clock names an unreached layer (#480: a channel layer plan in a retimed composition).
    std::vector<bool> usedClocks(definition.clocks.size(), false);
    const auto clockOf = [](const std::vector<std::uint32_t>& clocks, const std::size_t index) {
        return index < clocks.size() ? clocks[index] : 0U;
    };
    while (!work.empty()) {
        while (!work.empty()) {
            const auto index = work.back();
            work.pop_back();
            if (index >= operations.size() || reached[index])
                continue;
            reached[index] = true;
            forEachScopedInput(operations[index], 0, 0,
                               [&](const OperationIndex input, std::uint32_t) {
                                   work.push_back(input.value());
                               });
        }
        for (std::size_t index = 0; index < operations.size(); ++index) {
            if (!reached[index])
                continue;
            for (auto clock : {clockOf(definition.operationClocks, index),
                               clockOf(definition.placementClocks, index)})
                while (clock != 0 && clock < usedClocks.size() && !usedClocks[clock]) {
                    usedClocks[clock] = true;
                    work.push_back(definition.clocks[clock].layer.value());
                    clock = definition.clocks[clock].parent;
                }
        }
    }
    if (std::ranges::all_of(reached, [](const bool value) { return value; }))
        return;
    std::vector<std::optional<std::size_t>> mapped(operations.size());
    std::size_t next = 0;
    for (std::size_t index = 0; index < operations.size(); ++index)
        if (reached[index])
            mapped[index] = next++;
    const auto map = [&](const OperationIndex input) {
        return OperationIndex::fromRaw(*mapped[input.value()]);
    };
    const auto mapImages = [&](CompiledChannelExpression& expression) {
        for (auto& source : expression.sources)
            if (source.image)
                source.image = source.image->value() < mapped.size() && mapped[source.image->value()]
                                   ? std::optional{map(*source.image)}
                                   : std::nullopt;
    };
    std::vector<CompiledOperation> kept;
    std::vector<std::uint32_t> operationClocks;
    std::vector<std::uint32_t> placementClocks;
    kept.reserve(next);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        if (!reached[index])
            continue;
        auto copy = std::move(operations[index]);
        // A channel layer image may be dropped, so it is remapped apart from the output's input.
        std::optional<CompiledChannelExpression> layers;
        if (auto* output = std::get_if<CompiledCompositionOutput>(&copy);
            output != nullptr && output->channelLayers) {
            layers = std::move(output->channelLayers);
            output->channelLayers.reset();
        }
        remapInputs(copy, [&](const OperationIndex input, bool) { return map(input); });
        if (layers) {
            mapImages(*layers);
            std::get<CompiledCompositionOutput>(copy).channelLayers = std::move(layers);
        }
        if (auto* shuffle = std::get_if<CompiledChannelShuffle>(&copy))
            mapImages(shuffle->channels);
        else if (auto* cryptomatte = std::get_if<CompiledCryptomatte>(&copy))
            mapImages(cryptomatte->channels);
        kept.push_back(std::move(copy));
        if (index < definition.operationClocks.size())
            operationClocks.push_back(definition.operationClocks[index]);
        if (index < definition.placementClocks.size())
            placementClocks.push_back(definition.placementClocks[index]);
    }
    definition.operations = std::move(kept);
    definition.output = map(definition.output);
    if (!definition.operationClocks.empty())
        definition.operationClocks = std::move(operationClocks);
    if (!definition.placementClocks.empty())
        definition.placementClocks = std::move(placementClocks);
    // Clocks no kept operation uses are dropped (their Layer Output may be gone); the rest keep
    // their order, so a parent still precedes its children.
    if (!definition.clocks.empty()) {
        std::vector<std::uint32_t> clockMap(definition.clocks.size(), 0);
        std::vector<CompiledTimeClock> clocks;
        for (std::size_t clock = 0; clock < definition.clocks.size(); ++clock) {
            if (clock != 0 && !usedClocks[clock])
                continue;
            clockMap[clock] = static_cast<std::uint32_t>(clocks.size());
            auto copy = definition.clocks[clock];
            if (clock != 0) {
                copy.parent = clockMap[copy.parent];
                copy.layer = map(copy.layer);
            }
            clocks.push_back(copy);
        }
        for (auto& clock : definition.operationClocks)
            clock = clock < clockMap.size() ? clockMap[clock] : 0U;
        for (auto& clock : definition.placementClocks)
            clock = clock < clockMap.size() ? clockMap[clock] : 0U;
        definition.clocks = std::move(clocks);
    }
    for (auto& value : definition.valueOperations)
        if (auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel);
            bounds != nullptr && bounds->operationIndex.value() < mapped.size() &&
            mapped[bounds->operationIndex.value()])
            bounds->operationIndex = map(bounds->operationIndex);
}

} // namespace

void forEachOperationInput(const CompiledOperation& operation,
                           const std::function<void(OperationIndex)>& function) {
    forEachScopedInput(operation, 0, 0,
                       [&](const OperationIndex input, std::uint32_t) { function(input); });
}

void remapOperationInputs(CompiledOperation& operation,
                          const std::function<OperationIndex(OperationIndex)>& function) {
    remapInputs(operation, [&](const OperationIndex input, bool) { return function(input); });
}

void separateChannelLayerImages(CompiledCompositionPlanDefinition& definition) {
    if (definition.output.value() >= definition.operations.size())
        return;
    if (auto* output =
            std::get_if<CompiledCompositionOutput>(&definition.operations[definition.output.value()]);
        output != nullptr && output->channelLayers) {
        const auto node = output->sourceNodeId;
        std::vector<std::shared_ptr<const CompiledCompositionPlan>> plans(
            output->channelLayers->sources.size());
        for (std::size_t index = 0; index < plans.size(); ++index) {
            const auto& source = output->channelLayers->sources[index];
            if (!source.imageBacked || !source.image)
                continue;
            auto layer = definition;
            const auto merge = OperationIndex::fromRaw(layer.operations.size());
            layer.operations.emplace_back(CompiledMerge{node, {{{}, {}, *source.image}}});
            layer.output = OperationIndex::fromRaw(layer.operations.size());
            layer.operations.emplace_back(CompiledCompositionOutput{node, merge});
            // Both new operations sit on the plan clock, where the output reads its layers.
            if (!layer.operationClocks.empty())
                layer.operationClocks.insert(layer.operationClocks.end(), {0U, 0U});
            if (!layer.placementClocks.empty())
                layer.placementClocks.insert(layer.placementClocks.end(), {0U, 0U});
            pruneUnreachable(layer);
            plans[index] = std::make_shared<const CompiledCompositionPlan>(std::move(layer));
        }
        // `output` still addresses `definition`: the copies above never touch it.
        for (std::size_t index = 0; index < plans.size(); ++index)
            if (plans[index]) {
                output->channelLayers->sources[index].imagePlan = std::move(plans[index]);
                output->channelLayers->sources[index].image.reset();
            }
    }
    pruneUnreachable(definition);
}

std::optional<TimeScopeFailure> scopeLayerTime(CompiledCompositionPlanDefinition& definition) {
    const auto& operations = definition.operations;
    const bool sampled =
        std::ranges::any_of(operations, [](const auto& item) { return timeSample(item) != nullptr; });
    if (!sampled &&
        std::ranges::none_of(operations, [](const auto& item) { return retimed(item) != nullptr; }))
        return std::nullopt;

    // Clock 0 is the plan clock. Layer and Content clocks are keyed by (parent clock, layer op),
    // #484 Offset clocks by (parent clock, time sample op).
    std::vector<CompiledTimeClock> clocks{CompiledTimeClock{}};
    // Whether a clock is a time sample's that holds footage at its parent's time.
    std::vector<bool> holds{false};
    std::map<std::tuple<std::uint32_t, std::size_t, CompiledClockKind>, std::uint32_t> clockIds;
    const auto clockFor = [&](const std::uint32_t parent, const std::size_t layer,
                              const CompiledClockKind kind) -> std::optional<std::uint32_t> {
        const auto key = std::tuple{parent, layer, kind};
        if (const auto found = clockIds.find(key); found != clockIds.end())
            return found->second;
        if (clocks.size() >= kMaximumTimeClocks)
            return std::nullopt;
        const auto id = static_cast<std::uint32_t>(clocks.size());
        // `layer` is rewritten to the emitted copy below.
        clocks.push_back({kind, parent, OperationIndex::fromRaw(layer)});
        const auto* sample = kind == CompiledClockKind::Offset ? timeSample(operations[layer])
                                                               : nullptr;
        holds.push_back(sample != nullptr && sample->holdFootage);
        clockIds.emplace(key, id);
        return id;
    };
    // #484: an operation reached through a time sample is copied onto its Offset clock only when
    // its pixels can differ there. One that does not vary with time is read on the nearest clock
    // above the time samples (once, however many sub-frames read it), and under a sample that
    // holds footage, so is one whose only time dependence is footage (an image sequence or video
    // and everything derived from it alone): Hold keeps every sub-frame on the frame's own
    // footage frame. Both are read from the plan's own time-dependence analysis, the second with
    // every footage leaf replaced by a constant.
    std::vector<bool> invariant(operations.size(), false);
    std::vector<bool> holdable(operations.size(), false);
    if (sampled) {
        const auto dependence = [&](const bool replaceFootage) {
            auto copy = definition;
            copy.clocks.clear();
            copy.operationClocks.clear();
            copy.placementClocks.clear();
            if (replaceFootage)
                for (auto& operation : copy.operations)
                    if (footageLeaf(operation))
                        operation = CompiledSolid{};
            // A time sample is the identity on its input's pixels: here it varies with time
            // exactly when its input does, whatever the plan's cache analysis says of its key.
            for (auto& operation : copy.operations)
                if (auto* effect = std::get_if<CompiledImageEffect>(&operation);
                    effect != nullptr && std::holds_alternative<TimeSampleKernel>(effect->kernel))
                    effect->kernel = IdentityImageKernel{};
            const CompiledCompositionPlan plan(std::move(copy));
            std::vector<bool> dependent(operations.size(), true);
            for (std::size_t index = 0; index < operations.size(); ++index)
                dependent[index] = plan.operationTimeDependent(OperationIndex::fromRaw(index));
            return dependent;
        };
        const auto real = dependence(false);
        const auto withoutFootage = dependence(true);
        for (std::size_t index = 0; index < operations.size(); ++index) {
            invariant[index] = !real[index];
            holdable[index] = !withoutFootage[index];
        }
    }
    const auto inputClock = [&](const std::size_t input, std::uint32_t clock) {
        if (input >= operations.size())
            return clock;
        while (clock != 0 && clock < clocks.size() &&
               clocks[clock].kind == CompiledClockKind::Offset &&
               (invariant[input] || (holds[clock] && holdable[input])))
            clock = clocks[clock].parent;
        return clock;
    };

    // Which clocks each original operation is needed on, discovered from the output backwards.
    // Bounds readouts address image operations directly and need them on the plan clock.
    std::vector<std::set<std::uint32_t>> needed(operations.size());
    std::vector<std::pair<std::size_t, std::uint32_t>> roots{{definition.output.value(), 0}};
    for (const auto& value : definition.valueOperations)
        if (const auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel);
            bounds && bounds->operationIndex.value() < operations.size())
            roots.emplace_back(bounds->operationIndex.value(), 0);
    auto work = roots;
    // For each (layer op or time sample, placement clock): its (operand clock, content clock).
    std::map<std::pair<std::size_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>>
        layerClocks;
    while (!work.empty()) {
        const auto [index, clock] = work.back();
        work.pop_back();
        if (index >= operations.size() || !needed[index].insert(clock).second)
            continue;
        std::uint32_t content = clock;
        if (retimed(operations[index]) != nullptr) {
            const auto layerClock = clockFor(clock, index, CompiledClockKind::Layer);
            const auto contentClock =
                layerClock ? clockFor(*layerClock, index, CompiledClockKind::Content)
                           : std::nullopt;
            if (!contentClock)
                return TimeScopeFailure{"Layer time scopes exceed the supported clock count"};
            layerClocks[{index, clock}] = {*layerClock, *contentClock};
            content = *contentClock;
        } else if (timeSample(operations[index]) != nullptr) {
            const auto offsetClock = clockFor(clock, index, CompiledClockKind::Offset);
            if (!offsetClock)
                return TimeScopeFailure{"Motion blur time samples exceed the supported clock count"};
            layerClocks[{index, clock}] = {clock, *offsetClock};
            content = *offsetClock;
        }
        forEachScopedInput(operations[index], clock, content,
                           [&](const OperationIndex input, const std::uint32_t at) {
                               work.emplace_back(input.value(), inputClock(input.value(), at));
                           });
    }

    std::size_t total = 0;
    for (const auto& clocksOfOperation : needed)
        total += clocksOfOperation.size();
    if (total > kMaximumScopedOperations)
        return TimeScopeFailure{"Layer time scopes exceed the supported operation count"};

    // The emission order: the original topological order, one copy per clock; #484 with time
    // samples, a depth-first post-order from the output instead, so each time sample's copies are
    // evaluated (and released) before the next sample's begin.
    std::vector<std::pair<std::size_t, std::uint32_t>> order;
    order.reserve(total);
    if (!sampled) {
        for (std::size_t index = 0; index < operations.size(); ++index)
            for (const auto clock : needed[index])
                order.emplace_back(index, clock);
    } else {
        std::set<std::pair<std::size_t, std::uint32_t>> visited;
        struct Frame final {
            std::pair<std::size_t, std::uint32_t> node;
            std::vector<std::pair<std::size_t, std::uint32_t>> inputs;
            std::size_t next = 0;
        };
        const auto inputsOf = [&](const std::pair<std::size_t, std::uint32_t> node) {
            std::vector<std::pair<std::size_t, std::uint32_t>> inputs;
            std::uint32_t content = node.second;
            if (const auto found = layerClocks.find(node); found != layerClocks.end())
                content = found->second.second;
            forEachScopedInput(operations[node.first], node.second, content,
                               [&](const OperationIndex input, const std::uint32_t at) {
                                   inputs.emplace_back(input.value(),
                                                       inputClock(input.value(), at));
                               });
            return inputs;
        };
        // Every needed copy is reached from a root (`needed` was discovered from them).
        for (const auto& root : roots) {
            if (root.first >= operations.size() || !visited.insert(root).second)
                continue;
            std::vector<Frame> stack;
            stack.push_back({root, inputsOf(root), 0});
            while (!stack.empty()) {
                auto& top = stack.back();
                if (top.next < top.inputs.size()) {
                    const auto input = top.inputs[top.next++];
                    if (input.first < operations.size() && visited.insert(input).second)
                        stack.push_back({input, inputsOf(input), 0});
                    continue;
                }
                order.push_back(top.node);
                stack.pop_back();
            }
        }
    }

    // Emit in that order, remapping inputs.
    std::map<std::pair<std::size_t, std::uint32_t>, std::size_t> emitted;
    std::vector<CompiledOperation> scoped;
    std::vector<std::uint32_t> operationClocks;
    std::vector<std::uint32_t> placementClocks;
    std::vector<std::pair<std::size_t, std::uint32_t>> leaves;
    scoped.reserve(total);
    operationClocks.reserve(total);
    placementClocks.reserve(total);
    for (const auto& [index, clock] : order) {
        auto copy = operations[index];
        std::uint32_t operandClock = clock;
        std::uint32_t contentClock = clock;
        if (const auto found = layerClocks.find({index, clock}); found != layerClocks.end()) {
            operandClock = found->second.first;
            contentClock = found->second.second;
        }
        const bool scopes = retimed(operations[index]) != nullptr ||
                            timeSample(operations[index]) != nullptr;
        remapInputs(copy, [&](const OperationIndex input, const bool contentInput) {
            const auto at = contentInput && scopes ? contentClock : clock;
            return OperationIndex::fromRaw(
                emitted.at({input.value(), inputClock(input.value(), at)}));
        });
        const auto findOnClock = [&](const OperationIndex input) -> std::optional<OperationIndex> {
            const auto found = emitted.find({input.value(), clock});
            return found == emitted.end() ? std::nullopt
                                          : std::optional{OperationIndex::fromRaw(found->second)};
        };
        // A leaf's channel images are resolved against the copies emitted so far; in depth-first
        // order, after every copy is emitted.
        if (!sampled) {
            if (auto* shuffle = std::get_if<CompiledChannelShuffle>(&copy))
                remapLeafImages(shuffle->channels, findOnClock);
            else if (auto* cryptomatte = std::get_if<CompiledCryptomatte>(&copy))
                remapLeafImages(cryptomatte->channels, findOnClock);
        } else if (std::holds_alternative<CompiledChannelShuffle>(copy) ||
                   std::holds_alternative<CompiledCryptomatte>(copy)) {
            leaves.emplace_back(scoped.size(), clock);
        }
        emitted[{index, clock}] = scoped.size();
        scoped.push_back(std::move(copy));
        operationClocks.push_back(operandClock);
        placementClocks.push_back(clock);
    }
    for (const auto& [at, clock] : leaves) {
        const auto findOnClock = [&](const OperationIndex input) -> std::optional<OperationIndex> {
            const auto found = emitted.find({input.value(), clock});
            return found == emitted.end() || found->second >= at
                       ? std::nullopt
                       : std::optional{OperationIndex::fromRaw(found->second)};
        };
        if (auto* shuffle = std::get_if<CompiledChannelShuffle>(&scoped[at]))
            remapLeafImages(shuffle->channels, findOnClock);
        else if (auto* cryptomatte = std::get_if<CompiledCryptomatte>(&scoped[at]))
            remapLeafImages(cryptomatte->channels, findOnClock);
    }
    // Clocks name the emitted placement copy of the Layer Output (or time sample) that defines
    // them.
    for (auto& entry : clocks) {
        if (entry.kind == CompiledClockKind::Plan)
            continue;
        const auto placement = entry.kind == CompiledClockKind::Content
                                   ? clocks[entry.parent].parent
                                   : entry.parent;
        entry.layer = OperationIndex::fromRaw(emitted.at({entry.layer.value(), placement}));
    }
    for (auto& value : definition.valueOperations)
        if (auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel))
            if (const auto found = emitted.find({bounds->operationIndex.value(), 0});
                found != emitted.end())
                bounds->operationIndex = OperationIndex::fromRaw(found->second);
    definition.output = OperationIndex::fromRaw(emitted.at({definition.output.value(), 0}));
    definition.operations = std::move(scoped);
    definition.clocks = std::move(clocks);
    definition.operationClocks = std::move(operationClocks);
    definition.placementClocks = std::move(placementClocks);
    return std::nullopt;
}

} // namespace bloom::runtime::detail

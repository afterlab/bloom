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
                  function(effect.input, clock);
                  if (effect.secondaryInput)
                      function(*effect.secondaryInput, clock);
                  if (effect.maskInput)
                      function(*effect.maskInput, clock);
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
                         effect.input = function(effect.input, false);
                         if (effect.secondaryInput)
                             effect.secondaryInput = function(*effect.secondaryInput, false);
                         if (effect.maskInput)
                             effect.maskInput = function(*effect.maskInput, false);
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
    for (auto& clock : definition.clocks)
        if (clock.layer.value() < mapped.size() && mapped[clock.layer.value()])
            clock.layer = map(clock.layer);
    for (auto& value : definition.valueOperations)
        if (auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel);
            bounds != nullptr && bounds->operationIndex.value() < mapped.size() &&
            mapped[bounds->operationIndex.value()])
            bounds->operationIndex = map(bounds->operationIndex);
}

} // namespace

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
    if (std::ranges::none_of(operations, [](const auto& item) { return retimed(item) != nullptr; }))
        return std::nullopt;

    // Clock 0 is the plan clock. Layer and Content clocks are keyed by (parent clock, layer op).
    std::vector<CompiledTimeClock> clocks{CompiledTimeClock{}};
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
        clockIds.emplace(key, id);
        return id;
    };

    // Which clocks each original operation is needed on, discovered from the output backwards.
    // Bounds readouts address image operations directly and need them on the plan clock.
    std::vector<std::set<std::uint32_t>> needed(operations.size());
    std::vector<std::pair<std::size_t, std::uint32_t>> work{{definition.output.value(), 0}};
    for (const auto& value : definition.valueOperations)
        if (const auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel);
            bounds && bounds->operationIndex.value() < operations.size())
            work.emplace_back(bounds->operationIndex.value(), 0);
    // For each (layer op, placement clock): its (layer clock, content clock).
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
        }
        forEachScopedInput(operations[index], clock, content,
                           [&](const OperationIndex input, const std::uint32_t inputClock) {
                               work.emplace_back(input.value(), inputClock);
                           });
    }

    std::size_t total = 0;
    for (const auto& clocksOfOperation : needed)
        total += clocksOfOperation.size();
    if (total > kMaximumScopedOperations)
        return TimeScopeFailure{"Layer time scopes exceed the supported operation count"};

    // Emit in the original topological order, one copy per clock, remapping inputs.
    std::map<std::pair<std::size_t, std::uint32_t>, std::size_t> emitted;
    std::vector<CompiledOperation> scoped;
    std::vector<std::uint32_t> operationClocks;
    std::vector<std::uint32_t> placementClocks;
    scoped.reserve(total);
    operationClocks.reserve(total);
    placementClocks.reserve(total);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        for (const auto clock : needed[index]) {
            auto copy = operations[index];
            std::uint32_t operandClock = clock;
            std::uint32_t contentClock = clock;
            if (const auto found = layerClocks.find({index, clock}); found != layerClocks.end()) {
                operandClock = found->second.first;
                contentClock = found->second.second;
            }
            remapInputs(copy, [&](const OperationIndex input, const bool imageInputOfLayer) {
                const auto inputClock =
                    imageInputOfLayer && retimed(operations[index]) ? contentClock : clock;
                return OperationIndex::fromRaw(emitted.at({input.value(), inputClock}));
            });
            const auto findOnClock = [&](const OperationIndex input) -> std::optional<OperationIndex> {
                const auto found = emitted.find({input.value(), clock});
                return found == emitted.end() ? std::nullopt
                                              : std::optional{OperationIndex::fromRaw(found->second)};
            };
            if (auto* shuffle = std::get_if<CompiledChannelShuffle>(&copy))
                remapLeafImages(shuffle->channels, findOnClock);
            else if (auto* cryptomatte = std::get_if<CompiledCryptomatte>(&copy))
                remapLeafImages(cryptomatte->channels, findOnClock);
            emitted[{index, clock}] = scoped.size();
            scoped.push_back(std::move(copy));
            operationClocks.push_back(operandClock);
            placementClocks.push_back(clock);
        }
    }
    // Clocks name the emitted placement copy of the Layer Output that defines them.
    for (auto& entry : clocks) {
        if (entry.kind == CompiledClockKind::Plan)
            continue;
        const auto placement = entry.kind == CompiledClockKind::Layer
                                   ? entry.parent
                                   : clocks[entry.parent].parent;
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

#include <bloom/document/node_group_nodes.hpp>

#include <bloom/document/evaluation_inputs.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/warp_path_nodes.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace bloom::document {
namespace {

[[nodiscard]] std::string_view operandSchemaKeyOf(const SocketValueKind kind) noexcept {
    switch (kind) {
    case SocketValueKind::Scalar:
        return kScalarOperandParameterSchemaKey;
    case SocketValueKind::Vector2:
        return kVector2OperandParameterSchemaKey;
    case SocketValueKind::Color:
        return kColorOperandParameterSchemaKey;
    case SocketValueKind::Integer:
        return kIntegerOperandParameterSchemaKey;
    case SocketValueKind::Boolean:
        return kBooleanOperandParameterSchemaKey;
    default:
        return {};
    }
}

[[nodiscard]] ParameterValueKind parameterKindOf(const SocketValueKind kind) noexcept {
    switch (kind) {
    case SocketValueKind::Vector2:
        return ParameterValueKind::Vec2d;
    case SocketValueKind::Color:
        return ParameterValueKind::Color4d;
    case SocketValueKind::Integer:
        return ParameterValueKind::Integer;
    case SocketValueKind::Boolean:
        return ParameterValueKind::Boolean;
    default:
        return ParameterValueKind::Float64;
    }
}

[[nodiscard]] ParameterValue defaultValueOf(const SocketValueKind kind) {
    switch (kind) {
    case SocketValueKind::Vector2:
        return Vec2d{};
    case SocketValueKind::Color:
        return kDefaultValueColor;
    case SocketValueKind::Integer:
        return std::int64_t{0};
    case SocketValueKind::Boolean:
        return false;
    default:
        return 0.0;
    }
}

[[nodiscard]] ParameterDefinition slotParameter(const NodeGroupSlot& slot) {
    const auto key = operandSchemaKeyOf(slot.socket);
    return {std::string(slot.name),
            std::string(key),
            parameterKindOf(slot.socket),
            true,
            isAnimatableSchemaKey(key),
            defaultValueOf(slot.socket)};
}

[[nodiscard]] NodeDefinition functionDefinition() {
    NodeDefinition definition{{std::string(kFunctionNodeType), kNodeGroupNodeSchemaVersion},
                              NodeLoweringKind::ValueFunction,
                              {{std::string(kFunctionArgumentRole), SocketValueKind::Scalar, false}},
                              {{std::string(kFunctionResultPort), SocketValueKind::Scalar}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Math};
    definition.parameters.push_back({std::string(kNodeGroupReferenceRole),
                                     std::string(kNodeGroupReferenceSchemaKey),
                                     ParameterValueKind::Integer, true, false, std::int64_t{0}});
    definition.parameters.push_back({std::string(kFunctionArgumentRole),
                                     std::string(kScalarOperandParameterSchemaKey),
                                     ParameterValueKind::Float64, true,
                                     isAnimatableSchemaKey(kScalarOperandParameterSchemaKey), 0.0});
    return definition;
}

[[nodiscard]] NodeDefinition functionPlotDefinition() {
    NodeDefinition definition{{std::string(kFunctionPlotNodeType), kNodeGroupNodeSchemaVersion},
                              NodeLoweringKind::ValueFunction,
                              {},
                              {{std::string(kFunctionPlotPathPort), SocketValueKind::Path}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Values};
    definition.parameters.push_back({std::string(kNodeGroupReferenceRole),
                                     std::string(kNodeGroupReferenceSchemaKey),
                                     ParameterValueKind::Integer, true, false, std::int64_t{0}});
    // Every other parameter is an operand: a socket that a wire can drive, so the range, the
    // resolution and the placement can follow other values.
    const auto operand = [&](const std::string_view role, const SocketValueKind kind,
                             ParameterValue value) {
        const auto key = operandSchemaKeyOf(kind);
        definition.inputs.push_back({std::string(role), kind, false});
        definition.parameters.push_back({std::string(role), std::string(key),
                                         parameterKindOf(kind), true, isAnimatableSchemaKey(key),
                                         std::move(value)});
    };
    operand(kFunctionPlotStartRole, SocketValueKind::Scalar, -5.0);
    operand(kFunctionPlotEndRole, SocketValueKind::Scalar, 5.0);
    operand(kFunctionPlotSamplesRole, SocketValueKind::Integer, kDefaultFunctionPlotSamples);
    operand(kFunctionPlotOriginRole, SocketValueKind::Vector2, Vec2d{0.0, 0.0});
    operand(kFunctionPlotScaleRole, SocketValueKind::Vector2, Vec2d{100.0, 100.0});
    operand(kFunctionPlotSmoothRole, SocketValueKind::Boolean, true);
    return definition;
}

// #598 Warp Path (Function): the group is read as `f`, and a point (x, y) of the warp's space goes to
// (x + amount * f(y / size), y). Its operands are Warp Path's, in the same order, less the function
// selector and the phase, so a set of value nodes that drives one drives the other.
[[nodiscard]] NodeDefinition warpPathFunctionDefinition() {
    NodeDefinition definition{{std::string(kWarpPathFunctionNodeType), kNodeGroupNodeSchemaVersion},
                              NodeLoweringKind::ValueFunction,
                              {},
                              {{std::string(kWarpPathFunctionPathPort), SocketValueKind::Path}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Values};
    definition.parameters.push_back({std::string(kNodeGroupReferenceRole),
                                     std::string(kNodeGroupReferenceSchemaKey),
                                     ParameterValueKind::Integer, true, false, std::int64_t{0}});
    const auto operand = [&](const std::string_view role, const SocketValueKind kind,
                             const std::string_view key, ParameterValue value) {
        definition.inputs.push_back({std::string(role), kind, false});
        definition.parameters.push_back({std::string(role), std::string(key),
                                         kind == SocketValueKind::Path ? ParameterValueKind::Path
                                                                       : parameterKindOf(kind),
                                         true, isAnimatableSchemaKey(key), std::move(value)});
    };
    operand("path", SocketValueKind::Path, kPathOperandParameterSchemaKey, PathValue{});
    operand("amount", SocketValueKind::Scalar, kWarpAmountSchemaKey, 20.0);
    operand("size", SocketValueKind::Scalar, kWarpSizeSchemaKey, 100.0);
    operand("tolerance", SocketValueKind::Scalar, kWarpToleranceSchemaKey, 0.25);
    operand("position", SocketValueKind::Vector2, kVector2OperandParameterSchemaKey, Vec2d{0.0, 0.0});
    operand("anchor", SocketValueKind::Vector2, kVector2OperandParameterSchemaKey, Vec2d{0.0, 0.0});
    operand("scale", SocketValueKind::Vector2, kVector2OperandParameterSchemaKey, Vec2d{1.0, 1.0});
    operand("rotation", SocketValueKind::Scalar, kScalarOperandParameterSchemaKey, 0.0);
    operand("skew", SocketValueKind::Scalar, kPathSkewSchemaKey, 0.0);
    operand("skewAxis", SocketValueKind::Scalar, kPathSkewAxisSchemaKey, 0.0);
    return definition;
}

[[nodiscard]] NodeDefinition generatedDefinition(const std::string_view typeId) {
    if (typeId == kFunctionNodeType)
        return functionDefinition();
    if (typeId == kWarpPathFunctionNodeType)
        return warpPathFunctionDefinition();
    if (typeId == kFunctionPlotNodeType)
        return functionPlotDefinition();
    NodeDefinition definition{{std::string(typeId), kNodeGroupNodeSchemaVersion},
                              typeId == kNodeGroupNodeType ? NodeLoweringKind::NodeGroup
                                                           : NodeLoweringKind::GroupBoundary,
                              {},
                              {},
                              {},
                              std::nullopt,
                              typeId == kNodeGroupNodeType ? NodeCardinality::Many
                                                           : NodeCardinality::OnePerComposition,
                              NodeCategory::Utilities};
    const bool input = typeId == kGroupInputNodeType;
    const bool output = typeId == kGroupOutputNodeType;
    if (typeId == kNodeGroupNodeType)
        definition.parameters.push_back({std::string(kNodeGroupReferenceRole),
                                         std::string(kNodeGroupReferenceSchemaKey),
                                         ParameterValueKind::Integer, true, false,
                                         std::int64_t{0}});
    for (const auto& slot : kNodeGroupSlots) {
        // Group Input publishes every slot and takes only values, as operands the instance fills.
        // Group Output takes every slot and republishes its values for whatever read the instance.
        // A Node Group takes every slot and publishes every slot.
        if (slot.pixel) {
            if (!input)
                definition.inputs.push_back({std::string(slot.name), slot.socket, false});
            if (!output)
                definition.outputs.push_back({std::string(slot.name), slot.socket});
            continue;
        }
        definition.inputs.push_back({std::string(slot.name), slot.socket, false});
        definition.parameters.push_back(slotParameter(slot));
        definition.outputs.push_back({std::string(slot.name), slot.socket});
    }
    return definition;
}

// A mixed 64-bit hash, used to derive the ids of the nodes an instance brings in. The top bit is
// set so a derived id can never meet one the project allocator hands out.
[[nodiscard]] std::uint64_t mixIds(std::uint64_t base, std::uint64_t inner) noexcept {
    std::uint64_t value = base * 0x9E3779B97F4A7C15ULL + inner + 0x7F4A7C15ULL;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value | (std::uint64_t{1} << 63);
}

[[nodiscard]] bool isInstanceType(const std::string_view typeId) noexcept {
    return usesGroupReference(typeId);
}

// What a definition's own composition holds that an instance must not bring along: the structure
// of a composition. Everything else is a node the group computes with.
[[nodiscard]] bool isCompositionStructure(const std::string_view typeId) noexcept {
    return typeId == kLayerStackNodeType || typeId == kCompositionOutputNodeType ||
           typeId == kLayerOutputNodeType || typeId == kAdjustmentNodeType;
}

class Expander final {
  public:
    Expander(const Project& project, const Composition& composition)
        : project_(project), work_(composition) {}

    [[nodiscard]] NodeGroupExpansion run() {
        for (std::size_t depth = 0; depth <= kMaximumNodeGroupDepth; ++depth) {
            std::vector<NodeId> instances;
            for (const auto& node : work_.graph().nodes())
                if (isInstanceType(node.typeId) && !result_.unresolved.contains(node.id) &&
                    !result_.functionBodies.contains(node.id))
                    instances.push_back(node.id);
            if (instances.empty())
                break;
            if (depth == kMaximumNodeGroupDepth) {
                for (const auto id : instances)
                    result_.unresolved.emplace(id, "Node Groups are nested too deeply");
                break;
            }
            for (const auto id : instances)
                expandInstance(id);
        }
        for (auto& [consumer, body] : result_.functionBodies)
            collectBody(body);
        result_.composition = std::move(work_);
        return std::move(result_);
    }

  private:
    [[nodiscard]] NodeId deriveNode(const NodeId base, const NodeId inner) {
        auto raw = mixIds(base.value(), inner.value());
        while (!usedNodes_.insert(raw).second)
            ++raw;
        return NodeId::fromRaw(raw);
    }
    [[nodiscard]] EdgeId deriveEdge(const NodeId base, const EdgeId inner) {
        auto raw = mixIds(base.value(), inner.value());
        while (!usedEdges_.insert(raw).second)
            ++raw;
        return EdgeId::fromRaw(raw);
    }
    [[nodiscard]] ParameterId deriveParameter(const NodeId base, const ParameterId inner) {
        auto raw = mixIds(base.value(), inner.value());
        while (!usedParameters_.insert(raw).second)
            ++raw;
        return ParameterId::fromRaw(raw);
    }

    [[nodiscard]] static bool isPixelPort(const std::string_view port) noexcept {
        const auto slot = std::ranges::find(kNodeGroupSlots, port, &NodeGroupSlot::name);
        return slot != kNodeGroupSlots.end() && slot->pixel;
    }

    void expandInstance(const NodeId instanceId) {
        const auto* found = work_.graph().findNode(instanceId);
        if (found == nullptr)
            return;
        const NodeRecord instance = *found;
        const auto reference = nodeGroupReference(instance, work_.parameters());
        if (!reference) {
            result_.unresolved.emplace(instanceId, "The Node Group names no group");
            return;
        }
        const auto* definition = project_.findComposition(*reference);
        if (definition == nullptr || !isNodeGroupDefinition(*definition)) {
            result_.unresolved.emplace(instanceId, "The group this Node Group names is missing");
            return;
        }
        const auto* input = findGroupInput(*definition);
        const auto* output = findGroupOutput(*definition);
        if (input == nullptr || output == nullptr) {
            result_.unresolved.emplace(instanceId, "The group has no Group Input and Group Output");
            return;
        }
        if (work_.graph().nodes().size() + definition->graph().nodes().size() >
            kMaximumExpandedNodes) {
            result_.unresolved.emplace(instanceId, "The expanded groups hold too many nodes");
            return;
        }

        std::unordered_map<NodeId, NodeId> mapped;
        for (const auto& node : definition->graph().nodes())
            if (!isCompositionStructure(node.typeId))
                mapped.emplace(node.id, deriveNode(instanceId, node.id));

        for (const auto& node : definition->graph().nodes()) {
            const auto id = mapped.find(node.id);
            if (id == mapped.end())
                continue;
            NodeRecord copy{id->second, node.typeId, {}, node.schemaVersion};
            for (const auto& binding : node.parameters) {
                const auto* record = definition->parameters().find(binding.parameterId);
                if (record == nullptr)
                    continue;
                ParameterRecord cloned{deriveParameter(instanceId, record->id), record->schemaKey,
                                       record->source};
                if (auto* driver = std::get_if<DriverBindingSource>(&cloned.source)) {
                    if (const auto source = mapped.find(driver->sourceNodeId);
                        source != mapped.end())
                        driver->sourceNodeId = source->second;
                } else if (const auto* curve = std::get_if<AnimationCurveSource>(&record->source)) {
                    // A curve keeps its id: the definition's animation is shared by every
                    // instance, and an id the working store already holds is that same curve.
                    if (work_.animationCurves().find(curve->curveId) == nullptr)
                        if (const auto* curveRecord = definition->animationCurves().find(curve->curveId))
                            (void)work_.animationCurves().insert(*curveRecord);
                }
                copy.parameters.push_back({binding.role, cloned.id});
                (void)work_.parameters().insert(std::move(cloned));
            }
            (void)work_.graph().addNode(std::move(copy));
            if (const auto layout = definition->nodeLayout().find(node.id);
                layout != definition->nodeLayout().end() && layout->second.muted)
                work_.nodeLayout()[id->second].muted = true;
        }
        for (const auto& edge : definition->graph().edges()) {
            const auto* destination = std::get_if<NodeInputRef>(&edge.destination);
            const auto from = mapped.find(edge.source.nodeId);
            if (destination == nullptr || from == mapped.end())
                continue;
            const auto to = mapped.find(destination->nodeId);
            if (to == mapped.end())
                continue;
            (void)work_.graph().addEdge({deriveEdge(instanceId, edge.id),
                                         {from->second, edge.source.port},
                                         NodeInputRef{to->second, destination->port}});
        }

        const NodeId boundaryInput = mapped.at(input->id);
        const NodeId boundaryOutput = mapped.at(output->id);
        if (instance.typeId == kNodeGroupNodeType)
            spliceInstance(instance, boundaryInput, boundaryOutput);
        else
            result_.functionBodies.emplace(instanceId, NodeGroupBody{boundaryInput, boundaryOutput, {}});
    }

    // A function answers on scalar1 alone. Cut everything else off Group Output (its pixel inputs,
    // and the drivers of its other value slots) so the body holds only what the answer reads, then
    // gather those nodes.
    void collectBody(NodeGroupBody& body) {
        const auto* output = work_.graph().findNode(body.output);
        if (output == nullptr)
            return;
        std::vector<EdgeId> incoming;
        for (const auto& edge : work_.graph().edges())
            if (const auto* destination = std::get_if<NodeInputRef>(&edge.destination);
                destination != nullptr && destination->nodeId == body.output)
                incoming.push_back(edge.id);
        for (const auto id : incoming)
            (void)work_.graph().eraseEdge(id);
        std::optional<ParameterId> answer;
        for (const auto& binding : output->parameters) {
            if (binding.role == kFunctionOutputSlot) {
                answer = binding.parameterId;
                continue;
            }
            const auto slot = std::ranges::find(kNodeGroupSlots, binding.role, &NodeGroupSlot::name);
            if (slot == kNodeGroupSlots.end() || slot->pixel)
                continue;
            if (const auto* record = work_.parameters().find(binding.parameterId);
                record != nullptr && std::holds_alternative<DriverBindingSource>(record->source))
                (void)work_.parameters().setSource(binding.parameterId,
                                                   ConstantValueSource{defaultValueOf(slot->socket)});
        }
        std::unordered_set<NodeId> seen{body.output};
        std::vector<NodeId> pending;
        const auto follow = [&](const ParameterId parameter) {
            if (const auto* record = work_.parameters().find(parameter))
                if (const auto* driver = std::get_if<DriverBindingSource>(&record->source))
                    if (seen.insert(driver->sourceNodeId).second)
                        pending.push_back(driver->sourceNodeId);
        };
        if (answer)
            follow(*answer);
        body.nodes.push_back(body.output);
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            body.nodes.push_back(id);
            if (const auto* node = work_.graph().findNode(id))
                for (const auto& binding : node->parameters)
                    follow(binding.parameterId);
            for (const auto& edge : work_.graph().edges())
                if (const auto* destination = std::get_if<NodeInputRef>(&edge.destination);
                    destination != nullptr && destination->nodeId == id &&
                    seen.insert(edge.source.nodeId).second)
                    pending.push_back(edge.source.nodeId);
        }
    }

    // The instance's own value sockets become the values Group Input publishes, its pixel inputs
    // replace the sources of the edges that left Group Input, and whatever read the instance now
    // reads what feeds Group Output.
    void spliceInstance(const NodeRecord& instance, const NodeId boundaryInput,
                        const NodeId boundaryOutput) {
        const NodeId instanceId = instance.id;
        const auto* clonedInput = work_.graph().findNode(boundaryInput);
        for (const auto& slot : kNodeGroupSlots) {
            if (slot.pixel || clonedInput == nullptr)
                continue;
            const auto outer = std::ranges::find(instance.parameters, slot.name, &ParameterBinding::role);
            const auto inner =
                std::ranges::find(clonedInput->parameters, slot.name, &ParameterBinding::role);
            if (outer == instance.parameters.end() || inner == clonedInput->parameters.end())
                continue;
            const auto* source = work_.parameters().find(outer->parameterId);
            if (source != nullptr)
                (void)work_.parameters().setSource(inner->parameterId, source->source);
        }

        std::unordered_map<std::string, OutputPortRef> fed;
        for (const auto& edge : work_.graph().edges())
            if (const auto* destination = std::get_if<NodeInputRef>(&edge.destination);
                destination != nullptr && destination->nodeId == instanceId)
                fed.emplace(destination->port, edge.source);

        // Edges that left Group Input take their source from the instance's matching input.
        std::vector<EdgeRecord> fromInput;
        for (const auto& edge : work_.graph().edges())
            if (edge.source.nodeId == boundaryInput && isPixelPort(edge.source.port))
                fromInput.push_back(edge);
        for (auto edge : fromInput) {
            (void)work_.graph().eraseEdge(edge.id);
            const auto source = fed.find(edge.source.port);
            if (source == fed.end())
                continue;
            edge.source = source->second;
            (void)work_.graph().addEdge(std::move(edge));
        }

        std::unordered_map<std::string, OutputPortRef> produced;
        for (const auto& edge : work_.graph().edges())
            if (const auto* destination = std::get_if<NodeInputRef>(&edge.destination);
                destination != nullptr && destination->nodeId == boundaryOutput)
                produced.emplace(destination->port, edge.source);

        // A muted instance is bypassed as a muted node is: each pixel output is the input of the
        // same name (image1, mask1, channels1) and the group's own nodes are left unread. Its
        // value outputs still read Group Output's republished values.
        const bool muted = nodeMuted(work_.nodeLayout(), instanceId);
        const auto& pixelOutputs = muted ? fed : produced;

        std::vector<EdgeRecord> fromInstance;
        for (const auto& edge : work_.graph().edges())
            if (edge.source.nodeId == instanceId)
                fromInstance.push_back(edge);
        for (auto edge : fromInstance) {
            (void)work_.graph().eraseEdge(edge.id);
            if (isPixelPort(edge.source.port)) {
                const auto source = pixelOutputs.find(edge.source.port);
                if (source == pixelOutputs.end())
                    continue;
                edge.source = source->second;
            } else {
                // A value read by an edge (a reroute) reads Group Output's republished value.
                edge.source.nodeId = boundaryOutput;
            }
            (void)work_.graph().addEdge(std::move(edge));
        }

        // A Mask the instance made may be a layer's matte.
        for (const auto& layer : std::vector<LayerOutputBoundary>(work_.graph().layerOutputs().begin(),
                                                                  work_.graph().layerOutputs().end())) {
            auto* target = work_.graph().findLayer(layer.layerId);
            if (target == nullptr)
                continue;
            const auto retarget = [&](std::optional<OutputPortRef>& reference) {
                if (!reference || reference->nodeId != instanceId)
                    return;
                const auto source = pixelOutputs.find(reference->port);
                if (source == pixelOutputs.end())
                    reference.reset();
                else
                    reference = source->second;
            };
            if (target->trackMatte)
                retarget(target->trackMatte->node);
            for (auto& mask : target->masks)
                retarget(mask.matte);
        }

        // Values that read the instance read Group Output's republished value.
        std::vector<std::pair<ParameterId, std::string>> drivenByInstance;
        for (const auto& record : work_.parameters().records())
            if (const auto* driver = std::get_if<DriverBindingSource>(&record.source);
                driver != nullptr && driver->sourceNodeId == instanceId)
                drivenByInstance.emplace_back(record.id, driver->outputPort);
        for (const auto& [parameter, port] : drivenByInstance)
            (void)work_.parameters().setSource(parameter, DriverBindingSource{boundaryOutput, port});

        (void)work_.graph().eraseNode(instanceId);
    }

    const Project& project_;
    Composition work_;
    NodeGroupExpansion result_;
    std::unordered_set<std::uint64_t> usedNodes_, usedEdges_, usedParameters_;
};

} // namespace

bool isNodeGroupBoundaryType(const std::string_view typeId) noexcept {
    return typeId == kGroupInputNodeType || typeId == kGroupOutputNodeType;
}

bool isFunctionConsumerType(const std::string_view typeId) noexcept {
    return typeId == kFunctionNodeType || typeId == kFunctionPlotNodeType ||
           typeId == kWarpPathFunctionNodeType;
}

bool usesGroupReference(const std::string_view typeId) noexcept {
    return typeId == kNodeGroupNodeType || isFunctionConsumerType(typeId);
}

std::vector<NodeDefinition> nodeGroupNodeDefinitions() {
    return {generatedDefinition(kGroupInputNodeType), generatedDefinition(kGroupOutputNodeType),
            generatedDefinition(kNodeGroupNodeType), generatedDefinition(kFunctionNodeType),
            generatedDefinition(kFunctionPlotNodeType), generatedDefinition(kWarpPathFunctionNodeType)};
}

bool hasValidNodeGroupShape(const NodeDefinition& definition) {
    const auto typeId = definition.key.typeId;
    if (typeId != kGroupInputNodeType && typeId != kGroupOutputNodeType &&
        typeId != kNodeGroupNodeType && !isFunctionConsumerType(typeId))
        return false;
    return definition == generatedDefinition(typeId);
}

std::optional<CompositionId> nodeGroupReference(const NodeRecord& node,
                                                const ParameterStore& parameters) {
    const auto binding = std::ranges::find(node.parameters, kNodeGroupReferenceRole,
                                           &ParameterBinding::role);
    if (binding == node.parameters.end())
        return std::nullopt;
    const auto* record = parameters.find(binding->parameterId);
    const auto* constant =
        record == nullptr ? nullptr : std::get_if<ConstantValueSource>(&record->source);
    const auto* value = constant == nullptr ? nullptr : std::get_if<std::int64_t>(&constant->value);
    if (value == nullptr || *value <= 0)
        return std::nullopt;
    return CompositionId::fromRaw(static_cast<std::uint64_t>(*value));
}

const NodeRecord* findGroupInput(const Composition& composition) noexcept {
    const auto nodes = composition.graph().nodes();
    const auto found = std::ranges::find(nodes, kGroupInputNodeType, &NodeRecord::typeId);
    return found == nodes.end() ? nullptr : &*found;
}

const NodeRecord* findGroupOutput(const Composition& composition) noexcept {
    const auto nodes = composition.graph().nodes();
    const auto found = std::ranges::find(nodes, kGroupOutputNodeType, &NodeRecord::typeId);
    return found == nodes.end() ? nullptr : &*found;
}

bool isNodeGroupDefinition(const Composition& composition) noexcept {
    return findGroupOutput(composition) != nullptr;
}

NodeGroupExpansion expandNodeGroups(const Project& project, const Composition& composition) {
    const bool any = std::ranges::any_of(composition.graph().nodes(), [](const NodeRecord& node) {
        return isInstanceType(node.typeId);
    });
    if (!any)
        return {};
    return Expander(project, composition).run();
}

std::optional<NodeGroupSlotTarget> nodeGroupSlotTarget(const Project& project,
                                                       const NodeRecord& instance,
                                                       const ParameterStore& parameters,
                                                       const std::string_view slot) {
    if (instance.typeId != kNodeGroupNodeType)
        return std::nullopt;
    const auto reference = nodeGroupReference(instance, parameters);
    const auto* group = reference ? project.findComposition(*reference) : nullptr;
    const auto* input = group != nullptr ? findGroupInput(*group) : nullptr;
    if (input == nullptr)
        return std::nullopt;
    std::optional<NodeGroupSlotTarget> found;
    for (const auto& edge : group->graph().edges()) {
        if (edge.source.nodeId != input->id || edge.source.port != slot)
            continue;
        const auto* destination = std::get_if<NodeInputRef>(&edge.destination);
        if (destination == nullptr || found.has_value())
            return std::nullopt;
        const auto* node = group->graph().findNode(destination->nodeId);
        const auto* definition =
            node != nullptr ? builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
                            : nullptr;
        if (definition == nullptr)
            return std::nullopt;
        const auto parameter = std::ranges::find(definition->parameters, destination->port,
                                                 &ParameterDefinition::role);
        if (parameter == definition->parameters.end())
            return std::nullopt;
        found =
            NodeGroupSlotTarget{node->id, node->typeId, destination->port, parameter->schemaKey};
    }
    // A value slot reaches an inner parameter as a driver binding rather than an edge.
    for (const auto& node : group->graph().nodes())
        for (const auto& binding : node.parameters) {
            const auto* record = group->parameters().find(binding.parameterId);
            const auto* driver =
                record != nullptr ? std::get_if<DriverBindingSource>(&record->source) : nullptr;
            if (driver == nullptr || driver->sourceNodeId != input->id ||
                driver->outputPort != slot)
                continue;
            if (found.has_value())
                return std::nullopt;
            found = NodeGroupSlotTarget{node.id, node.typeId, binding.role, record->schemaKey};
        }
    return found;
}

} // namespace bloom::document

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/commands/operations.hpp>

#include "node_operation_support.hpp"
#include "render_footprint.hpp"

#include <bloom/core/value_primitives.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <string>
#include <variant>

namespace bloom::commands {
bool canApplyNodeOperation(const document::Snapshot& snapshot, const Operation& operation) {
    // #357: a scratch draft shares the snapshot's content and validates only what the operation
    // changed, instead of rebuilding and revalidating a whole isolated document per query.
    auto draft = document::Draft::isolatedFrom(snapshot);
    if (operation.apply(draft).status == OperationStatus::Rejected)
        return false;
    // #502: the command stack reconciles entries and Layer nodes before it validates.
    const auto reconciled = reconcileEntryLayers(snapshot.project(), draft);
    return !reconciled.failure && draft.valid();
}

OperationResult checkAddNodeEligibility(const document::Project& project,
                                        const document::IdAllocator& ids,
                                        const document::CompositionId compositionId,
                                        const std::string_view nodeTypeId,
                                        const document::Vec2d layoutPosition,
                                        const document::NodeDefinitionRegistry& registry) {
    const auto* composition = project.findComposition(compositionId);
    if (!composition)
        return detail::missingComposition(compositionId);
    if (!detail::finite(layoutPosition))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Node position must be finite");
    if (!registry.isFrozen())
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node authoring requires a frozen definition registry");
    const document::NodeDefinition* definition = nullptr;
    for (const auto& candidate : registry.definitions()) {
        if (candidate.key.typeId == nodeTypeId &&
            (!definition || candidate.key.schemaVersion > definition->key.schemaVersion))
            definition = &candidate;
    }
    if (!definition)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node type is not registered: " + std::string(nodeTypeId));
    if (definition->cardinality == document::NodeCardinality::OnePerComposition &&
        std::ranges::any_of(composition->graph().nodes(), [nodeTypeId](const auto& existing) {
            return existing.typeId == nodeTypeId;
        }))
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Only one " + std::string(nodeTypeId) +
                                             " node is allowed per composition");
    auto available = ids;
    if (!available.allocateNode())
        return detail::exhaustedIds("node");
    for (std::size_t index = 0; index < definition->parameters.size(); ++index)
        if (!available.allocateParameter())
            return detail::exhaustedIds("node and parameter");
    if (nodeTypeId == document::kLayerOutputNodeType && !available.allocateLayer())
        return detail::exhaustedIds("node, parameter and layer");
    return OperationResult::applied();
}

OperationResult SetNodeParameterValue::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    auto* node = composition ? composition->graph().findNode(nodeId_) : nullptr;
    if (node == nullptr)
        return detail::missingNode(composition, compositionId_, nodeId_);
    if (const auto binding = std::ranges::find(node->parameters, role_,
                                               &document::ParameterBinding::role);
        binding != node->parameters.end())
        return SetParameterSource(compositionId_, binding->parameterId,
                                  document::ConstantValueSource{value_})
            .apply(draft);
    const auto* definition =
        document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
    const auto parameter =
        definition == nullptr
            ? definition->parameters.end()
            : std::ranges::find(definition->parameters, role_, &document::ParameterDefinition::role);
    if (definition == nullptr || parameter == definition->parameters.end() || parameter->required)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Node " + std::to_string(nodeId_.value()) + " (" + node->typeId +
                ") has no optional parameter \"" + role_ + "\"");
    const auto parameterId = draft.ids().allocateParameter();
    if (!parameterId)
        return detail::exhaustedIds("parameter");
    if (!composition->parameters().insert(
            {*parameterId, parameter->schemaKey, document::ConstantValueSource{value_}}))
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "The value for parameter \"" + role_ + "\" of node " +
                std::to_string(nodeId_.value()) + " violates the parameter schema");
    node->parameters.push_back({role_, *parameterId});
    auto result = OperationResult::applied();
    result.outputs.push_back({"parameter", *parameterId});
    return result;
}

std::string_view AddNode::typeId() const noexcept { return "bloom.node.add"; }
OperationResult AddNode::apply(document::Draft& draft) const {
    if (auto eligibility = checkAddNodeEligibility(draft.project(), draft.ids(), compositionId_,
                                                   nodeTypeId_, layoutPosition_, registry_);
        eligibility.status == OperationStatus::Rejected)
        return eligibility;
    auto* composition = draft.project().findComposition(compositionId_);
    // Issue #355: a node added with no links is not compiled, so adding it publishes an empty
    // footprint whenever the compiled topology provably stays the same.
    const auto reachableBefore = document::RenderReachability::of(*composition, registry_);
    const document::NodeDefinition* definition = nullptr;
    for (const auto& candidate : registry_.definitions()) {
        if (candidate.key.typeId == nodeTypeId_ &&
            (!definition || candidate.key.schemaVersion > definition->key.schemaVersion))
            definition = &candidate;
    }
    const auto nodeId = draft.ids().allocateNode();
    if (!nodeId)
        return detail::exhaustedIds("node");
    document::NodeRecord node{*nodeId, definition->key.typeId, {}, definition->key.schemaVersion};
    std::vector<OperationOutput> outputs{{std::string(kAddNodeOutput), *nodeId}};
    for (const auto& parameter : definition->parameters) {
        // ADR 0024: Time Remap is bound only by EnableLayerTimeRemap; its absence means disabled.
        if (parameter.schemaKey == document::kTimeRemapParameterSchemaKey)
            continue;
        const auto parameterId = draft.ids().allocateParameter();
        if (!parameterId)
            return detail::exhaustedIds("node and parameter");
        auto value = parameter.defaultValue;
        if (parameter.schemaKey == document::kSolidWidthParameterSchemaKey)
            value = static_cast<double>(composition->format().width());
        else if (parameter.schemaKey == document::kSolidHeightParameterSchemaKey)
            value = static_cast<double>(composition->format().height());
        else if (parameter.schemaKey == document::kGradientStartParameterSchemaKey &&
                 nodeTypeId_ == document::kRadialGradientNodeType)
            value = document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                                    static_cast<double>(composition->format().height()) / 2.0};
        else if (parameter.schemaKey == document::kGradientEndParameterSchemaKey)
            value = nodeTypeId_ == document::kRadialGradientNodeType
                        ? document::Vec2d{static_cast<double>(composition->format().width()),
                                          static_cast<double>(composition->format().height()) / 2.0}
                        : document::Vec2d{static_cast<double>(composition->format().width()), 0.0};
        // #512: a new Camera Shake gets a seed of its own, hashed from its node id, so two shakes
        // added with default settings never move together. It is an ordinary stored constant. A
        // new Frame Rate Hold's cadence seed is the same.
        else if (parameter.schemaKey == document::motionSchemaKey(document::kCameraShakeNodeType, "seed") ||
                 parameter.schemaKey ==
                     document::motionSchemaKey(document::kFrameRateHoldNodeType, "seed"))
            value = static_cast<std::int64_t>(
                core::primitives::hashWord(static_cast<std::int64_t>(nodeId->value())) %
                1'000'000ULL);
        else if (parameter.schemaKey == document::kPositionParameterSchemaKey)
            value = document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                                    static_cast<double>(composition->format().height()) / 2.0};
        if (!composition->parameters().insert({*parameterId, parameter.schemaKey,
                                               document::ConstantValueSource{std::move(value)}}))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Node definition default violates its parameter schema");
        node.parameters.push_back({parameter.role, *parameterId});
        outputs.push_back({"parameter." + parameter.role, *parameterId});
    }
    if (!composition->graph().addNode(std::move(node)))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Node could not be inserted");
    // The requested point is still authoritative when it is clear: it is where the artist pointed
    // (task N1, item 3). When the card would touch another, only this new card moves, to the
    // nearest grid position kNodeLayoutGap past its neighbour (item 4); existing authoring
    // positions are never rewritten by AddNode.
    composition->nodeLayout()[*nodeId] = {layoutPosition_, 128.0, false, false};
    const auto floor = nodeCardFloorSizes(*composition, sizes_);
    const auto placement = placeNodeCard(*composition, estimatedNodeCardSize(*composition, *nodeId),
                                         layoutPosition_, nodeCardSizes(*composition, sizes_),
                                         {*nodeId}, kNodeLayoutGrid, &floor, floor.at(*nodeId));
    composition->nodeLayout()[*nodeId] = {placement, 128.0, false, false};
    // A Layer node gets its layer IDENTITY here, at creation, rather than on its first connection
    // to Merge (task FIX1, item B). The alternative -- waiting for the Merge link -- would leave a
    // card on the canvas with no name to rename, no LayerId for Properties and the Timeline to
    // address, and two different shapes of Layer node to reason about. The TIMELINE still lists a
    // layer only once it has a stack slot, which is the thing that actually makes it draw, so
    // "added" and "participating" stay distinguishable without a second node shape.
    if (nodeTypeId_ == document::kLayerOutputNodeType) {
        const auto layerId = draft.ids().allocateLayer();
        if (!layerId)
            return detail::exhaustedIds("layer");
        const auto number = composition->graph().layerOutputs().size() + 1;
        if (!composition->graph().addLayerOutput({*nodeId, *layerId,
                                                  "Layer " + std::to_string(number),
                                                  std::string(document::kLayerOutputOutputPort)}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer boundary could not be inserted");
        outputs.push_back({"layer", *layerId});
    }
    auto result = OperationResult::applied(std::move(outputs));
    result.affectedTimes =
        detail::unreachableEditFootprint(*composition, *reachableBefore, {*nodeId}, registry_);
    return result;
}

namespace {

// TEMPORAL-DELETE. Inspects the graph BEFORE any erase and decides whether removing exactly this
// set of nodes can only change rendered output inside the union of the removed layer boundaries'
// half-open active spans. Conservative default: any mismatch yields std::nullopt, which means the
// whole render may have changed and no reuse evidence is published.
[[nodiscard]] std::optional<AffectedTimeFootprint>
removalTimeFootprint(const document::Composition& composition,
                     const std::set<document::NodeId>& nodes) {
    const auto& graph = composition.graph();
    const auto duration = composition.duration();

    // Every removed node must be a known layer boundary. Anything else (a source, effect, reroute,
    // merge, or value node) is unproven.
    std::vector<const document::LayerOutputBoundary*> boundaries;
    std::set<document::LayerId> removedLayers;
    for (const auto id : nodes) {
        // #502: a Merge's entry layers share its node id; removing a Merge is not a layer removal.
        const auto boundary = std::ranges::find_if(graph.layerOutputs(), [id](const auto& layer) {
            return !layer.entryOwned() && layer.nodeId == id;
        });
        if (boundary == graph.layerOutputs().end())
            return std::nullopt;
        boundaries.push_back(&*boundary);
        removedLayers.insert(boundary->layerId);
    }

    // A removed track matte source changes its surviving targets outside the removed span.
    for (const auto& layer : graph.layerOutputs())
        if (!nodes.contains(layer.nodeId) && layer.trackMatte &&
            removedLayers.contains(layer.trackMatte->source))
            return std::nullopt;

    // A surviving child parented to a removed boundary follows only the deleted head, so its output
    // outside the deleted span changes too.
    for (const auto& layer : graph.layerOutputs()) {
        if (nodes.contains(layer.nodeId))
            continue;
        if (layer.parent.has_value() && removedLayers.contains(*layer.parent))
            return std::nullopt;
    }

    // A removed boundary's output may not feed any non-merge consumer: node inputs, effects,
    // reroutes and the composition output all consume the whole output, including outside the
    // visible span.
    for (const auto& edge : graph.edges()) {
        if (!nodes.contains(edge.source.nodeId))
            continue;
        if (!std::holds_alternative<document::LayerStackInputRef>(edge.destination))
            return std::nullopt;
    }

    // A surviving parameter driven by a removed node's output depends on it at every time.
    for (const auto& record : composition.parameters().records()) {
        if (const auto* driver = std::get_if<document::DriverBindingSource>(&record.source)) {
            if (nodes.contains(driver->sourceNodeId))
                return std::nullopt;
        }
    }

    // Removing a solo layer can UNsuppress other layers across the whole composition.
    for (const auto* boundary : boundaries) {
        if (boundary->solo)
            return std::nullopt;
    }

    // A muted Layer Stack compiles only its first slot (snapshot_compiler_lowering.ipp). Removing a
    // participating layer can promote a later layer into that slot, changing output wherever the
    // promoted layer is active -- which need not lie inside the removed boundary's own span. That
    // is unprovable, so a removed boundary participating in a muted stack forces whole-render.
    for (const auto* boundary : boundaries) {
        const bool participatesInMutedStack =
            std::ranges::any_of(graph.merges(), [&](const document::LayerStack& stack) {
                const auto layout = composition.nodeLayout().find(stack.nodeId());
                if (layout == composition.nodeLayout().end() || !layout->second.muted)
                    return false;
                return std::ranges::any_of(stack.entries(),
                                           [&](const document::LayerStackEntry& entry) {
                                               return entry.layerId == boundary->layerId;
                                           });
            });
        if (participatesInMutedStack)
            return std::nullopt;
    }

    // The change is confined to where a removed, merge-participating boundary was active. A
    // boundary with no merge slot is isolated (no outgoing edges were allowed above) and
    // contributes nothing.
    std::vector<AffectedTimeRange> intervals;
    for (const auto* boundary : boundaries) {
        const bool participating = std::ranges::any_of(graph.merges(), [&](const auto& merge) {
            return std::ranges::any_of(merge.entries(),
                                       [&](const document::LayerStackEntry& entry) {
                                           return entry.layerId == boundary->layerId;
                                       });
        });
        if (!participating)
            continue;
        intervals.push_back({boundary->inPoint, boundary->endPoint(duration)});
    }
    return normalizeAffectedTimeFootprint(AffectedTimeFootprint{.compositionId = composition.id(),
                                                                .intervals = std::move(intervals)});
}

// Everything a removal changes besides the removed nodes themselves: each consumer of a removed
// output (including a Merge losing a slot), each node whose parameter a removed node drives, each
// Merge listing a removed layer, and each surviving parent whose transform-parent status may lapse.
[[nodiscard]] std::set<document::NodeId>
removalTouchedNodes(const document::Composition& composition,
                    const std::set<document::NodeId>& nodes) {
    const auto& graph = composition.graph();
    std::set<document::NodeId> touched = nodes;
    for (const auto& edge : graph.edges())
        if (nodes.contains(edge.source.nodeId))
            touched.insert(detail::destinationNode(edge.destination));
    std::set<document::LayerId> layers;
    for (const auto& boundary : graph.layerOutputs()) {
        if (!nodes.contains(boundary.nodeId))
            continue;
        layers.insert(boundary.layerId);
        if (boundary.parent)
            if (const auto* parent = graph.findLayer(*boundary.parent))
                touched.insert(parent->nodeId);
    }
    for (const auto& merge : graph.merges())
        for (const auto& entry : merge.entries())
            if (layers.contains(entry.layerId))
                touched.insert(merge.nodeId());
    for (const auto& node : graph.nodes())
        for (const auto& binding : node.parameters) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* driver =
                parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                          : nullptr;
            if (driver != nullptr && nodes.contains(driver->sourceNodeId))
                touched.insert(node.id);
        }
    return touched;
}

} // namespace

std::string_view RemoveNodes::typeId() const noexcept { return "bloom.node.remove"; }
OperationResult RemoveNodes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::missingComposition(compositionId_);
    if (nodes_.empty())
        return OperationResult::noChange();
    std::set<document::ParameterId> candidates;
    for (const auto id : nodes_) {
        const auto* node = composition->graph().findNode(id);
        if (!node)
            return detail::missingNode(composition, compositionId_, id);
        if (detail::protectedNode(composition->graph(), id))
            return OperationResult::rejected(
                OperationIssueCode::Unsupported,
                "Node " + std::to_string(id.value()) +
                    " is the composition Output or the Merge connected to it and cannot be removed");
        for (const auto& binding : node->parameters)
            candidates.insert(binding.parameterId);
        // ADR 0028: a removed layer's masks own parameters (and curves) outside node bindings;
        // #502 so do a removed Merge's entry layers.
        for (const auto& boundary : composition->graph().layerOutputs()) {
            // #383: a matte mask reading a removed node is removed with it.
            for (const auto& mask : boundary.masks)
                if (mask.matte && mask.matte->nodeId == id)
                    for (const auto& [parameterId, schemaKey] : document::layerMaskParameters(mask)) {
                        (void)schemaKey;
                        candidates.insert(parameterId);
                    }
            if (boundary.nodeId != id)
                continue;
            for (const auto& mask : boundary.masks)
                for (const auto& [parameterId, schemaKey] : document::layerMaskParameters(mask)) {
                    (void)schemaKey;
                    candidates.insert(parameterId);
                }
            for (const auto& binding : boundary.parameters)
                candidates.insert(binding.parameterId);
        }
    }
    // Classify from the pre-erase graph. std::nullopt is the conservative whole-render default; a
    // present footprint (possibly empty) is provable bounded reuse evidence. Issue #355: removing
    // nodes the compiler never reaches is render-neutral, whatever kind of node they are.
    const auto reachableBefore = document::RenderReachability::of(*composition, registry_);
    const auto touched = removalTouchedNodes(*composition, nodes_);
    auto footprint = removalTimeFootprint(*composition, nodes_);
    for (const auto id : nodes_) {
        (void)composition->graph().eraseNode(id);
        composition->nodeLayout().erase(id);
    }
    // A removed node leaves its frame, and a frame with nothing left in it goes with it.
    (void)detail::detachFromNodeGroups(*composition, nodes_);
    detail::eraseOrphanedParameters(*composition, candidates);
    if (auto neutral =
            detail::unreachableEditFootprint(*composition, *reachableBefore, touched, registry_))
        footprint = std::move(neutral);
    auto result = OperationResult::applied();
    result.affectedTimes = footprint;
    return result;
}

std::string_view RenameLayer::typeId() const noexcept { return "bloom.layer.rename"; }
OperationResult RenameLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::missingComposition(compositionId_);
    if (!document::isValidHumanFacingName(name_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer name must be valid nonempty UTF-8");
    for (const auto& layer : composition->graph().layerOutputs()) {
        if (layer.layerId != layerId_)
            continue;
        // Renaming is the artist taking the name over: the layer stops following its source.
        if (layer.name == name_ && !layer.nameFollowsSource)
            return OperationResult::noChange();
        (void)composition->graph().renameLayer(layerId_, name_);
        if (auto* renamed = composition->graph().findLayer(layerId_))
            renamed->nameFollowsSource = false;
        return OperationResult::applied();
    }
    return detail::missingLayer(composition, compositionId_, layerId_);
}

std::string_view ConnectPorts::typeId() const noexcept { return "bloom.node.connect"; }
OperationResult ConnectPorts::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::missingComposition(compositionId_);
    auto& graph = composition->graph();
    const auto reachableBefore = document::RenderReachability::of(*composition, registry_);
    const auto neutral = [&](OperationResult result) {
        result.affectedTimes = detail::unreachableEditFootprint(
            *composition, *reachableBefore, {detail::destinationNode(destination_)}, registry_);
        return result;
    };
    const auto sourceKind = graph.outputKind(source_, registry_);
    const auto inputKind = graph.inputKind(destination_, registry_);
    // An unconnected REROUTE has no kind yet, and that is not a missing port -- it is the whole of
    // what "a reroute takes the kind of the link it joins" means (task FIX1, item I). The first
    // link into one is accepted whatever it carries; every link after it is checked against what
    // the reroute now holds, because by then outputKind()/inputKind() answer with that kind.
    const auto kindless = [&graph](const document::NodeId nodeId) {
        const auto* node = graph.findNode(nodeId);
        return node != nullptr && document::isRerouteNodeType(node->typeId);
    };
    const bool sourceIsKindlessReroute = !sourceKind && kindless(source_.nodeId);
    const bool destinationIsKindlessReroute =
        !inputKind && kindless(detail::destinationNode(destination_));
    if (!sourceKind && !sourceIsKindlessReroute)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Output \"" + source_.port + "\" of node " + std::to_string(source_.nodeId.value()) +
                " is not a registered output port in composition " +
                std::to_string(compositionId_.value()));
    if (!inputKind && !destinationIsKindlessReroute)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "The " + detail::describeInput(destination_) +
                " is not a registered input port in composition " +
                std::to_string(compositionId_.value()));
    if (sourceKind && inputKind && !graph.acceptsConnection(*sourceKind, destination_, registry_))
        return OperationResult::rejected(
            OperationIssueCode::SocketKindMismatch,
            "Output \"" + source_.port + "\" of node " + std::to_string(source_.nodeId.value()) +
                " cannot connect to the " + detail::describeInput(destination_) +
                ": their socket kinds do not match");
    // #543: the first link into a gradient stop's socket binds the stop's own link record: a
    // parameter holding only the driver, bound under the socket's name beside the gradient.
    if (const auto* fixed = std::get_if<document::NodeInputRef>(&destination_);
        fixed != nullptr && detail::parameterSocketBinding(graph, destination_) == nullptr)
        if (const auto stop = document::parseGradientStopPort(fixed->port)) {
            auto* node = graph.findNode(fixed->nodeId);
            if (node == nullptr ||
                std::ranges::none_of(node->parameters, [&](const document::ParameterBinding& b) {
                    return b.role == stop->role;
                }))
                return OperationResult::rejected(
                    OperationIssueCode::InvalidTarget,
                    "Node " + std::to_string(fixed->nodeId.value()) + " has no \"" + stop->role +
                        "\" gradient; edit the gradient before linking the stop \"" +
                        fixed->port + "\"");
            const auto parameterId = draft.ids().allocateParameter();
            if (!parameterId)
                return detail::exhaustedIds("parameter");
            if (!composition->parameters().insert(
                    {*parameterId, std::string(document::gradientStopSchemaKey(stop->component)),
                     document::DriverBindingSource{source_.nodeId, source_.port}}))
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "The driver binding for the " + detail::describeInput(destination_) +
                        " could not be inserted into composition " +
                        std::to_string(compositionId_.value()));
            node->parameters.push_back({fixed->port, *parameterId});
            if (const auto failure = detail::validateGraph(*composition, registry_))
                return *failure;
            return neutral(OperationResult::applied({{"parameter", *parameterId}}));
        }
    // Task S7: a link into an operand socket is the PARAMETER's driver binding, not an edge. One
    // authored value, one durable record of where it comes from -- so there is nothing for an edge
    // and a binding to disagree about, and DisconnectInput has one thing to undo.
    if (const auto* binding = detail::parameterSocketBinding(graph, destination_)) {
        const auto* parameter = composition->parameters().find(binding->parameterId);
        if (parameter == nullptr)
            return detail::brokenReference(
                "The " + detail::describeInput(destination_) + " is bound to parameter " +
                std::to_string(binding->parameterId.value()) + ", which composition " +
                std::to_string(compositionId_.value()) + " does not hold");
        const document::DriverBindingSource driver{source_.nodeId, source_.port};
        if (std::holds_alternative<document::DriverBindingSource>(parameter->source) &&
            std::get<document::DriverBindingSource>(parameter->source) == driver)
            return OperationResult::noChange();
        if (!composition->parameters().setSource(binding->parameterId, driver))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "The driver binding of parameter " + std::to_string(binding->parameterId.value()) +
                    " could not be replaced for the " + detail::describeInput(destination_));
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return neutral(OperationResult::applied());
    }
    // A link into Merge's ordered multi-input whose slot id is the invalid sentinel MEANS "make a
    // new slot here" (task FIX1, item B): the stack slot is what a Layer node's connection to Merge
    // IS, so creating it is part of connecting rather than a separate command the artist has to
    // find.
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&destination_);
        slot != nullptr && !slot->slotId.isValid()) {
        auto* stack = graph.merge(slot->stackNodeId);
        if (!stack)
            return detail::brokenReference("Merge node " + std::to_string(slot->stackNodeId.value()) +
                                           " does not exist in composition " +
                                           std::to_string(compositionId_.value()));
        const auto boundaries = graph.layerOutputs();
        // #502: a Merge's own entry layers share its node id; only a Layer node names a slot.
        const auto boundary = std::ranges::find_if(boundaries, [this](const auto& candidate) {
            return !candidate.entryOwned() && candidate.nodeId == source_.nodeId;
        });
        const auto layerId = boundary == boundaries.end() ? document::LayerId{} : boundary->layerId;
        // An audio edge shares its Layer's existing row rather than opening a second one: one slot
        // per layer carries both roles (docs/architecture/layer-graph-model.md, Audio Sources And
        // Layers). A content drop always opens a fresh row -- the image edge is what puts a Layer
        // in the stack in the first place -- so this reuse applies only to the audio role.
        const document::LayerStackEntry* existingSlot = nullptr;
        if (slot->role == document::kLayerStackAudioInputRole && layerId.isValid()) {
            const auto entries = stack->entries();
            const auto found =
                std::ranges::find(entries, layerId, &document::LayerStackEntry::layerId);
            if (found != entries.end())
                existingSlot = &*found;
        }
        document::LayerSlotId slotId;
        if (existingSlot != nullptr) {
            slotId = existingSlot->slotId;
        } else {
            const auto allocatedSlotId = draft.ids().allocateLayerSlot();
            if (!allocatedSlotId)
                return detail::exhaustedIds("layer slot");
            slotId = *allocatedSlotId;
        }
        const auto edgeId = draft.ids().allocateEdge();
        if (!edgeId)
            return detail::exhaustedIds("edge");
        if (existingSlot == nullptr) {
            if (!stack->append({slotId, layerId}))
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "Slot " + std::to_string(slotId.value()) + " could not be added to Merge node " +
                        std::to_string(slot->stackNodeId.value()) + " for layer " +
                        std::to_string(layerId.value()));
            if (insertBefore_.has_value() && !stack->moveBefore(slotId, insertBefore_))
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "Slot " + std::to_string(slotId.value()) +
                        " could not be placed before the requested slot on Merge node " +
                        std::to_string(slot->stackNodeId.value()));
        }
        if (!graph.addEdge({*edgeId, source_,
                            document::LayerStackInputRef{slot->stackNodeId, slotId, slot->role}},
                           registry_))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "The connection from node " + std::to_string(source_.nodeId.value()) + " to slot " +
                    std::to_string(slotId.value()) + " of Merge node " +
                    std::to_string(slot->stackNodeId.value()) + " could not be inserted");
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return neutral(OperationResult::applied(
            {{std::string(kConnectPortsSlotOutput), slotId}, {"edge", *edgeId}}));
    }
    const auto* previous = detail::inputEdge(graph, destination_);
    if (previous && previous->source == source_)
        return OperationResult::noChange();
    const auto edgeId = previous ? std::optional(previous->id) : draft.ids().allocateEdge();
    if (!edgeId)
        return detail::exhaustedIds("edge");
    if (previous)
        (void)graph.eraseEdge(*edgeId);
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&destination_)) {
        auto* stack = graph.merge(slot->stackNodeId);
        if (!stack || !stack->find(slot->slotId))
            return detail::brokenReference("The " + detail::describeInput(destination_) +
                                           " does not exist in composition " +
                                           std::to_string(compositionId_.value()));
        const auto entries = stack->entries();
        const auto found =
            std::ranges::find(entries, slot->slotId, &document::LayerStackEntry::slotId);
        const auto next = std::next(found) == entries.end()
                              ? std::nullopt
                              : std::optional(std::next(found)->slotId);
        const auto boundaries = graph.layerOutputs();
        const auto boundary = std::ranges::find_if(boundaries, [this](const auto& candidate) {
            return !candidate.entryOwned() && candidate.nodeId == source_.nodeId;
        });
        const auto replacedLayer = found->layerId;
        // #502: an entry keeps its own layer when another plain image replaces its content; a new
        // Layer node source takes the entry over (the reconcile pass settles the entry layer).
        const auto* own = document::entryOwnedLayer(graph, slot->stackNodeId, slot->slotId);
        const auto layerId = boundary != boundaries.end() ? boundary->layerId
                             : own != nullptr ? own->layerId
                                 : document::LayerId{};
        (void)stack->erase(slot->slotId);
        if (!stack->append({slot->slotId, layerId}) || !stack->moveBefore(slot->slotId, next))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "The source of slot " + std::to_string(slot->slotId.value()) + " of Merge node " +
                    std::to_string(slot->stackNodeId.value()) + " could not be replaced");
        if (replacedLayer != layerId)
            graph.clearTrackMattesOutsideMerge(replacedLayer);
    }
    if (!graph.addEdge({*edgeId, source_, destination_}, registry_))
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "The connection from output \"" + source_.port + "\" of node " +
                std::to_string(source_.nodeId.value()) + " to the " +
                detail::describeInput(destination_) + " could not be inserted");
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    return neutral(OperationResult::applied({{"edge", *edgeId}}));
}

std::string_view SetMergeEnabled::typeId() const noexcept { return "bloom.merge.set-enabled"; }
OperationResult SetMergeEnabled::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    auto* merge = composition ? composition->graph().merge(mergeId_) : nullptr;
    if (!merge)
        return detail::brokenReference("Merge node " + std::to_string(mergeId_.value()) +
                                       " does not exist in composition " +
                                       std::to_string(compositionId_.value()));
    if (merge->enabled() == enabled_)
        return OperationResult::noChange();
    merge->setEnabled(enabled_);
    return OperationResult::applied();
}

std::string_view ReorderMergeInput::typeId() const noexcept { return "bloom.merge.reorder-input"; }
OperationResult ReorderMergeInput::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    auto* stack = composition ? composition->graph().merge(mergeId_) : nullptr;
    if (!stack)
        return detail::brokenReference("Merge node " + std::to_string(mergeId_.value()) +
                                       " does not exist in composition " +
                                       std::to_string(compositionId_.value()));
    if (!stack->find(slotId_))
        return detail::brokenReference("Slot " + std::to_string(slotId_.value()) +
                                       " does not exist on Merge node " +
                                       std::to_string(mergeId_.value()));
    if (newIndex_ >= stack->entries().size())
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Position " + std::to_string(newIndex_) + " is outside Merge node " +
                std::to_string(mergeId_.value()) + ", which has " +
                std::to_string(stack->entries().size()) + " inputs");
    const auto entries = stack->entries();
    const auto found = std::ranges::find(entries, slotId_, &document::LayerStackEntry::slotId);
    const auto oldIndex = static_cast<std::size_t>(std::distance(entries.begin(), found));
    if (oldIndex == newIndex_)
        return OperationResult::noChange();
    const auto beforeIndex = newIndex_ + (oldIndex < newIndex_ ? 1U : 0U);
    const auto before =
        beforeIndex == entries.size() ? std::nullopt : std::optional(entries[beforeIndex].slotId);
    if (!stack->moveBefore(slotId_, before))
        return detail::brokenReference("Slot " + std::to_string(slotId_.value()) +
                                       " could not be moved to position " +
                                       std::to_string(newIndex_) + " on Merge node " +
                                       std::to_string(mergeId_.value()));
    return OperationResult::applied();
}

std::string_view DisconnectInput::typeId() const noexcept { return "bloom.node.disconnect-input"; }
OperationResult DisconnectInput::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::missingComposition(compositionId_);
    auto& graph = composition->graph();
    if (!graph.inputKind(input_, registry_))
        return detail::brokenReference("The " + detail::describeInput(input_) +
                                       " does not exist in composition " +
                                       std::to_string(compositionId_.value()));
    const auto reachableBefore = document::RenderReachability::of(*composition, registry_);
    std::set<document::NodeId> touched{detail::destinationNode(input_)};
    const auto neutral = [&](OperationResult result) {
        result.affectedTimes =
            detail::unreachableEditFootprint(*composition, *reachableBefore, touched, registry_);
        return result;
    };
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&input_); slot != nullptr) {
        auto* stack = graph.merge(slot->stackNodeId);
        if (!stack || stack->find(slot->slotId) == nullptr)
            return detail::brokenReference("The " + detail::describeInput(input_) +
                                           " does not exist in composition " +
                                           std::to_string(compositionId_.value()));
        // Detaching a stack slot's ONLY remaining edge removes the slot (task FIX1, item B): a slot
        // with nothing in it is not a shape the canonical graph admits, so "the slot" and "the link
        // into it" are one thing to the artist and one thing here. A slot now carries up to two
        // typed edges, content and audio (task FOLLOW-1) -- one Layer, one row, both roles -- so
        // removing just the audio link from an otherwise-fed Layer must leave its image row exactly
        // where it was; only a slot left with NEITHER edge is the shape this actually removes. The
        // Layer node keeps its boundary and its LayerId regardless, so reconnecting is one gesture
        // rather than a rebuild.
        const auto removedLayer = stack->find(slot->slotId)->layerId;
        const auto* edge = detail::inputEdge(graph, input_);
        if (edge != nullptr)
            (void)graph.eraseEdge(edge->id);
        const bool stillFed = std::ranges::any_of(graph.edges(), [&](const auto& candidate) {
            const auto* other = std::get_if<document::LayerStackInputRef>(&candidate.destination);
            return other != nullptr && other->stackNodeId == slot->stackNodeId &&
                   other->slotId == slot->slotId;
        });
        if (!stillFed && !stack->erase(slot->slotId))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Slot " + std::to_string(slot->slotId.value()) + " could not be removed from Merge node " +
                    std::to_string(slot->stackNodeId.value()));
        // A layer that leaves its Merge loses the track mattes it shared there (ADR 0028), as a
        // matte whose source node is deleted is cleared.
        if (!stillFed)
            graph.clearTrackMattesOutsideMerge(removedLayer);
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return neutral(OperationResult::applied());
    }
    // Unlinking an operand socket restores its parameter to the constant its registered default
    // names; see registeredDefault() for why that, and not a remembered previous value, is what an
    // explicit disconnect lands on.
    if (const auto* binding = detail::parameterSocketBinding(graph, input_)) {
        const auto* parameter = composition->parameters().find(binding->parameterId);
        if (parameter == nullptr)
            return detail::brokenReference(
                "The " + detail::describeInput(input_) + " is bound to parameter " +
                std::to_string(binding->parameterId.value()) + ", which composition " +
                std::to_string(compositionId_.value()) + " does not hold");
        if (!std::holds_alternative<document::DriverBindingSource>(parameter->source))
            return OperationResult::noChange();
        const auto* fixed = std::get_if<document::NodeInputRef>(&input_);
        // #543: unlinking a gradient stop removes its link record; the stop keeps the colour and
        // position it was authored with.
        if (fixed != nullptr && document::parseGradientStopPort(fixed->port)) {
            const auto parameterId = binding->parameterId;
            auto* node = graph.findNode(fixed->nodeId);
            std::erase_if(node->parameters, [&](const document::ParameterBinding& candidate) {
                return candidate.parameterId == parameterId;
            });
            static_cast<void>(composition->parameters().erase(parameterId));
            if (const auto failure = detail::validateGraph(*composition, registry_))
                return *failure;
            return neutral(OperationResult::applied());
        }
        auto fallback = fixed == nullptr ? std::nullopt
                                         : detail::registeredDefault(graph, fixed->nodeId,
                                                                     binding->role, registry_);
        if (!fallback.has_value())
            return OperationResult::rejected(
                OperationIssueCode::Unsupported,
                "The " + detail::describeInput(input_) +
                    " has no registered default to restore, so its link cannot be removed");
        if (!composition->parameters().setSource(
                binding->parameterId, document::ConstantValueSource{*std::move(fallback)}))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "The registered default for the " + detail::describeInput(input_) +
                    " violates the schema of parameter " +
                    std::to_string(binding->parameterId.value()));
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return neutral(OperationResult::applied());
    }
    const auto* edge = detail::inputEdge(graph, input_);
    if (!edge)
        return OperationResult::noChange();
    const auto upstream = edge->source.nodeId;
    const auto downstream = detail::destinationNode(input_);
    (void)graph.eraseEdge(edge->id);
    // A reroute with nothing on either side is a dot floating in the canvas that the artist never
    // placed: it existed only to bend a wire, and the wire is gone (task FIX1, item I). Removed in
    // the SAME transaction, so one undo puts the link and its bend back together. Both ends are
    // checked, because either of them may be the reroute this disconnect stranded.
    for (const auto candidate : {upstream, downstream})
        if (detail::eraseStrandedReroute(*composition, candidate))
            touched.insert(candidate);
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    return neutral(OperationResult::applied());
}

std::string_view RemapGradientStopLinks::typeId() const noexcept {
    return "bloom.node.remap-gradient-stop-links";
}
OperationResult RemapGradientStopLinks::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    auto* node = composition ? composition->graph().findNode(nodeId_) : nullptr;
    if (node == nullptr)
        return detail::missingNode(composition, compositionId_, nodeId_);
    const auto reachableBefore = document::RenderReachability::of(*composition, registry_);
    std::vector<document::ParameterBinding> kept;
    std::vector<document::ParameterId> removed;
    bool changed = false;
    for (auto binding : node->parameters) {
        auto stop = document::parseGradientStopPort(binding.role);
        if (!stop || stop->role != gradientRole_ || stop->stop >= newIndexOfOld_.size()) {
            kept.push_back(std::move(binding));
            continue;
        }
        const auto target = newIndexOfOld_[stop->stop];
        if (!target || *target >= document::kMaximumGradientStops) {
            removed.push_back(binding.parameterId);
            changed = true;
            continue;
        }
        if (*target != stop->stop) {
            binding.role = document::gradientStopPortName(gradientRole_, *target, stop->component);
            changed = true;
        }
        kept.push_back(std::move(binding));
    }
    if (!changed)
        return OperationResult::noChange();
    std::set<std::string_view> roles;
    for (const auto& binding : kept)
        if (!roles.insert(binding.role).second)
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Two stop links of gradient \"" + gradientRole_ + "\" on node " +
                    std::to_string(nodeId_.value()) + " would land on one stop");
    node->parameters = std::move(kept);
    for (const auto id : removed)
        static_cast<void>(composition->parameters().erase(id));
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    auto result = OperationResult::applied();
    result.affectedTimes =
        detail::unreachableEditFootprint(*composition, *reachableBefore, {nodeId_}, registry_);
    return result;
}

std::string_view DissolveNode::typeId() const noexcept { return "bloom.node.dissolve"; }
OperationResult DissolveNode::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::missingComposition(compositionId_);
    auto& graph = composition->graph();
    const auto* node = graph.findNode(nodeId_);
    if (!node)
        return detail::missingNode(composition, compositionId_, nodeId_);
    if (detail::protectedNode(graph, nodeId_))
        return OperationResult::rejected(
            OperationIssueCode::Unsupported,
            "Node " + std::to_string(nodeId_.value()) +
                " is the composition Output or the Merge connected to it and cannot be dissolved");
    const auto* definition = registry_.find(node->typeId, node->schemaVersion);
    if (!definition)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node " + std::to_string(nodeId_.value()) + " (" +
                                             node->typeId + ") has no registered socket schema");
    const auto input = std::ranges::find(definition->inputs, document::SocketValueKind::Image,
                                         &document::InputPortDefinition::valueKind);
    const auto output = std::ranges::find(definition->outputs, document::SocketValueKind::Image,
                                          &document::OutputPortDefinition::valueKind);
    if (input == definition->inputs.end() || output == definition->outputs.end())
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node " + std::to_string(nodeId_.value()) + " (" +
                                             node->typeId +
                                             ") has no Image input and Image output pair to dissolve through");
    const auto* incoming = detail::inputEdge(graph, document::NodeInputRef{nodeId_, input->name});
    if (!incoming)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "Node " + std::to_string(nodeId_.value()) +
                                             " has nothing connected to its \"" + input->name +
                                             "\" Image input, so it cannot be dissolved");
    const auto source = incoming->source;
    const bool layerNode = node->typeId == document::kLayerOutputNodeType;
    std::optional<document::OutputPortRef> audioSource;
    if (layerNode)
        if (const auto* audio = detail::inputEdge(
                graph, document::NodeInputRef{
                           nodeId_, std::string(document::kLayerOutputAudioInputPort)}))
            audioSource = audio->source;
    std::vector<document::EdgeRecord> consumers;
    // #502: a Layer node's Merge slots stay where they are and take its content (and audio)
    // source, so the entries keep their place; the reconcile pass gives each the Layer node's
    // values back as its own entry layer, and the picture does not change.
    std::vector<document::EdgeRecord> slotEdges;
    std::vector<document::EdgeId> releasedSlotEdges;
    for (auto edge : graph.edges()) {
        if (edge.source.nodeId != nodeId_)
            continue;
        if (std::holds_alternative<document::LayerStackInputRef>(edge.destination)) {
            // Any other node's stack slot is NOT reconnected: dissolving takes the node out.
            if (!layerNode)
                continue;
            releasedSlotEdges.push_back(edge.id);
            if (edge.source.port == output->name) {
                edge.source = source;
                slotEdges.push_back(std::move(edge));
            } else if (edge.source.port == document::kLayerOutputAudioOutputPort && audioSource) {
                edge.source = *audioSource;
                slotEdges.push_back(std::move(edge));
            }
            continue;
        }
        if (edge.source.port != output->name)
            continue;
        edge.source = source;
        consumers.push_back(std::move(edge));
    }
    std::set<document::ParameterId> candidates;
    for (const auto& binding : node->parameters)
        candidates.insert(binding.parameterId);
    // ADR 0028: the Layer node's masks own parameters outside its bindings.
    for (const auto& boundary : graph.layerOutputs())
        if (!boundary.entryOwned() && boundary.nodeId == nodeId_)
            for (const auto& mask : boundary.masks)
                for (const auto& [parameterId, schemaKey] : document::layerMaskParameters(mask)) {
                    (void)schemaKey;
                    candidates.insert(parameterId);
                }
    const auto reachableBefore = document::RenderReachability::of(*composition, registry_);
    const auto touched = removalTouchedNodes(*composition, {nodeId_});
    for (const auto id : releasedSlotEdges)
        (void)graph.eraseEdge(id);
    std::set<std::pair<document::NodeId, document::LayerSlotId>> released;
    for (const auto& edge : slotEdges) {
        const auto& slot = std::get<document::LayerStackInputRef>(edge.destination);
        released.emplace(slot.stackNodeId, slot.slotId);
    }
    for (auto& stack : graph.merges()) {
        for (const auto& entry : stack.entries()) {
            if (!released.contains({stack.nodeId(), entry.slotId}) || !entry.layerId.isValid())
                continue;
            if (!graph.merge(stack.nodeId())->setLayerId(entry.slotId, {}))
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "Dissolving node " + std::to_string(nodeId_.value()) +
                        " could not keep the layer of slot " + std::to_string(entry.slotId.value()) +
                        " on Merge node " + std::to_string(stack.nodeId().value()));
        }
    }
    // A slot the Layer fed only audio it has no source for leaves, as before.
    for (const auto& stack : composition->graph().merges()) {
        std::vector<document::LayerSlotId> emptied;
        for (const auto& entry : stack.entries())
            if (!released.contains({stack.nodeId(), entry.slotId}) &&
                std::ranges::none_of(graph.edges(), [&](const auto& candidate) {
                    const auto* input =
                        std::get_if<document::LayerStackInputRef>(&candidate.destination);
                    return input != nullptr && input->stackNodeId == stack.nodeId() &&
                           input->slotId == entry.slotId;
                }))
                emptied.push_back(entry.slotId);
        for (const auto slot : emptied)
            (void)graph.merge(stack.nodeId())->erase(slot);
    }
    (void)graph.eraseNode(nodeId_);
    composition->nodeLayout().erase(nodeId_);
    (void)detail::detachFromNodeGroups(*composition, {nodeId_});
    for (const auto& edge : consumers) {
        if (!graph.addEdge(edge, registry_))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Dissolving node " + std::to_string(nodeId_.value()) +
                    " could not reconnect the output of node " +
                    std::to_string(edge.source.nodeId.value()) + " to node " +
                    std::to_string(detail::destinationNode(edge.destination).value()));
    }
    for (const auto& edge : slotEdges) {
        if (!graph.addEdge(edge, registry_))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Dissolving node " + std::to_string(nodeId_.value()) +
                    " could not reconnect node " + std::to_string(edge.source.nodeId.value()) +
                    " to the " + detail::describeInput(edge.destination));
    }
    // A slot now fed straight by an upstream Layer node names that node's layer.
    for (const auto& [stackId, slotId] : released) {
        const auto fed = std::ranges::find_if(graph.edges(), [&](const auto& edge) {
            const auto* input = std::get_if<document::LayerStackInputRef>(&edge.destination);
            return input != nullptr && input->stackNodeId == stackId && input->slotId == slotId;
        });
        if (fed == graph.edges().end())
            continue;
        const auto upstream = std::ranges::find_if(graph.layerOutputs(), [&](const auto& layer) {
            return !layer.entryOwned() && layer.nodeId == fed->source.nodeId;
        });
        if (upstream == graph.layerOutputs().end())
            continue;
        if (!graph.merge(stackId)->setLayerId(slotId, upstream->layerId))
            return OperationResult::rejected(
                OperationIssueCode::Unsupported,
                "Dissolving node " + std::to_string(nodeId_.value()) +
                    " would give the upstream Layer node " + std::to_string(fed->source.nodeId.value()) +
                    " a second entry on Merge node " + std::to_string(stackId.value()));
    }
    detail::eraseOrphanedParameters(*composition, candidates);
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    auto result = OperationResult::applied();
    result.affectedTimes =
        detail::unreachableEditFootprint(*composition, *reachableBefore, touched, registry_);
    return result;
}
} // namespace bloom::commands

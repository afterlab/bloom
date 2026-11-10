#include <bloom/commands/entry_layer_operations.hpp>

#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/commands/node_operations.hpp>

#include "entry_layer_support.hpp"
#include "node_operation_support.hpp"

#include <bloom/document/evaluation_inputs.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/merge_entries.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bloom::commands {
namespace {

using document::CanonicalGraph;
using document::Composition;
using document::LayerId;
using document::LayerOutputBoundary;
using document::LayerSlotId;
using document::NodeId;
using document::ParameterId;

OperationResult missingEntry(const NodeId merge, const LayerSlotId slot) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Slot " + std::to_string(slot.value()) +
                                         " does not exist on Merge node " +
                                         std::to_string(merge.value()));
}

// The entry layer a slot owns by its own record, whether or not the slot still names it (a source
// replaced on a slot resets the slot's layer; reconcile puts it back or retires the layer).
const LayerOutputBoundary* ownedBoundary(const CanonicalGraph& graph, const NodeId merge,
                                         const LayerSlotId slot) {
    for (const auto& layer : graph.layerOutputs())
        if (layer.entryOwned() && layer.nodeId == merge && *layer.entrySlot == slot)
            return &layer;
    return nullptr;
}

// The Layer node feeding `edge` directly, if any.
const LayerOutputBoundary* directLayerNode(const CanonicalGraph& graph,
                                           const document::EdgeRecord* edge) {
    if (edge == nullptr)
        return nullptr;
    for (const auto& layer : graph.layerOutputs())
        if (!layer.entryOwned() && layer.nodeId == edge->source.nodeId)
            return &layer;
    return nullptr;
}

// A name for a new entry layer from its source's kind, when the source has no name of its own.
std::string sourceKindName(const CanonicalGraph& graph, const NodeId merge,
                           const LayerSlotId slot) {
    if (document::mergeEntryAdjustment(graph, merge, slot))
        return "Adjustment Layer";
    const auto* edge = document::mergeSlotImageEdge(graph, merge, slot);
    const auto* node = edge != nullptr ? graph.findNode(edge->source.nodeId) : nullptr;
    if (node != nullptr) {
        static const std::map<std::string_view, std::string_view> kNames{
            {document::kLayerStackNodeType, "Merge"},
            {document::kSolidSourceNodeType, "Solid"},
            {document::kTextSourceNodeType, "Text"},
            {document::kCheckerSourceNodeType, "Checker"},
            {document::kLinearGradientNodeType, "Gradient"},
            {document::kRadialGradientNodeType, "Gradient"},
            {document::kCompositionSourceNodeType, "Composition"},
            {document::kAdjustmentNodeType, "Adjustment Layer"},
            {"bloom.shape-source", "Shape"},
            {"bloom.image-source", "Image"},
            {"bloom.video-source", "Video"},
            {"bloom.vector-artwork", "Vector Artwork"},
        };
        if (const auto found = kNames.find(node->typeId); found != kNames.end())
            return std::string(found->second);
    }
    return "Layer " + std::to_string(graph.layerOutputs().size() + 1);
}

// Creates the entry layer of (merge, slot) at its defaults. The caller has checked the entry exists
// and has no layer yet.
OperationResult createEntryLayer(document::Draft& draft, const document::CompositionId compositionId,
                                 const NodeId merge, const LayerSlotId slot) {
    auto* composition = draft.project().findComposition(compositionId);
    const auto layerId = draft.ids().allocateLayer();
    if (!layerId)
        return detail::exhaustedIds("layer");
    LayerOutputBoundary boundary{merge, *layerId, {}, std::string(document::kLayerStackOutputPort)};
    boundary.entrySlot = slot;
    boundary.pivot = document::LayerPivot::Frame;
    std::vector<OperationOutput> outputs{{std::string(kEntryLayerOutput), *layerId}};
    for (const auto& role : document::entryLayerRoles()) {
        if (role.role == document::kTimeRemapParameterRole)
            continue; // ADR 0024: bound only while Time Remap is enabled.
        const auto value = detail::entryLayerDefaultValue(*composition, role.role);
        const auto parameterId = draft.ids().allocateParameter();
        if (!parameterId)
            return detail::exhaustedIds("parameter");
        if (!value || !composition->parameters().insert({*parameterId, std::string(role.schemaKey),
                                                          document::ConstantValueSource{*value}}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Entry layer default violates its parameter schema");
        boundary.parameters.push_back({std::string(role.role), *parameterId});
        outputs.push_back({"parameter." + std::string(role.role), *parameterId});
    }
    // Bindings are kept in their canonical (role) order, the order a saved document reopens in.
    std::ranges::sort(boundary.parameters, {}, &document::ParameterBinding::role);
    const auto& readable = std::as_const(*composition);
    if (auto name = document::layerSourceName(draft.project(), readable, boundary)) {
        boundary.name = std::move(*name);
        boundary.nameFollowsSource = true;
    } else {
        boundary.name = sourceKindName(readable.graph(), merge, slot);
    }
    auto& graph = composition->graph();
    if (!graph.addLayerOutput(std::move(boundary)) ||
        !graph.merge(merge)->setLayerId(slot, *layerId))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Entry layer could not be inserted");
    return OperationResult::applied(std::move(outputs));
}

std::optional<LayerId> outputLayer(const OperationResult& result) {
    for (const auto& output : result.outputs)
        if (output.name == kEntryLayerOutput)
            if (const auto* id = std::get_if<LayerId>(&output.id))
                return *id;
    return std::nullopt;
}

template <typename Id>
std::optional<Id> outputId(const OperationResult& result, const std::string_view name) {
    for (const auto& output : result.outputs)
        if (output.name == name)
            if (const auto* id = std::get_if<Id>(&output.id))
                return *id;
    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// EnsureEntryLayer

OperationResult EnsureEntryLayer::apply(document::Draft& draft) const {
    const auto* composition = std::as_const(draft).project().findComposition(composition_);
    if (composition == nullptr)
        return detail::missingComposition(composition_);
    const auto& graph = composition->graph();
    const auto* stack = graph.merge(merge_);
    if (stack == nullptr || stack->find(slot_) == nullptr)
        return missingEntry(merge_, slot_);
    if (const auto* layer = document::mergeEntryLayer(graph, merge_, slot_))
        return OperationResult::noChange({{std::string(kEntryLayerOutput), layer->layerId}});
    return createEntryLayer(draft, composition_, merge_, slot_);
}

// ---------------------------------------------------------------------------------------------
// EntryLayerEdit

std::string_view EntryLayerEdit::typeId() const noexcept { return "bloom.merge.entry-layer-edit"; }

OperationResult EntryLayerEdit::apply(document::Draft& draft) const {
    built_.reset();
    created_ = false;
    if (!build_)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Entry layer edit has no operation to apply");
    auto ensured = EnsureEntryLayer(composition_, merge_, slot_).apply(draft);
    if (ensured.status == OperationStatus::Rejected)
        return ensured;
    const auto layer = outputLayer(ensured);
    if (!layer)
        return detail::brokenReference("Slot " + std::to_string(slot_.value()) + " of Merge node " +
                                       std::to_string(merge_.value()) +
                                       " did not report an entry layer after it was ensured");
    std::shared_ptr<Operation> operation = build_(*layer);
    if (!operation)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Entry layer edit built no operation");
    auto edited = operation->apply(draft);
    if (edited.status == OperationStatus::Rejected)
        return edited;
    created_ = ensured.status == OperationStatus::Applied;
    built_ = std::move(operation);
    auto outputs = std::move(ensured.outputs);
    outputs.insert(outputs.end(), std::make_move_iterator(edited.outputs.begin()),
                   std::make_move_iterator(edited.outputs.end()));
    edited.outputs = std::move(outputs);
    if (edited.status != OperationStatus::Applied)
        return edited;
    if (created_) {
        // Creating the layer is a topology change: publish the whole-render default.
        edited.affectedTimes.reset();
        edited.layerIdentityRemaps.reset();
    }
    return edited;
}

bool EntryLayerEdit::renderAffecting() const noexcept {
    return created_ || built_ == nullptr || built_->renderAffecting();
}

std::optional<document::CompositionId> EntryLayerEdit::parameterEditScope() const noexcept {
    if (created_ || built_ == nullptr)
        return std::nullopt;
    return built_->parameterEditScope();
}

// ---------------------------------------------------------------------------------------------
// InsertLayerNode

OperationResult InsertLayerNode::apply(document::Draft& draft) const {
    const auto* readable = std::as_const(draft).project().findComposition(composition_);
    if (readable == nullptr)
        return detail::missingComposition(composition_);
    const auto* stack = readable->graph().merge(merge_);
    if (stack == nullptr || stack->find(slot_) == nullptr)
        return missingEntry(merge_, slot_);
    const auto* edge = document::mergeSlotImageEdge(readable->graph(), merge_, slot_);
    if (edge == nullptr)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "An entry without an image has no Layer node to insert");
    if (const auto* layer = document::mergeEntryLayer(readable->graph(), merge_, slot_);
        layer != nullptr && layer->locked)
        return detail::lockedLayer(*layer);
    const auto edgeId = edge->id;
    const auto source = edge->source;
    const auto destination = edge->destination;
    const auto sourcePosition = effectiveNodePosition(*readable, source.nodeId);
    const auto sourceSize = estimatedNodeCardSize(*readable, source.nodeId);

    auto added = AddNode(composition_, std::string(document::kLayerOutputNodeType),
                         {sourcePosition.x + sourceSize.width + kNodeLayoutGap, sourcePosition.y})
                     .apply(draft);
    if (added.status != OperationStatus::Applied)
        return added;
    const auto node = outputId<NodeId>(added, kAddNodeOutput);
    const auto layer = outputId<LayerId>(added, "layer");
    const auto inputEdge = draft.ids().allocateEdge();
    if (!node || !layer)
        return detail::brokenReference("Adding a Layer node to composition " +
                                       std::to_string(composition_.value()) +
                                       " did not report the new node and layer"
                                       "; the entry layer cannot be converted");
    if (!inputEdge)
        return detail::exhaustedIds("edge");
    auto* composition = draft.project().findComposition(composition_);
    auto& graph = composition->graph();
    (void)graph.eraseEdge(edgeId);
    if (!graph.addEdge({*inputEdge, source,
                        document::NodeInputRef{
                            *node, std::string(document::kLayerOutputContentInputPort)}}) ||
        !graph.addEdge({edgeId, {*node, std::string(document::kLayerOutputOutputPort)},
                        destination}) ||
        !graph.merge(merge_)->setLayerId(slot_, *layer))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The Layer node could not be inserted into the entry");
    // A new Layer node shows its source's name, as a layer made from a source does. When the entry
    // has its own layer, the reconcile pass gives the node that layer's name and values instead.
    if (auto* boundary = graph.findLayer(*layer))
        if (auto name = document::layerSourceName(draft.project(), std::as_const(*composition),
                                                  *boundary)) {
            boundary->name = std::move(*name);
            boundary->nameFollowsSource = true;
        }
    auto result = OperationResult::applied({{std::string(kEntryLayerNodeOutput), *node},
                                            {std::string(kEntryLayerOutput), *layer},
                                            {"edge", *inputEdge}});
    for (auto& output : added.outputs)
        if (output.name.starts_with("parameter."))
            result.outputs.push_back(std::move(output));
    return result;
}

// ---------------------------------------------------------------------------------------------
// AddAdjustmentLayer

OperationResult AddAdjustmentLayer::apply(document::Draft& draft) const {
    const auto* readable = std::as_const(draft).project().findComposition(composition_);
    if (readable == nullptr)
        return detail::missingComposition(composition_);
    const auto* stack = readable->graph().merge(merge_);
    if (stack == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Merge node " + std::to_string(merge_.value()) +
                                             " does not exist in composition " +
                                             std::to_string(composition_.value()));
    if (above_ && stack->find(*above_) == nullptr)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "The entry to add the Adjustment Layer above does not "
                                         "exist in the Merge");
    const auto before = above_ ? above_
                        : stack->entries().empty()
                            ? std::nullopt
                            : std::optional(stack->entries().front().slotId);
    const auto mergePosition = effectiveNodePosition(*readable, merge_);
    const auto existing = static_cast<std::size_t>(
        std::ranges::count(readable->graph().nodes(), document::kAdjustmentNodeType,
                           &document::NodeRecord::typeId));
    auto added = AddNode(composition_, std::string(document::kAdjustmentNodeType),
                         {mergePosition.x - 2.0 * kNodeLayoutGap - 128.0,
                          mergePosition.y - 2.0 * kNodeLayoutGap})
                     .apply(draft);
    if (added.status != OperationStatus::Applied)
        return added;
    const auto node = outputId<NodeId>(added, kAddNodeOutput);
    if (!node)
        return detail::brokenReference("Adding an Adjustment node to composition " +
                                       std::to_string(composition_.value()) +
                                       " did not report the new node");
    auto connected =
        ConnectPorts(composition_, {*node, std::string(document::kAdjustmentOutputPort)},
                     document::LayerStackInputRef{
                         merge_, {}, std::string(document::kLayerStackContentInputRole)},
                     document::builtInNodeDefinitions(), before)
            .apply(draft);
    if (connected.status != OperationStatus::Applied)
        return connected;
    const auto slot = outputId<LayerSlotId>(connected, kConnectPortsSlotOutput);
    if (!slot)
        return detail::brokenReference("Connecting the new Adjustment node to Merge node " +
                                       std::to_string(merge_.value()) +
                                       " did not report the new slot");
    auto ensured = EnsureEntryLayer(composition_, merge_, *slot).apply(draft);
    if (ensured.status != OperationStatus::Applied)
        return ensured.status == OperationStatus::Rejected ? ensured : missingEntry(merge_, *slot);
    const auto layer = outputLayer(ensured);
    auto* composition = draft.project().findComposition(composition_);
    auto* boundary = layer ? composition->graph().findLayer(*layer) : nullptr;
    if (boundary == nullptr)
        return detail::brokenReference("The entry layer of slot " + std::to_string(slot->value()) +
                                       " on Merge node " + std::to_string(merge_.value()) +
                                       " was not found after it was ensured");
    boundary->name = "Adjustment Layer " + std::to_string(existing + 1);
    boundary->nameFollowsSource = false;
    std::vector<OperationOutput> outputs{{std::string(kEntryLayerNodeOutput), *node},
                                         {std::string(kEntryLayerSlotOutput), *slot},
                                         {std::string(kEntryLayerOutput), *layer}};
    for (auto& output : ensured.outputs)
        if (output.name.starts_with("parameter."))
            outputs.push_back(std::move(output));
    return OperationResult::applied(std::move(outputs));
}

// ---------------------------------------------------------------------------------------------
// Entry-layer copies and deletion, for the layer commands

namespace detail {

std::optional<OperationResult> duplicateEntryLayer(document::Draft& draft,
                                                   const document::CompositionId compositionId,
                                                   const LayerId original, EntryLayerCopy& copy,
                                                   std::vector<OperationOutput>& outputs) {
    auto* composition = draft.project().findComposition(compositionId);
    const auto* readable = composition ? &std::as_const(*composition) : nullptr;
    const auto* layer = readable ? readable->graph().findLayer(original) : nullptr;
    if (layer == nullptr || !layer->entryOwned())
        return detail::brokenReference("Layer " + std::to_string(original.value()) +
                                       (layer == nullptr ? " does not exist in composition "
                                                         : " is not an entry layer in composition ") +
                                       std::to_string(compositionId.value()));
    auto boundary = *layer;
    const auto merge = boundary.nodeId;
    const auto slot = *boundary.entrySlot;
    const auto* stack = readable->graph().merge(merge);
    if (stack == nullptr || stack->find(slot) == nullptr)
        return missingEntry(merge, slot);
    const auto entries = stack->entries();
    const auto found = std::ranges::find(entries, slot, &document::LayerStackEntry::slotId);
    const auto below = std::next(found) == entries.end() ? std::nullopt
                                                         : std::optional(std::next(found)->slotId);
    std::vector<document::EdgeRecord> edges;
    for (const auto& edge : readable->graph().edges())
        if (const auto* input = std::get_if<document::LayerStackInputRef>(&edge.destination);
            input != nullptr && input->stackNodeId == merge && input->slotId == slot)
            edges.push_back(edge);
    const auto layerId = draft.ids().allocateLayer();
    const auto slotId = draft.ids().allocateLayerSlot();
    if (!layerId || !slotId)
        return detail::exhaustedIds("layer and layer slot");
    boundary.layerId = *layerId;
    boundary.entrySlot = *slotId;
    boundary.name += " copy";
    for (auto& binding : boundary.parameters) {
        const auto id =
            detail::copyParameterFrom(draft.ids(), *readable, *composition, binding.parameterId);
        if (!id)
            return detail::exhaustedIds("layer, layer slot and parameter");
        outputs.push_back({"parameter." + std::to_string(binding.parameterId.value()), *id});
        binding.parameterId = *id;
    }
    for (auto& mask : boundary.masks) {
        const auto maskId = draft.ids().allocateLayerMask();
        if (!maskId)
            return detail::exhaustedIds("parameter and layer mask");
        mask.id = *maskId;
        for (auto* parameter : {&mask.path, &mask.feather, &mask.expansion, &mask.opacity}) {
            const auto id =
                detail::copyParameterFrom(draft.ids(), *readable, *composition, *parameter);
            if (!id)
                return detail::exhaustedIds("layer mask and parameter");
            *parameter = *id;
        }
    }
    auto& graph = composition->graph();
    auto& target = *graph.merge(merge);
    if (!target.append({*slotId, *layerId}) || !target.moveBefore(*slotId, below))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The entry copy could not take a slot");
    for (auto edge : edges) {
        const auto edgeId = draft.ids().allocateEdge();
        if (!edgeId)
            return detail::exhaustedIds("edge");
        edge.id = *edgeId;
        std::get<document::LayerStackInputRef>(edge.destination).slotId = *slotId;
        if (!graph.addEdge(std::move(edge)))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "The entry copy could not take its sources");
    }
    if (!graph.addLayerOutput(std::move(boundary)))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The entry layer copy could not be inserted");
    outputs.push_back({"layer." + std::to_string(original.value()), *layerId});
    outputs.push_back({"slot." + std::to_string(slot.value()), *slotId});
    copy = {*layerId, *slotId};
    return std::nullopt;
}

std::optional<OperationResult> deleteEntryLayer(Composition& composition, const LayerId layerId) {
    const auto* layer = std::as_const(composition).graph().findLayer(layerId);
    if (layer == nullptr || !layer->entryOwned())
        return detail::brokenReference("Layer " + std::to_string(layerId.value()) +
                                       (layer == nullptr ? " does not exist"
                                                         : " is not an entry layer") +
                                       " in composition " +
                                       std::to_string(composition.id().value()));
    const auto boundary = *layer;
    auto& graph = composition.graph();
    std::vector<document::EdgeId> edges;
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<document::LayerStackInputRef>(&edge.destination);
            input != nullptr && input->stackNodeId == boundary.nodeId &&
            input->slotId == *boundary.entrySlot)
            edges.push_back(edge.id);
    for (const auto id : edges)
        (void)graph.eraseEdge(id);
    if (auto* stack = graph.merge(boundary.nodeId))
        (void)stack->erase(*boundary.entrySlot);
    (void)graph.eraseLayerOutput(layerId);
    std::vector<LayerId> referring;
    for (const auto& other : std::as_const(graph).layerOutputs())
        if (other.parent == layerId || (other.trackMatte && other.trackMatte->source == layerId))
            referring.push_back(other.layerId);
    for (const auto id : referring) {
        auto& other = *graph.findLayer(id);
        if (other.parent == layerId)
            other.parent.reset();
        if (other.trackMatte && other.trackMatte->source == layerId)
            other.trackMatte.reset();
    }
    std::set<ParameterId> candidates;
    for (const auto& binding : boundary.parameters)
        candidates.insert(binding.parameterId);
    for (const auto& mask : boundary.masks)
        for (const auto id : {mask.path, mask.feather, mask.expansion, mask.opacity})
            candidates.insert(id);
    detail::eraseOrphanedParameters(composition, candidates);
    return std::nullopt;
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// The reconcile pass

namespace {

// Whether anything the entry/layer relation reads changed: node identities and kinds, edges, Merge
// slots and the layer boundaries' identities. A parameter, curve, flag or layout edit is none.
bool topologyChanged(const CanonicalGraph& before, const CanonicalGraph& after) {
    const auto sameNodes = std::ranges::equal(
        before.nodes(), after.nodes(), [](const auto& a, const auto& b) {
            return a.id == b.id && a.typeId == b.typeId && a.schemaVersion == b.schemaVersion;
        });
    if (!sameNodes || !std::ranges::equal(before.edges(), after.edges()))
        return true;
    if (!std::ranges::equal(before.merges(), after.merges(), [](const auto& a, const auto& b) {
            return a.nodeId() == b.nodeId() && std::ranges::equal(a.entries(), b.entries());
        }))
        return true;
    return !std::ranges::equal(
        before.layerOutputs(), after.layerOutputs(), [](const auto& a, const auto& b) {
            return a.layerId == b.layerId && a.nodeId == b.nodeId && a.entrySlot == b.entrySlot;
        });
}

class CompositionReconcile final {
  public:
    CompositionReconcile(const Composition& before, Composition& after, document::IdAllocator& ids,
                         EntryLayerReconcileWork& work)
        : before_(before), after_(after), ids_(ids), work_(work) {}

    [[nodiscard]] std::optional<OperationIssue> run() {
        removeStrandedEntryLayers();
        // The entries to examine, copied: the passes below edit the Merge records.
        std::vector<document::MergeEntryRef> entries;
        for (const auto& stack : graph().merges())
            for (const auto& entry : stack.entries())
                entries.push_back({stack.nodeId(), entry.slotId});
        for (const auto& entry : entries) {
            ++work_.entriesExamined;
            if (auto failure = reconcileEntry(entry))
                return failure;
        }
        cleanUpRemovedLayers();
        return std::nullopt;
    }

    [[nodiscard]] bool changed() const noexcept { return changed_; }

  private:
    [[nodiscard]] CanonicalGraph& graph() { return after_.graph(); }
    [[nodiscard]] const CanonicalGraph& readGraph() const {
        return std::as_const(after_).graph();
    }

    static OperationIssue exhausted(const std::string_view kinds) {
        return {OperationIssueCode::Unsupported,
                "The " + std::string(kinds) +
                    " ID space is exhausted; the project cannot create another one"};
    }

    void retire(const LayerOutputBoundary& layer, const bool keepParameters) {
        if (!keepParameters) {
            for (const auto& binding : layer.parameters)
                candidates_.insert(binding.parameterId);
            for (const auto& mask : layer.masks)
                for (const auto id : {mask.path, mask.feather, mask.expansion, mask.opacity})
                    candidates_.insert(id);
        }
        retiredLayers_.insert(layer.layerId);
        (void)graph().eraseLayerOutput(layer.layerId);
        changed_ = true;
    }

    // Entry layers whose Merge or slot no longer exists.
    void removeStrandedEntryLayers() {
        std::vector<LayerOutputBoundary> stranded;
        for (const auto& layer : readGraph().layerOutputs()) {
            if (!layer.entryOwned())
                continue;
            const auto* stack = readGraph().merge(layer.nodeId);
            if (stack == nullptr || stack->find(*layer.entrySlot) == nullptr)
                stranded.push_back(layer);
        }
        for (const auto& layer : stranded) {
            retire(layer, false);
            ++work_.removed;
        }
    }

    // Every reference other layers hold to `from` now names `to` (or clears when `to` is none or
    // would name the referring layer itself).
    void repoint(const LayerId from, const std::optional<LayerId> to) {
        std::vector<LayerId> referring;
        for (const auto& layer : readGraph().layerOutputs())
            if (layer.parent == from || (layer.trackMatte && layer.trackMatte->source == from))
                referring.push_back(layer.layerId);
        for (const auto id : referring) {
            auto& layer = *graph().findLayer(id);
            if (layer.parent == from) {
                layer.parent = to && *to != layer.layerId ? to : std::nullopt;
                changed_ = true;
            }
            if (layer.trackMatte && layer.trackMatte->source == from) {
                if (to && *to != layer.layerId)
                    layer.trackMatte->source = *to;
                else
                    layer.trackMatte.reset();
                changed_ = true;
            }
        }
    }

    // Whether `from`'s output reaches node `to` along the graph's edges.
    [[nodiscard]] bool feeds(const NodeId from, const NodeId to) const {
        std::set<NodeId> seen{from};
        std::vector<NodeId> pending{from};
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            if (node == to)
                return true;
            for (const auto& edge : readGraph().edges())
                if (edge.source.nodeId == node) {
                    const auto next = detail::destinationNode(edge.destination);
                    if (seen.insert(next).second)
                        pending.push_back(next);
                }
        }
        return false;
    }

    [[nodiscard]] bool isDefaultLayerNode(const LayerOutputBoundary& layer) const {
        if (!layer.enabled || layer.solo || layer.inPoint != core::RationalTime{} ||
            layer.outPoint != core::RationalTime{} || layer.parent || !layer.masks.empty() ||
            layer.trackMatte || layer.pivot != document::LayerPivot::Content ||
            !(layer.timing == document::LayerTiming{}) ||
            document::nodeMuted(after_.nodeLayout(), layer.nodeId))
            return false;
        const auto* node = readGraph().findNode(layer.nodeId);
        if (node == nullptr)
            return false;
        for (const auto& binding : node->parameters) {
            if (binding.role == document::kTimeRemapParameterRole)
                return false;
            const auto expected = detail::entryLayerDefaultValue(after_, binding.role);
            const auto* record = after_.parameters().find(binding.parameterId);
            const auto* constant =
                record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
            // Position: a Layer node's own default is the composition centre too (AddNode).
            if (!expected || constant == nullptr || !(constant->value == *expected))
                return false;
        }
        return true;
    }

    // Moves the entry layer `entry`'s values into Layer node `layer`.
    [[nodiscard]] std::optional<OperationIssue> adopt(const LayerOutputBoundary& entry,
                                                      const LayerId layerId) {
        auto* target = graph().findLayer(layerId);
        auto* node = target ? graph().findNode(target->nodeId) : nullptr;
        if (node == nullptr)
            return OperationIssue{OperationIssueCode::InvalidTarget,
                                  "The Layer node of layer " + std::to_string(layerId.value()) +
                                      ", which adopts the entry layer, does not exist"};
        auto& parameters = after_.parameters();
        std::set<ParameterId> moved;
        for (const auto& binding : entry.parameters) {
            const auto existing =
                std::ranges::find(node->parameters, binding.role, &document::ParameterBinding::role);
            if (existing == node->parameters.end()) {
                node->parameters.push_back(binding); // ownership moves with the binding
                moved.insert(binding.parameterId);
                continue;
            }
            const auto* from = parameters.find(binding.parameterId);
            const auto* into = parameters.find(existing->parameterId);
            if (from == nullptr || into == nullptr)
                continue;
            std::optional<document::AnimationCurveId> retired;
            if (const auto* animated = std::get_if<document::AnimationCurveSource>(&into->source))
                retired = animated->curveId;
            const auto source = from->source;
            (void)parameters.erase(binding.parameterId);
            moved.insert(binding.parameterId);
            if (!parameters.setSource(existing->parameterId, source))
                return OperationIssue{OperationIssueCode::InvalidValue,
                                      "The Layer node could not adopt the entry's values"};
            if (retired)
                (void)after_.animationCurves().erase(*retired);
        }
        // A role the entry layer does not bind (Time Remap off) is off on the Layer node too.
        std::erase_if(node->parameters, [&](const document::ParameterBinding& binding) {
            const bool entryRole = std::ranges::any_of(
                document::entryLayerRoles(), [&](const auto& role) { return role.role == binding.role; });
            const bool bound = std::ranges::any_of(
                entry.parameters, [&](const auto& own) { return own.role == binding.role; });
            if (entryRole && !bound) {
                candidates_.insert(binding.parameterId);
                return true;
            }
            return false;
        });
        target->name = entry.name;
        target->labelColor = entry.labelColor;
        target->enabled = entry.enabled;
        target->solo = entry.solo;
        target->locked = entry.locked;
        target->inPoint = entry.inPoint;
        target->outPoint = entry.outPoint;
        target->parent = entry.parent == layerId ? std::nullopt : entry.parent;
        target->timing = entry.timing;
        target->masks = entry.masks; // the masks and their parameters move with the layer
        target->trackMatte =
            entry.trackMatte && entry.trackMatte->source == layerId ? std::nullopt : entry.trackMatte;
        target->shy = entry.shy;
        target->nameFollowsSource = entry.nameFollowsSource;
        target->pivot = entry.pivot;
        auto layout = detail::layoutFor(after_, node->id);
        if (layout.muted == entry.enabled) {
            layout.muted = !entry.enabled;
            after_.nodeLayout()[node->id] = layout;
        }
        // The entry layer leaves; its parameters were moved or consumed above.
        auto retiredEntry = entry;
        retiredEntry.masks.clear();
        std::erase_if(retiredEntry.parameters,
                      [&](const auto& binding) { return moved.contains(binding.parameterId); });
        retire(retiredEntry, false);
        return std::nullopt;
    }

    // A new entry layer for (merge, slot) holding a copy of Layer node `layer`'s values, read from
    // `source` (the composition that still has the Layer node).
    [[nodiscard]] std::optional<OperationIssue>
    giveBack(const Composition& source, const LayerOutputBoundary layer,
             const document::MergeEntryRef& entry) {
        const auto* node = source.graph().findNode(layer.nodeId);
        if (node == nullptr)
            return std::nullopt;
        const auto layerId = ids_.allocateLayer();
        if (!layerId)
            return exhausted("layer");
        auto copy = layer;
        copy.nodeId = entry.merge;
        copy.layerId = *layerId;
        copy.outputPort = std::string(document::kLayerStackOutputPort);
        copy.entrySlot = entry.slot;
        copy.parameters.clear();
        copy.masks.clear();
        for (const auto& role : document::entryLayerRoles()) {
            const auto binding =
                std::ranges::find(node->parameters, role.role, &document::ParameterBinding::role);
            const auto fallback = detail::entryLayerDefaultValue(after_, role.role);
            std::optional<ParameterId> id;
            if (binding != node->parameters.end()) {
                id = detail::copyParameterFrom(ids_, source, after_, binding->parameterId,
                                               fallback);
            } else if (role.role != document::kTimeRemapParameterRole && fallback) {
                id = ids_.allocateParameter();
                if (id && !after_.parameters().insert({*id, std::string(role.schemaKey),
                                                       document::ConstantValueSource{*fallback}}))
                    id.reset();
            } else {
                continue;
            }
            if (!id)
                return exhausted("parameter");
            copy.parameters.push_back({std::string(role.role), *id});
        }
        std::ranges::sort(copy.parameters, {}, &document::ParameterBinding::role);
        for (auto mask : layer.masks) {
            const auto maskId = ids_.allocateLayerMask();
            if (!maskId)
                return exhausted("layer mask");
            mask.id = *maskId;
            for (auto* parameter : {&mask.path, &mask.feather, &mask.expansion, &mask.opacity}) {
                const auto id = detail::copyParameterFrom(ids_, source, after_, *parameter);
                if (!id)
                    return exhausted("layer mask and parameter");
                *parameter = *id;
            }
            copy.masks.push_back(std::move(mask));
        }
        if (copy.parent && (*copy.parent == layer.layerId || !readGraph().findLayer(*copy.parent)))
            copy.parent.reset();
        if (copy.trackMatte && (copy.trackMatte->source == layer.layerId ||
                                !readGraph().findLayer(copy.trackMatte->source)))
            copy.trackMatte.reset();
        if (const auto* extent = std::get_if<document::SourceLayerExtent>(&copy.timing.contentExtent);
            extent != nullptr && readGraph().findNode(extent->sourceNodeId) == nullptr)
            copy.timing.contentExtent = {};
        if (!graph().addLayerOutput(std::move(copy)) ||
            !graph().merge(entry.merge)->setLayerId(entry.slot, *layerId))
            return OperationIssue{OperationIssueCode::InvalidValue,
                                  "The entry could not take back its layer values"};
        changed_ = true;
        ++work_.returned;
        // References to a deleted Layer node follow its values to the first entry that took them,
        // where an entry layer can be referred to: by the Merge's own entry layers, or by a layer
        // that does not feed the Merge (a parent and a matte evaluate before what they serve, and
        // an entry layer evaluates with its Merge).
        if (readGraph().findLayer(layer.layerId) == nullptr &&
            returnedTo_.emplace(layer.layerId, *layerId).second) {
            for (const auto& previous : before_.graph().layerOutputs()) {
                auto* current = graph().findLayer(previous.layerId);
                if (current == nullptr || current->layerId == *layerId)
                    continue;
                if (!(current->entryOwned() && current->nodeId == entry.merge) &&
                    feeds(current->nodeId, entry.merge))
                    continue;
                if (previous.parent == layer.layerId && !current->parent)
                    current->parent = *layerId;
                if (previous.trackMatte && previous.trackMatte->source == layer.layerId &&
                    !current->trackMatte) {
                    current->trackMatte = previous.trackMatte;
                    current->trackMatte->source = *layerId;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<OperationIssue> reconcileEntry(const document::MergeEntryRef& entry) {
        const auto* edge = document::mergeSlotImageEdge(readGraph(), entry.merge, entry.slot);
        const auto* chain = edge ? document::chainLayer(readGraph(), edge->source) : nullptr;
        const auto* own = ownedBoundary(readGraph(), entry.merge, entry.slot);
        const auto* slot = readGraph().merge(entry.merge)->find(entry.slot);
        if (chain != nullptr) {
            const auto chainId = chain->layerId;
            if (own != nullptr) {
                const auto ownCopy = *own;
                const bool createdNow = before_.graph().findNode(chain->nodeId) == nullptr;
                const bool adoptable =
                    !adopted_.contains(chainId) &&
                    (createdNow ||
                     (isDefaultLayerNode(*chain) &&
                      document::mergeEntriesOfLayer(before_.graph(), chainId).empty()));
                if (adoptable) {
                    adopted_.insert(chainId);
                    if (auto failure = adopt(ownCopy, chainId))
                        return failure;
                    ++work_.adopted;
                } else {
                    retire(ownCopy, false);
                    ++work_.replaced;
                }
                repoint(ownCopy.layerId, chainId);
            }
            // The slot names its direct Layer node, or nothing when the Layer node is further up.
            const auto* direct = directLayerNode(readGraph(),
                                                 document::mergeSlotImageEdge(readGraph(),
                                                                              entry.merge,
                                                                              entry.slot));
            const auto expected = direct ? direct->layerId : LayerId{};
            if (readGraph().merge(entry.merge)->find(entry.slot)->layerId != expected) {
                if (!graph().merge(entry.merge)->setLayerId(entry.slot, expected))
                    return OperationIssue{OperationIssueCode::InvalidValue,
                                          "The Merge slot could not name its Layer node"};
                changed_ = true;
            }
            return std::nullopt;
        }
        if (own != nullptr) {
            if (slot->layerId != own->layerId) {
                if (!graph().merge(entry.merge)->setLayerId(entry.slot, own->layerId))
                    return OperationIssue{OperationIssueCode::InvalidValue,
                                          "The Merge slot could not name its entry layer"};
                changed_ = true;
            }
            return std::nullopt;
        }
        if (edge == nullptr)
            return std::nullopt;
        // No layer now. Did a Layer node leave this entry?
        const auto* previousStack = before_.graph().merge(entry.merge);
        if (previousStack == nullptr || previousStack->find(entry.slot) == nullptr)
            return std::nullopt;
        const auto* previous = document::mergeEntryLayer(before_.graph(), entry.merge, entry.slot);
        if (previous == nullptr || previous->entryOwned())
            return std::nullopt;
        const auto* current = readGraph().findLayer(previous->layerId);
        const bool stillHere =
            current != nullptr && !current->entryOwned() && readGraph().findNode(current->nodeId);
        // A slot that still names the departed layer stops naming it before the copy takes over.
        if (slot->layerId.isValid() && !graph().merge(entry.merge)->setLayerId(entry.slot, {}))
            return OperationIssue{OperationIssueCode::InvalidValue,
                                  "The Merge slot could not release its layer"};
        return stillHere ? giveBack(after_, *current, entry)
                         : giveBack(before_, *previous, entry);
    }

    void cleanUpRemovedLayers() {
        for (const auto& previous : before_.graph().layerOutputs()) {
            if (readGraph().findLayer(previous.layerId) != nullptr)
                continue;
            for (const auto& mask : previous.masks)
                for (const auto id : {mask.path, mask.feather, mask.expansion, mask.opacity})
                    candidates_.insert(id);
            for (const auto& binding : previous.parameters)
                candidates_.insert(binding.parameterId);
            if (!returnedTo_.contains(previous.layerId))
                retiredLayers_.insert(previous.layerId);
        }
        for (const auto id : retiredLayers_)
            if (readGraph().findLayer(id) == nullptr && !returnedTo_.contains(id)) {
                bool referenced = false;
                for (const auto& layer : readGraph().layerOutputs())
                    referenced = referenced || layer.parent == id ||
                                 (layer.trackMatte && layer.trackMatte->source == id);
                if (referenced)
                    repoint(id, std::nullopt);
            }
        for (const auto id : candidates_) {
            if (detail::parameterReferenced(readGraph(), id))
                continue;
            const auto* record = after_.parameters().find(id);
            if (record == nullptr)
                continue;
            std::optional<document::AnimationCurveId> curve;
            if (const auto* animated = std::get_if<document::AnimationCurveSource>(&record->source))
                curve = animated->curveId;
            (void)after_.parameters().erase(id);
            changed_ = true;
            if (curve && std::ranges::none_of(after_.parameters().records(), [&](const auto& other) {
                    const auto* source = std::get_if<document::AnimationCurveSource>(&other.source);
                    return source != nullptr && source->curveId == *curve;
                }))
                (void)after_.animationCurves().erase(*curve);
        }
    }

    const Composition& before_;
    Composition& after_;
    document::IdAllocator& ids_;
    EntryLayerReconcileWork& work_;
    std::set<ParameterId> candidates_;
    std::set<LayerId> adopted_;
    std::set<LayerId> retiredLayers_;
    std::map<LayerId, LayerId> returnedTo_;
    bool changed_ = false;
};

} // namespace

EntryLayerReconcileResult reconcileEntryLayers(const document::Project& before,
                                               document::Draft& draft,
                                               EntryLayerReconcileWork* work) {
    EntryLayerReconcileWork local;
    auto& counters = work != nullptr ? *work : local;
    EntryLayerReconcileResult result;
    std::vector<document::CompositionId> touched;
    for (const auto& composition : std::as_const(draft).project().compositions()) {
        const auto* previous = before.findComposition(composition.id());
        if (previous == nullptr || previous->contentIdentity() == composition.contentIdentity())
            continue;
        if (!topologyChanged(previous->graph(), composition.graph())) {
            ++counters.compositionsSkipped;
            continue;
        }
        touched.push_back(composition.id());
    }
    for (const auto id : touched) {
        ++counters.compositionsReconciled;
        CompositionReconcile pass(*before.findComposition(id), *draft.project().findComposition(id),
                                  draft.ids(), counters);
        if (auto failure = pass.run()) {
            result.failure = std::move(failure);
            return result;
        }
        result.changed = result.changed || pass.changed();
    }
    return result;
}

} // namespace bloom::commands

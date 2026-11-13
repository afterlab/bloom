#include <bloom/commands/layer_stack_operations.hpp>

#include <bloom/commands/node_operations.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/project.hpp>

#include "entry_layer_support.hpp"
#include "node_operation_support.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::commands {
namespace {

// The Layer Output boundary node of every layer, or the rejection that names the first one the
// composition does not hold.
[[nodiscard]] std::optional<OperationResult>
boundaryNodes(const document::Draft& draft, const document::CompositionId compositionId,
              const std::vector<document::LayerId>& layers, std::set<document::NodeId>& nodes,
              std::vector<document::LayerId>* entryLayers = nullptr) {
    const auto* composition = draft.project().findComposition(compositionId);
    if (composition == nullptr)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Composition " + std::to_string(compositionId.value()) + " does not exist");
    if (layers.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "No layers were named");
    for (const auto layer : layers) {
        const auto* boundary = composition->graph().findLayer(layer);
        if (boundary == nullptr)
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "Layer " + std::to_string(layer.value()) +
                                                 " is not in the composition");
        // #502: a Merge entry's own layer is not a node; it is handled by the caller.
        if (boundary->entryOwned()) {
            if (entryLayers != nullptr &&
                std::ranges::find(*entryLayers, layer) == entryLayers->end())
                entryLayers->push_back(layer);
            continue;
        }
        nodes.insert(boundary->nodeId);
    }
    return std::nullopt;
}

// One "reads from" relation of the graph: `reader` uses what `source` makes.
struct NodeUse final {
    document::NodeId reader;
    document::NodeId source;
};

// #714: the nodes only the deleted layers use. A layer brings its own source nodes (an imported
// SVG's paths, shapes, gradients and merges; a footage layer's Media node; a Solid's source), and
// deleting the layer used to leave them in the graph, linked to nothing.
//
// The candidates are everything upstream of the deleted layers: what links into their slots (or,
// for a layer with a boundary node, into that node), what drives their parameters, their matte
// nodes, and so on up the graph. A candidate stays when anything that is not itself going reads
// it: another layer, a node outside the set, a parameter it drives elsewhere. So a source shared
// with another layer stays, and so does everything it reads in turn. A node Delete refuses (a
// locked layer's) stays as well.
[[nodiscard]] std::set<document::NodeId>
ownedSourceNodes(const document::Composition& composition,
                 const std::vector<document::LayerId>& layers,
                 const std::set<document::NodeId>& boundaryNodes) {
    const auto& graph = composition.graph();
    std::set<document::LayerId> deleted(layers.begin(), layers.end());
    // The slots that go with the deleted entry layers: a link into one of them is the deleted
    // layer reading its source, not the Merge's other layers reading it.
    std::set<std::pair<document::NodeId, document::LayerSlotId>> deletedSlots;
    for (const auto& layer : graph.layerOutputs())
        if (deleted.contains(layer.layerId) && layer.entryOwned())
            deletedSlots.emplace(layer.nodeId, *layer.entrySlot);

    // Every use that survives the deletion, and the seeds: what the deleted layers read.
    std::vector<NodeUse> uses;
    std::vector<document::NodeId> pending;
    for (const auto& edge : graph.edges()) {
        if (const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination);
            slot != nullptr && deletedSlots.contains({slot->stackNodeId, slot->slotId})) {
            pending.push_back(edge.source.nodeId);
            continue;
        }
        uses.push_back({detail::destinationNode(edge.destination), edge.source.nodeId});
    }
    const auto driverOf = [&](const document::ParameterId id) -> std::optional<document::NodeId> {
        const auto* parameter = composition.parameters().find(id);
        const auto* driver =
            parameter ? std::get_if<document::DriverBindingSource>(&parameter->source) : nullptr;
        return driver != nullptr ? std::optional(driver->sourceNodeId) : std::nullopt;
    };
    for (const auto& node : graph.nodes())
        for (const auto& binding : node.parameters)
            if (const auto source = driverOf(binding.parameterId))
                uses.push_back({node.id, *source});
    for (const auto& layer : graph.layerOutputs()) {
        const bool going = deleted.contains(layer.layerId);
        const auto read = [&](const document::NodeId source) {
            if (going)
                pending.push_back(source);
            else
                uses.push_back({layer.nodeId, source});
        };
        for (const auto& binding : layer.parameters)
            if (const auto source = driverOf(binding.parameterId))
                read(*source);
        if (layer.trackMatte && layer.trackMatte->node)
            read(layer.trackMatte->node->nodeId);
        for (const auto& mask : layer.masks) {
            if (mask.matte)
                read(mask.matte->nodeId);
            for (const auto& [parameterId, schemaKey] : document::layerMaskParameters(mask)) {
                (void)schemaKey;
                if (const auto source = driverOf(parameterId))
                    read(*source);
            }
        }
    }
    for (const auto node : boundaryNodes)
        pending.push_back(node);

    // Upstream closure of the seeds.
    std::multimap<document::NodeId, document::NodeId> sourcesOf;
    for (const auto& use : uses)
        sourcesOf.emplace(use.reader, use.source);
    std::set<document::NodeId> owned;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (graph.findNode(id) == nullptr || !owned.insert(id).second)
            continue;
        const auto [first, last] = sourcesOf.equal_range(id);
        for (auto source = first; source != last; ++source)
            pending.push_back(source->second);
    }
    // A boundary node goes whatever reads it; it is the layer. Everything else must be deletable
    // and read by nothing that stays.
    for (auto candidate = owned.begin(); candidate != owned.end();)
        if (!boundaryNodes.contains(*candidate) &&
            nodeRemovalRefusal(composition, *candidate) != NodeRemovalRefusal::None)
            candidate = owned.erase(candidate);
        else
            ++candidate;
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& use : uses)
            if (owned.contains(use.source) && !owned.contains(use.reader) &&
                !boundaryNodes.contains(use.source)) {
                owned.erase(use.source);
                changed = true;
            }
    }
    for (const auto node : boundaryNodes)
        owned.erase(node);
    return owned;
}

} // namespace

OperationResult DeleteLayers::apply(document::Draft& draft) const {
    std::set<document::NodeId> nodes;
    std::vector<document::LayerId> entryLayers;
    if (auto failure = boundaryNodes(draft, composition_, layers_, nodes, &entryLayers))
        return std::move(*failure);
    // #714: the layers' own source nodes go with them, in the same operation (one undo step).
    // Read from the graph as it is before anything is removed.
    {
        auto owned = ownedSourceNodes(*draft.project().findComposition(composition_), layers_, nodes);
        nodes.merge(owned);
    }
    // An entry layer leaves with its slot; its Merge stays, and the sources only it used go
    // with the nodes below.
    for (const auto layer : entryLayers)
        if (auto failure =
                detail::deleteEntryLayer(*draft.project().findComposition(composition_), layer))
            return std::move(*failure);
    if (nodes.empty())
        return OperationResult::applied();
    auto removed = RemoveNodes(composition_, std::move(nodes)).apply(draft);
    if (!entryLayers.empty() && removed.status == OperationStatus::Applied)
        removed.affectedTimes.reset();
    return removed;
}

OperationResult DuplicateLayers::apply(document::Draft& draft) const {
    std::set<document::NodeId> nodes;
    if (auto failure = boundaryNodes(draft, composition_, layers_, nodes))
        return std::move(*failure);
    // A layer named N times is copied N times, as running Duplicate N times would: pass k copies
    // every layer named more than k times, so layers copied in the same pass keep their parent and
    // matte links among those copies.
    std::map<document::LayerId, std::size_t> named;
    for (const auto layer : layers_)
        ++named[layer];
    std::size_t passes = 0;
    for (const auto& [layer, count] : named)
        passes = std::max(passes, count);
    OperationResult result = OperationResult::applied();
    std::vector<std::vector<OperationOutput>> passOutputs;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        std::vector<document::LayerId> copied;
        for (const auto& [layer, count] : named)
            if (count > pass)
                copied.push_back(layer);
        std::set<document::NodeId> passNodes;
        std::vector<document::LayerId> passEntries;
        if (auto failure = boundaryNodes(draft, composition_, copied, passNodes, &passEntries))
            return std::move(*failure);
        auto duplicated =
            passNodes.empty()
                ? OperationResult::applied()
                : DuplicateNodes(composition_, std::move(passNodes), document::Vec2d{0, 0})
                      .apply(draft);
        if (duplicated.status == OperationStatus::Rejected)
            return duplicated;
        // #502: an entry layer is copied into a new slot directly below its own.
        std::map<document::LayerId, document::LayerId> entryCopies;
        for (const auto layer : passEntries) {
            detail::EntryLayerCopy copy;
            if (auto failure = detail::duplicateEntryLayer(draft, composition_, layer, copy,
                                                           duplicated.outputs))
                return std::move(*failure);
            entryCopies.emplace(layer, copy.layer);
        }
        if (!entryCopies.empty()) {
            duplicated.affectedTimes.reset();
            duplicated.layerIdentityRemaps.reset();
            // Parent and matte links among the layers copied in this pass follow the copies,
            // as DuplicateNodes does among Layer nodes.
            std::map<document::LayerId, document::LayerId> copies = entryCopies;
            for (const auto& output : duplicated.outputs)
                if (output.name.starts_with("layer."))
                    if (const auto* id = std::get_if<document::LayerId>(&output.id))
                        copies.emplace(document::LayerId::fromRaw(std::stoull(
                                           output.name.substr(std::string_view("layer.").size()))),
                                       *id);
            auto& graph = draft.project().findComposition(composition_)->graph();
            for (const auto& [original, copy] : copies) {
                const auto* source = std::as_const(graph).findLayer(original);
                auto* target = graph.findLayer(copy);
                if (source == nullptr || target == nullptr)
                    continue;
                if (source->parent && copies.contains(*source->parent))
                    target->parent = copies.at(*source->parent);
                else if (entryCopies.contains(original))
                    target->parent.reset();
                if (source->trackMatte && copies.contains(source->trackMatte->source))
                    target->trackMatte->source = copies.at(source->trackMatte->source);
            }
        }
        if (pass == 0) {
            result = duplicated;
        } else {
            result.outputs.insert(result.outputs.end(), duplicated.outputs.begin(),
                                  duplicated.outputs.end());
            // A footprint only one pass proved is not the whole edit's.
            result.affectedTimes =
                result.affectedTimes && duplicated.affectedTimes
                    ? mergeAffectedTimeFootprints(result.affectedTimes, *duplicated.affectedTimes)
                    : std::nullopt;
            result.layerIdentityRemaps.reset();
        }
        passOutputs.push_back(std::move(duplicated.outputs));
    }
    // One "layer" output per copy, in argument order: the k-th mention of a layer is its k-th copy.
    std::vector<OperationOutput> copies;
    std::map<document::LayerId, std::size_t> seen;
    for (const auto layer : layers_) {
        const auto name = "layer." + std::to_string(layer.value());
        for (const auto& output : passOutputs[seen[layer]++])
            if (output.name == name)
                copies.push_back({"layer", output.id});
    }
    result.outputs.insert(result.outputs.end(), copies.begin(), copies.end());
    return result;
}

} // namespace bloom::commands

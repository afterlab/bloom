#pragma once

// #502 Every Merge entry is a layer. These are the shared, read-only answers about entries that the
// compiler, the commands, the render-reachability proof and every editor ask, so one rule decides
// which layer an entry is, which parameters a layer owns, and which entry an Adjustment serves.
//
//   * An entry's layer is the Layer node nearest the Merge along its image chain (through image
//     effects' first image input and Image Reroutes), else the entry's own layer -- a
//     LayerOutputBoundary owned by the slot, which the slot's layerId names -- else none yet: the
//     entry then behaves exactly as an entry layer at its defaults (an identity).
//   * A layer's parameters are its Layer node's bindings, or the entry layer's own bindings.
//   * An Adjustment node serves the first Merge entry each of its image paths reaches. It serves an
//     entry only when every path reaches the same one.

#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bloom::document {

class Composition;
class ParameterStore;

struct MergeEntryRef {
    NodeId merge;
    LayerSlotId slot;

    friend bool operator==(const MergeEntryRef&, const MergeEntryRef&) = default;
};

struct MergeEntryRefHash {
    [[nodiscard]] std::size_t operator()(const MergeEntryRef& entry) const noexcept {
        return std::hash<std::uint64_t>{}(entry.merge.value() * 0x9E3779B97F4A7C15ULL ^
                                          entry.slot.value());
    }
};

// The edge feeding a Merge slot's image content, or null for an audio-only slot.
[[nodiscard]] const EdgeRecord* mergeSlotImageEdge(const CanonicalGraph& graph, NodeId merge,
                                                   LayerSlotId slot);

// The Merge entry that owns `slot` anywhere in the graph.
[[nodiscard]] std::optional<MergeEntryRef> mergeEntryOfSlot(const CanonicalGraph& graph,
                                                            LayerSlotId slot);

// The parameter bindings a layer owns: its Layer node's, or an entry layer's own.
[[nodiscard]] std::span<const ParameterBinding> layerParameterBindings(
    const CanonicalGraph& graph, const LayerOutputBoundary& layer);
[[nodiscard]] const ParameterBinding* layerParameterBinding(const CanonicalGraph& graph,
                                                            const LayerOutputBoundary& layer,
                                                            std::string_view role);

// The Layer node nearest `source` walking up its image chain, or null when the chain reaches
// anything else first (a source, a Merge, an Adjustment, a multi-image node's secondary input is
// never walked).
[[nodiscard]] const LayerOutputBoundary*
chainLayer(const CanonicalGraph& graph, const OutputPortRef& source,
           const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The entry's own layer (the boundary its slot owns), whether or not a chain Layer shadows it.
[[nodiscard]] const LayerOutputBoundary* entryOwnedLayer(const CanonicalGraph& graph, NodeId merge,
                                                         LayerSlotId slot);

// The entry's layer, as defined above; null when the entry has none yet.
[[nodiscard]] const LayerOutputBoundary*
mergeEntryLayer(const CanonicalGraph& graph, NodeId merge, LayerSlotId slot,
                const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// Every Merge entry whose layer is `layer` (more than one for a shared Layer node).
[[nodiscard]] std::vector<MergeEntryRef>
mergeEntriesOfLayer(const CanonicalGraph& graph, LayerId layer,
                    const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The first Merge entries each image path from an Adjustment node reaches, in a stable order.
[[nodiscard]] std::vector<MergeEntryRef>
adjustmentTargets(const CanonicalGraph& graph, NodeId adjustment,
                  const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// Every Adjustment that serves exactly one entry, keyed by that entry.
[[nodiscard]] std::unordered_map<MergeEntryRef, NodeId, MergeEntryRefHash>
adjustmentEntries(const CanonicalGraph& graph,
                  const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// Whether `entry` is an adjustment entry (an Adjustment node serves it).
[[nodiscard]] std::optional<NodeId>
mergeEntryAdjustment(const CanonicalGraph& graph, NodeId merge, LayerSlotId slot,
                     const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The roles an entry layer binds, in Layer node order, with their schema keys. Time Remap is bound
// only while enabled.
struct EntryLayerRole {
    std::string_view role;
    std::string_view schemaKey;
};
[[nodiscard]] std::span<const EntryLayerRole> entryLayerRoles() noexcept;

// Entry layers' document rules: the slot exists in the owning Merge and names the layer, the slot
// is not fed directly by a Layer node, the parameters bind exactly the entry roles (Time Remap
// optional) to parameters of the right schema owned by nothing else, each a constant or an
// animation curve.
[[nodiscard]] ValidationResult validateEntryLayers(const CanonicalGraph& graph,
                                                   const ParameterStore& parameters);

} // namespace bloom::document

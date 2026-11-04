#pragma once

// #502 Every Merge entry is a layer. The read-back view of a composition's Merge entries that the
// MCP `layers` and `nodes` queries and the Python snapshot share, so both report the same entry,
// layer, values and markers: whether the entry's layer is a Layer node, its own entry layer or
// none yet (it then renders as an entry layer at its defaults), its parameter ids and constant
// values by role, and whether it is an adjustment entry, a shared Layer node or a nested Merge.

#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {
class Composition;
}

namespace bloom::scripting {

enum class EntryLayerKind : std::uint8_t {
    // No layer yet: the entry renders as an entry layer at its defaults.
    None,
    // A Layer node on the entry's image chain (the nearest one to the Merge).
    Node,
    // The entry's own layer (a LayerOutputBoundary its slot owns).
    Entry,
};

[[nodiscard]] std::string_view entryLayerKindName(EntryLayerKind kind) noexcept;
[[nodiscard]] std::string_view layerPivotName(document::LayerPivot pivot) noexcept;

// One layer control: its parameter (absent while the entry has no layer) and its constant value,
// or the default the entry renders with when it has no layer. `value` is empty for an animated or
// driven parameter (read its keys through the parameter id).
struct LayerRoleValue final {
    std::string_view role;
    std::optional<document::ParameterId> parameter;
    std::optional<document::ParameterValue> value;
};

// The controls of `layer` in entry-role order, or the entry defaults when `layer` is null. Time
// Remap is listed only while it is bound (enabled).
[[nodiscard]] std::vector<LayerRoleValue>
layerRoleValues(const document::Composition& composition,
                const document::LayerOutputBoundary* layer);

struct MergeEntryView final {
    document::NodeId merge;
    document::LayerSlotId slot;
    // Position in the Merge's stack, 0 the topmost.
    std::size_t index = 0;
    // The composition's output Merge (the Timeline's root).
    bool outputMerge = false;
    // The entry's layer; null while it has none yet.
    const document::LayerOutputBoundary* layer = nullptr;
    EntryLayerKind kind = EntryLayerKind::None;
    // The Layer node when kind is Node.
    std::optional<document::NodeId> layerNode;
    // The node feeding the slot's image directly, and its type id.
    std::optional<document::NodeId> sourceNode;
    std::string sourceType;
    // The Adjustment node serving this entry, when it is an adjustment entry.
    std::optional<document::NodeId> adjustment;
    // The entry's Layer node also is the layer of another entry (it shares its values).
    bool shared = false;
    // The Merge whose stack this entry shows, when it is a nested Merge (through Layer nodes,
    // reroutes and image filters).
    std::optional<document::NodeId> nestedMerge;
    document::LayerPivot pivot = document::LayerPivot::Frame;
    std::vector<LayerRoleValue> roles;
};

// Every entry of every Merge: the output Merge first, then the others in graph order, each top to
// bottom.
[[nodiscard]] std::vector<MergeEntryView>
mergeEntryViews(const document::Composition& composition);

// The entry view of one slot, or nullopt when the Merge has no such slot.
[[nodiscard]] std::optional<MergeEntryView>
mergeEntryView(const document::Composition& composition, document::NodeId merge,
               document::LayerSlotId slot);

} // namespace bloom::scripting

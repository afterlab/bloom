#pragma once

// #645 Drilling into grouped artwork. The Viewer's level is a chain of group layers, each a child
// entry of the one before it, starting at the composition's output Merge. These are the read-only
// answers about that structure, shared by the session (which holds the level), the runtime pick
// and the MCP and Python level setters, so one rule decides what a group, a child and a shape are.
//
//   * A layer is a GROUP when its image chain (through its Layer node, image effects' first image
//     input and Image Reroutes, so an imported group's Blur is included) reaches a Merge: the
//     layer's content Merge. Its CHILDREN are that Merge's entry layers, topmost first.
//   * A layer used as another layer's track matte in the same Merge is a matte source: never a
//     child a click picks, never a level.
//   * A SHAPE is a layer whose source is a Path-kind Shape Source, or a small group (the importer's
//     gradient fill, dashed or gradient stroke): two to eight leaf entries side by side, no group
//     among them, whose Shape Sources all read one and the same Path node, directly or through Path
//     Stroke, any other part drawing through a track matte. Its points are edited through
//     `edited`: the layer whose Shape reads its path (or that Path node) directly.
//
// Everything here is pure: a function of one composition's graph and parameters.

#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::document {

class Composition;

// The Merge whose picture `layer` shows, or nullopt when the layer is a leaf.
[[nodiscard]] std::optional<NodeId>
layerContentMerge(const Composition& composition, LayerId layer,
                  const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The node a leaf layer's image chain ends at (a Shape Source, an Image Source, a Vector Artwork
// node, ...), or nullopt for a group or an unconnected entry.
[[nodiscard]] std::optional<NodeId>
layerContentSource(const Composition& composition, LayerId layer,
                   const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The Merge a level's children are entries of: the composition's output Merge for the top level
// (an empty chain), else the last group's content Merge. Nullopt when the chain does not resolve.
[[nodiscard]] std::optional<NodeId>
drillLevelMerge(const Composition& composition, std::span<const LayerId> chain,
                const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// Whether `layer` is the track matte source of another entry layer of `merge`.
[[nodiscard]] bool
isTrackMatteSource(const Composition& composition, NodeId merge, LayerId layer,
                   const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The pickable children of `merge`, topmost first: each entry's layer, without plain entries (no
// layer yet) and without matte sources.
[[nodiscard]] std::vector<LayerId>
drillChildren(const Composition& composition, NodeId merge,
              const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// One entry of a Merge with what its image chain reaches: a content Merge (a group) or a source.
struct DrillEntry final {
    LayerId layer;
    std::optional<NodeId> contentMerge;
    std::optional<NodeId> source;
    // Used as the track matte of another entry of the same Merge.
    bool matteSource = false;
};

// Every entry of `merge` that has a layer, topmost first, matte sources included and marked.
[[nodiscard]] std::vector<DrillEntry>
mergeEntryContents(const Composition& composition, NodeId merge,
                   const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// Every entry layer of `merge`, topmost first, matte sources included (plain entries left out).
[[nodiscard]] std::vector<LayerId>
mergeEntryLayers(const Composition& composition, NodeId merge,
                 const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The group layers enclosing `layer`, outermost first, ending with the one whose content Merge
// holds `layer` as an entry. Empty for a top-level layer; nullopt when `layer` is not reachable as
// an entry from the composition's output Merge.
[[nodiscard]] std::optional<std::vector<LayerId>>
drillAncestors(const Composition& composition, LayerId layer,
               const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// A layer drawn as one editable outline (see the file comment).
struct DrillShape final {
    // The layer the session selects to show and edit the points: the shape layer itself, or the
    // small group's child whose Shape reads the path or Path node directly.
    LayerId edited;
    // The Path node the outline comes from, when a Path node drives it.
    std::optional<NodeId> pathNode;
    // True for a small group (the shape is a group of its fill and stroke parts).
    bool smallGroup = false;

    friend bool operator==(const DrillShape&, const DrillShape&) = default;
};

[[nodiscard]] std::optional<DrillShape>
drillShape(const Composition& composition, LayerId layer,
           const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The first layer whose Shape draws Path node `pathNode`'s value directly (graph order), so
// selecting a Path node in the node editor can say where its shape is.
[[nodiscard]] std::optional<LayerId>
shapeLayerOfPathNode(const Composition& composition, NodeId pathNode,
                     const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// A group the Viewer can enter: its content is a Merge and it is not a small-group shape.
[[nodiscard]] bool isDrillGroup(const Composition& composition, LayerId layer,
                                const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// Why a level was refused.
enum class DrillLevelRefusal : std::uint8_t {
    None = 0,
    // An id names no layer of the composition.
    UnknownLayer = 1,
    // A chain member is not a group (its content is not a Merge, or it is a small-group shape).
    NotAGroup = 2,
    // A chain member is not a child entry of the level before it (the first: of the top level).
    NotAChain = 3,
    // The points layer is not a shape, or not a child of the last group.
    NotAShape = 4,
};

struct DrillLevelCheck final {
    DrillLevelRefusal refusal = DrillLevelRefusal::None;
    // The id the refusal is about.
    LayerId layer;
    [[nodiscard]] bool ok() const noexcept { return refusal == DrillLevelRefusal::None; }
};

[[nodiscard]] DrillLevelCheck
checkDrillLevel(const Composition& composition, std::span<const LayerId> chain,
                std::optional<LayerId> points,
                const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The layers the Viewer draws dimmed while it is inside `chain` (isolation): at every level from
// the top, each child other than the group entered next. Matte sources are left alone (the layer
// they matte is dimmed, or is inside the entered group and must not be), and nothing inside the
// last entered group is dimmed. Empty for the top level or a chain that does not resolve.
[[nodiscard]] std::vector<LayerId>
drillIsolationDimmed(const Composition& composition, std::span<const LayerId> chain,
                     const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// The longest prefix of `chain` that is still a valid chain of groups.
[[nodiscard]] std::size_t
validDrillPrefix(const Composition& composition, std::span<const LayerId> chain,
                 const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// A plain sentence for a refusal, naming the layer by its name or id.
[[nodiscard]] std::string drillLevelRefusalText(const Composition& composition,
                                                const DrillLevelCheck& check);

} // namespace bloom::document

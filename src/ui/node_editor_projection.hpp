#pragma once
// The Nodes canvas's own indexes (#359): what one projection learns about the graph's links, and
// which live item stands for which document id. Both are derived presentation state -- rebuilt
// from the snapshot, never written back -- and both exist so a projection reconciles only what an
// edit changed and a gesture finds its items without walking the scene.

#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bloom::ui::node_editor {
class NodeEdgeItem;
class NodeGroupItem;
class NodeItem;

// An input port as an ordered value. The document's InputPortRef is a variant with equality only;
// the canvas orders it so links can be keyed by where they land.
struct NodeInputKey final {
    std::uint8_t kind = 0;
    std::uint64_t node = 0;
    std::uint64_t slot = 0;
    std::string port;
    friend auto operator<=>(const NodeInputKey&, const NodeInputKey&) = default;
};
[[nodiscard]] NodeInputKey nodeInputKey(const document::InputPortRef& input);

// One drawn link, by its destination first: an input has exactly one incoming link, so every link
// into one input sorts together and a destination lookup is a range rather than a scan.
struct NodeLinkKey final {
    NodeInputKey destination;
    std::uint64_t sourceNode = 0;
    std::string sourcePort;
    std::uint64_t edge = 0;
    friend auto operator<=>(const NodeLinkKey&, const NodeLinkKey&) = default;
};
[[nodiscard]] NodeLinkKey nodeLinkKey(const document::EdgeRecord& link);

// Built once per projection from the composition: the canvas's one list of links (graph edges
// followed by every driver binding), the inputs an edge terminates on, and the outputs anything
// reads from. Every card's sockets answer "am I linked?" from here, so a projection walks the edge
// list once instead of once per port per card.
struct NodeProjectionIndex final {
    std::vector<document::EdgeRecord> links;
    std::set<NodeInputKey> edgeInputs;
    std::unordered_map<std::uint64_t, std::set<std::string>> linkedOutputs;

    [[nodiscard]] bool edgeInto(const document::InputPortRef& input) const {
        return edgeInputs.contains(nodeInputKey(input));
    }
    [[nodiscard]] bool outputLinked(const document::NodeId node, const std::string& port) const {
        const auto found = linkedOutputs.find(node.value());
        return found != linkedOutputs.end() && found->second.contains(port);
    }
};
[[nodiscard]] NodeProjectionIndex buildProjectionIndex(const document::Composition& composition);

// Everything a card's refresh reads from outside its own composition's content, held with the
// snapshot itself so identity comparisons stay sound: the retained snapshot keeps the content it
// was taken from alive, so an equal address can only be that same content.
struct NodeProjectionInputs final {
    std::optional<document::Snapshot> snapshot;
    document::CompositionId composition;
    const void* content = nullptr;
    const void* assets = nullptr;
    std::size_t assetCount = 0;
    std::vector<std::pair<std::uint64_t, std::string>> compositions;
    std::optional<std::string> compositionColorSpace;
    std::optional<document::ColorSettings> colorSettings;
    std::string workingColorSpace;
    bool colorConverterReady = false;

    [[nodiscard]] bool sameContent(const NodeProjectionInputs& other) const;
};

// The live items the scene holds, by document id.
struct NodeCanvasIndex final {
    std::unordered_map<std::uint64_t, NodeItem*> nodes;
    std::unordered_map<std::uint64_t, NodeGroupItem*> groups;
    // A node is a member of at most one group (document::findNodeGroupOf), so this is one answer.
    std::unordered_map<std::uint64_t, NodeGroupItem*> memberGroup;
    std::map<NodeLinkKey, NodeEdgeItem*> edges;
    // The cards whose values follow the playhead (bug #401): a time change visits only these.
    std::vector<NodeItem*> timeVarying;
    // The link index of the projection on screen, which a gesture's per-move checks ask.
    NodeProjectionIndex links;
    NodeProjectionInputs projected;

    [[nodiscard]] NodeItem* node(const document::NodeId id) const {
        const auto found = nodes.find(id.value());
        return found == nodes.end() ? nullptr : found->second;
    }
    [[nodiscard]] NodeGroupItem* group(const document::NodeGroupId id) const {
        const auto found = groups.find(id.value());
        return found == groups.end() ? nullptr : found->second;
    }
    // Every link whose destination is `input`.
    template <class Visit> void visitLinksInto(const document::InputPortRef& input, Visit visit) const {
        const auto key = nodeInputKey(input);
        for (auto found = edges.lower_bound(NodeLinkKey{key, 0, {}, 0});
             found != edges.end() && found->first.destination == key; ++found)
            visit(*found->second);
    }
    void clear() {
        nodes.clear();
        groups.clear();
        memberGroup.clear();
        edges.clear();
        timeVarying.clear();
        links = {};
        projected = {};
    }
};
} // namespace bloom::ui::node_editor

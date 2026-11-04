#include <bloom/document/image_preview.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>

namespace bloom::document {

NodeDefinition imagePreviewDefinition() {
    NodeDefinition node{
        {std::string(kImagePreviewNodeType), kImagePreviewNodeSchemaVersion},
        NodeLoweringKind::ImagePreview,
        // Optional: an unconnected preview shows a placeholder. The port also
        // takes a Mask or a ChannelSet (acceptsPixelKinds), shown as its image.
        {{std::string(kImagePreviewInputPort), SocketValueKind::Image, false, true}},
        // A sink: nothing connects from it, so no plan ever reaches it.
        {},
        {},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Output};
    // An inline selector (no socket), like Transform 2D's extent: it chooses how the interface
    // draws the preview and never reaches evaluation.
    node.parameters.push_back({std::string(kImagePreviewDisplayParameterRole),
                               std::string(kImagePreviewDisplayParameterSchemaKey),
                               ParameterValueKind::Integer, true, false,
                               static_cast<std::int64_t>(ImagePreviewDisplay::Rgba)});
    return node;
}

bool isImagePreviewNodeType(const std::string_view typeId) noexcept {
    return typeId == kImagePreviewNodeType;
}

std::optional<ImagePreviewSource> imagePreviewSource(const CanonicalGraph& graph,
                                                     const NodeId preview,
                                                     const NodeDefinitionRegistry& registry) {
    const auto* node = graph.findNode(preview);
    if (node == nullptr || !isImagePreviewNodeType(node->typeId))
        return std::nullopt;
    const auto edgeInto = [&](const NodeId id, const std::string_view port) -> const EdgeRecord* {
        const auto found = std::ranges::find_if(graph.edges(), [&](const auto& edge) {
            const auto* fixed = std::get_if<NodeInputRef>(&edge.destination);
            return fixed != nullptr && fixed->nodeId == id && fixed->port == port;
        });
        return found == graph.edges().end() ? nullptr : &*found;
    };
    const auto* edge = edgeInto(preview, kImagePreviewInputPort);
    std::unordered_set<std::uint64_t> seen;
    while (edge != nullptr) {
        const auto* source = graph.findNode(edge->source.nodeId);
        if (source == nullptr || !seen.insert(source->id.value()).second)
            return std::nullopt;
        if (isRerouteNodeType(source->typeId)) {
            edge = edgeInto(source->id, kValuePortName);
            continue;
        }
        const auto kind = graph.outputKind(edge->source, registry);
        if (!kind || !isPixelSocketKind(*kind))
            return std::nullopt;
        return ImagePreviewSource{edge->source, *kind};
    }
    return std::nullopt;
}

} // namespace bloom::document

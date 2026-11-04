#pragma once

// #490: the Image Preview node. An inspection sink: one input that takes an Image, a Mask or a
// ChannelSet, no output, and one display choice. It never enters a compiled composition plan --
// nothing is reached from a node with no output -- so it cannot change the composition's output,
// RAM Preview frames, exports or headless renders. The interface shows its input as a node-card
// thumbnail and, optionally, behind the node editor's graph.

#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kImagePreviewNodeType = "bloom.image-preview";
inline constexpr std::uint32_t kImagePreviewNodeSchemaVersion = 1;
inline constexpr std::string_view kImagePreviewInputPort = "image";
inline constexpr std::string_view kImagePreviewDisplayParameterRole = "display";
inline constexpr std::string_view kImagePreviewDisplayParameterSchemaKey =
    "bloom.image-preview.display";

// The stored values of `display`. RGBA through Alpha are the Viewer's own channel presentations;
// Normalized stretches the visible image's finite minimum..maximum to black..white so a data pass
// (depth, mist, a position) is readable.
enum class ImagePreviewDisplay : std::int64_t {
    Rgba = 0,
    Rgb = 1,
    Alpha = 2,
    Red = 3,
    Green = 4,
    Blue = 5,
    Normalized = 6,
};
inline constexpr std::int64_t kMaximumImagePreviewDisplay =
    static_cast<std::int64_t>(ImagePreviewDisplay::Normalized);

[[nodiscard]] NodeDefinition imagePreviewDefinition();
[[nodiscard]] bool isImagePreviewNodeType(std::string_view typeId) noexcept;

// What an Image Preview shows: the output its input is connected to, followed through any
// reroutes, and that output's kind (Image, Mask or ChannelSet). Nullopt when the input is
// unconnected, ends at an unconnected reroute, or `preview` is not an Image Preview.
struct ImagePreviewSource final {
    OutputPortRef output;
    SocketValueKind kind = SocketValueKind::Image;
    friend bool operator==(const ImagePreviewSource&, const ImagePreviewSource&) = default;
};
[[nodiscard]] std::optional<ImagePreviewSource>
imagePreviewSource(const CanonicalGraph& graph, NodeId preview,
                   const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

} // namespace bloom::document

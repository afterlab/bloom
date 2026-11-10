#pragma once

// #642 Vector Artwork: an SVG imported "as one layer". The layer is an ordinary Merge entry whose
// source is one Vector Artwork node. The node names a data block that holds the SVG file's own
// bytes, so the artwork renders with its source file missing, and the compiler expands the node,
// before anything is lowered, into exactly the nodes the editable import would have made
// (bloom/svgartwork/svg_artwork.hpp): the artist's graph holds one node, every route renders the
// file's vector geometry at the output resolution, and the picture is the editable import's.
// See docs/architecture/vector-artwork.md.

#include <bloom/document/data_block.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kVectorArtworkNodeType = "bloom.vector-artwork";
inline constexpr std::uint32_t kVectorArtworkNodeSchemaVersion = 1;
inline constexpr std::string_view kVectorArtworkOutputPort = "image";
// The data block the artwork is read from, by DataBlockRecordId; 0 names none (an empty node,
// which draws nothing).
inline constexpr std::string_view kVectorArtworkBlockParameterRole = "artwork";
inline constexpr std::string_view kVectorArtworkBlockParameterSchemaKey = "bloom.vector-artwork.artwork";
// Contain-and-centre into the composition (true), or the file's own pixels, centred (false).
inline constexpr std::string_view kVectorArtworkFitParameterRole = "fit";
inline constexpr std::string_view kVectorArtworkFitParameterSchemaKey = "bloom.vector-artwork.fit";

// The data block: an Opaque block whose payload is the SVG file's bytes exactly as read, whose
// provenance is the file's locator and the SHA-256 of those bytes (the same pair as the file's
// Assets record), and whose media type is SVG's.
inline constexpr std::string_view kVectorArtworkBlockTypeId = "bloom.vector-artwork.svg";
inline constexpr std::string_view kVectorArtworkBlockMediaType = "image/svg+xml";
inline constexpr SchemaVersion kVectorArtworkBlockSchemaVersion{1, 0};

[[nodiscard]] NodeDefinition vectorArtworkDefinition();
// Whether `value` lies in the document domain of the parameter `schemaKey` (one of the two above).
[[nodiscard]] bool isVectorArtworkParameterValueValid(std::string_view schemaKey,
                                                      const ParameterValue& value) noexcept;
[[nodiscard]] constexpr bool isVectorArtworkParameterSchemaKey(const std::string_view key) noexcept {
    return key == kVectorArtworkBlockParameterSchemaKey || key == kVectorArtworkFitParameterSchemaKey;
}

[[nodiscard]] bool isVectorArtworkBlock(const DataBlockRecord& block) noexcept;

// The block a Vector Artwork node names: its constant `artwork` value. Nothing when the node is
// not a Vector Artwork node, names none, or names a block that is missing or is not an SVG block.
[[nodiscard]] std::optional<DataBlockRecordId> vectorArtworkBlockId(const NodeRecord& node,
                                                                    const ParameterStore& parameters);
[[nodiscard]] const DataBlockRecord* vectorArtworkBlock(const Project& project, const NodeRecord& node,
                                                        const ParameterStore& parameters);
[[nodiscard]] bool vectorArtworkFits(const NodeRecord& node, const ParameterStore& parameters);

// The Assets record of the file a block was read from: the record with the block's locator and
// digest. Nothing when the file was never recorded or its record was removed.
[[nodiscard]] const AssetRecord* vectorArtworkSourceAsset(const Project& project,
                                                          const DataBlockRecord& block);
// #646: an Assets record of an SVG file (an image asset whose locator ends in ".svg", any case).
// It has no decoded picture; adding it to a composition builds the artwork through the SVG import.
[[nodiscard]] bool isSvgSourceAsset(const AssetRecord& asset) noexcept;
// #646: a project made before an SVG could be added again may hold an Image Source whose asset is an
// SVG record. It has no picture to decode, so the layer draws nothing. The named warning for such
// a node, or nothing for any other node: "'logo.svg' is an SVG source, not footage: add it again
// from Assets to rebuild the artwork". The document is not rewritten.
[[nodiscard]] std::optional<std::string> svgFootageWarning(const Project& project, const Composition& composition,
                                                           const NodeRecord& node);
// The same for a layer: the warning of the image source its content reads, through the nodes
// between (a blur, a colour node, a reroute).
[[nodiscard]] std::optional<std::string> layerSvgFootageWarning(const Project& project, const Composition& composition,
                                                                LayerId layer);
// The SVG block of the project that holds bytes with this SHA-256, so a second import of the
// same file shares one copy.
[[nodiscard]] const DataBlockRecord* findVectorArtworkBlock(const Project& project,
                                                            const core::Sha256Digest& digest);

// Every Vector Artwork node of `composition` whose `artwork` names a block the project does not
// hold as an SVG block. Project validation refuses such a node, so a block an artwork reads
// cannot be removed from under it.
[[nodiscard]] ValidationResult validateVectorArtworkReferences(const Project& project,
                                                               const Composition& composition);

} // namespace bloom::document

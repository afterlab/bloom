#pragma once

#include <bloom/document/node_definition_registry.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ADR 0026 multi-channel nodes: the image source's channel selection parameters, the Shuffle,
// Channel Copy and Remove definitions, and the Composition Output's channel layer list.
namespace bloom::document {

inline constexpr std::string_view kImageSourceNodeType = "bloom.image-source";
inline constexpr std::uint32_t kImageSourceNodeSchemaVersion = 2;
inline constexpr std::string_view kImageSourceImageOutputPort = "image";
inline constexpr std::string_view kImageSourceChannelsOutputPort = "channels";
inline constexpr std::string_view kImagePartParameterRole = "part";
inline constexpr std::string_view kImagePartParameterSchemaKey = "bloom.image.part";
inline constexpr std::string_view kImageLayerParameterRole = "layer";
inline constexpr std::string_view kImageLayerParameterSchemaKey = "bloom.image.layer";

// #480: a Video source's decoded frame as a channel set (R, G, B, A in the working space).
inline constexpr std::string_view kVideoSourceNodeType = "bloom.video-source";
inline constexpr std::string_view kVideoSourceChannelsOutputPort = "channels";

inline constexpr std::string_view kChannelShuffleNodeType = "bloom.channel-shuffle";
inline constexpr std::string_view kChannelCopyNodeType = "bloom.channel-copy";
inline constexpr std::string_view kChannelRemoveNodeType = "bloom.channel-remove";
inline constexpr std::uint32_t kChannelNodeSchemaVersion = 1;
// Every channel node reads its primary set from `channels` and publishes a set on `channels`.
inline constexpr std::string_view kChannelSetPort = "channels";
inline constexpr std::string_view kChannelCopyFromPort = "from";
inline constexpr std::string_view kChannelShuffleImagePort = "image";

// Shuffle mapping encodings (ADR 0026 §4). Colour converts the RGB triple from the channel set's
// input colour space to the working space; Raw places the values in working-space RGB unchanged.
// Auto (#480, the default of a new Shuffle or Pass) is Raw when a mapped red, green or blue channel
// is a data channel (depth, normals, motion, position, UV, IDs, any Data-encoded channel) and
// Colour otherwise. Colour on a data channel is refused with a diagnostic.
inline constexpr std::int64_t kChannelShuffleColourMode = 0;
inline constexpr std::int64_t kChannelShuffleRawMode = 1;
inline constexpr std::int64_t kChannelShuffleAutoMode = 2;
// #480: the finite value a non-finite (NaN or infinite) sample becomes, for example a far depth.
// Optional, so a Shuffle saved before it reads 0, the value Bloom always used.
inline constexpr std::string_view kChannelNonFiniteParameterRole = "nonFinite";

// #480 Pass: one render pass of a channel set as an image. `pass` names a channel (a grey image) or
// a layer (its colour or vector channels as RGB); empty reads the first data layer. Alpha is 1 and
// the colour is not associated, so data values arrive unchanged.
inline constexpr std::string_view kChannelPassNodeType = "bloom.channel-pass";
inline constexpr std::string_view kChannelPassImagePort = "image";
inline constexpr std::string_view kChannelPassParameterRole = "pass";
inline constexpr std::string_view kChannelPassParameterSchemaKey = "bloom.channel-pass.pass";

// #480 Image to Channel Set (amends ADR 0026 §1): an image's stored red, green, blue and alpha as
// named channels of one layer, so a processed pass can be merged with Channel Copy and written to
// layered OpenEXR. `channels` names the image's channels in order (one to four names).
inline constexpr std::string_view kImageToChannelSetNodeType = "bloom.image-to-channel-set";
inline constexpr std::string_view kImageToChannelSetImagePort = "image";
inline constexpr std::string_view kImageToChannelSetLayerParameterRole = "layer";
inline constexpr std::string_view kImageToChannelSetChannelsParameterRole = "channels";
inline constexpr std::string_view kImageToChannelSetEncodingParameterRole = "encoding";
inline constexpr std::int64_t kImageToChannelSetDataEncoding = 0;
inline constexpr std::int64_t kImageToChannelSetColourEncoding = 1;

inline constexpr std::string_view kCompositionOutputChannelsInputPort = "channels";
inline constexpr std::string_view kCompositionOutputLayersParameterRole = "layers";
inline constexpr std::string_view kCompositionOutputLayersParameterSchemaKey =
    "bloom.composition-output.layers";

inline constexpr std::size_t kMaximumChannelNameBytes = 256;
inline constexpr std::size_t kMaximumChannelPatternBytes = 4096;

// #364 Cryptomatte (compatible with the common compositing readers): mattes picked by name from a
// Cryptomatte layer of the connected channel set. `mask` is the ADR 0028 Mask (premultiplied white
// x coverage); `image` is the optional input image multiplied by coverage (matte in alpha), or the
// matte as a white image.
inline constexpr std::string_view kCryptomatteNodeType = "bloom.cryptomatte";
inline constexpr std::uint32_t kCryptomatteNodeSchemaVersion = 1;
inline constexpr std::string_view kCryptomatteImageInputPort = "image";
inline constexpr std::string_view kCryptomatteMaskOutputPort = "mask";
inline constexpr std::string_view kCryptomatteImageOutputPort = "image";
// #364: not a socket. A Viewer inspection of this port shows the ID-colour preview (every object
// in its own colour, the selected mattes brightened) over the input image.
inline constexpr std::string_view kCryptomattePreviewInspectionPort = "preview";
inline constexpr std::string_view kCryptomatteLayerParameterRole = "layer";
inline constexpr std::string_view kCryptomatteLayerParameterSchemaKey = "bloom.cryptomatte.layer";
inline constexpr std::string_view kCryptomatteMattesParameterRole = "mattes";
inline constexpr std::string_view kCryptomatteMattesParameterSchemaKey = "bloom.cryptomatte.mattes";
inline constexpr std::size_t kMaximumCryptomatteMatteListBytes = 65536;

[[nodiscard]] NodeDefinition cryptomatteDefinition();

[[nodiscard]] NodeDefinition channelShuffleDefinition();
[[nodiscard]] NodeDefinition channelCopyDefinition();
[[nodiscard]] NodeDefinition channelRemoveDefinition();
[[nodiscard]] NodeDefinition channelPassDefinition();
[[nodiscard]] NodeDefinition imageToChannelSetDefinition();

// #480: the Image to Channel Set `channels` list split on commas with surrounding blanks trimmed,
// or nullopt when it is not one to four distinct, non-empty names without a '.'.
[[nodiscard]] std::optional<std::vector<std::string>>
splitImageToChannelSetNames(std::string_view list);

// Whether `schemaKey` is one of the channel nodes' parameter schemas, and whether `value` is a
// constant inside its domain.
[[nodiscard]] bool isChannelParameterSchemaKey(std::string_view schemaKey) noexcept;
[[nodiscard]] bool isChannelParameterValueValid(std::string_view schemaKey,
                                                const ParameterValue& value) noexcept;

} // namespace bloom::document

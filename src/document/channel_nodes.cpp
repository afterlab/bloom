#include <bloom/document/channel_nodes.hpp>

#include <bloom/core/utf8.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace bloom::document {
namespace {

[[nodiscard]] bool boundedText(const ParameterValue& value, const std::size_t limit) noexcept {
    const auto* text = std::get_if<std::string>(&value);
    return text != nullptr && text->size() <= limit && text->find('\0') == std::string::npos &&
           core::isValidUtf8(*text);
}

[[nodiscard]] ParameterDefinition text(const std::string_view typeId, std::string role,
                                       std::string value) {
    auto schemaKey = std::string(typeId) + "." + role;
    return {std::move(role), std::move(schemaKey), ParameterValueKind::String, true, false,
            std::move(value)};
}

} // namespace

NodeDefinition cryptomatteDefinition() {
    NodeDefinition node{{std::string(kCryptomatteNodeType), kCryptomatteNodeSchemaVersion},
                        NodeLoweringKind::Cryptomatte,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true},
                         {std::string(kCryptomatteImageInputPort), SocketValueKind::Image, false}},
                        {{std::string(kCryptomatteMaskOutputPort), SocketValueKind::Mask},
                         {std::string(kCryptomatteImageOutputPort), SocketValueKind::Image}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    // The Cryptomatte layer type name (`CryptoObject`, `CryptoMaterial`, `CryptoAsset`, ...). Empty
    // (the default, #487) reads the first Cryptomatte layer the channel set carries.
    node.parameters.push_back({std::string(kCryptomatteLayerParameterRole),
                               std::string(kCryptomatteLayerParameterSchemaKey),
                               ParameterValueKind::String, true, false, std::string{}});
    // The matte list: comma-separated names (a literal comma is written `\,`), or `<hex>` raw IDs.
    node.parameters.push_back({std::string(kCryptomatteMattesParameterRole),
                               std::string(kCryptomatteMattesParameterSchemaKey),
                               ParameterValueKind::String, true, false, std::string{}});
    return node;
}

NodeDefinition channelShuffleDefinition() {
    const auto type = kChannelShuffleNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true}},
                        {{std::string(kChannelShuffleImagePort), SocketValueKind::Image},
                         {std::string(kChannelSetPort), SocketValueKind::ChannelSet}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    // A slot names a source channel, or the constants "0" and "1". An empty slot (the default,
    // #487) reads the input's default colour layer: the root R/G/B when present, else the first
    // layer carrying R, G and B -- so a rootless multi-layer render shuffles its beauty pass.
    node.parameters.push_back(text(type, "red", ""));
    node.parameters.push_back(text(type, "green", ""));
    node.parameters.push_back(text(type, "blue", ""));
    node.parameters.push_back(text(type, "alpha", "1"));
    node.parameters.push_back({"mode", std::string(type) + ".mode", ParameterValueKind::Integer,
                               true, false, kChannelShuffleAutoMode});
    // Whether the mapped RGB is already associated (premultiplied) with the mapped alpha, as
    // OpenEXR stores it; straight sources such as PNG set it off.
    node.parameters.push_back({"associated", std::string(type) + ".associated",
                               ParameterValueKind::Boolean, true, false, true});
    // The layer the `channels` output names its R, G, B and A in; empty is the root layer.
    node.parameters.push_back(text(type, "layer", ""));
    node.parameters.push_back({std::string(kChannelNonFiniteParameterRole),
                               std::string(type) + ".non-finite", ParameterValueKind::Float64,
                               false, false, 0.0});
    return node;
}

NodeDefinition channelPassDefinition() {
    const auto type = kChannelPassNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true}},
                        {{std::string(kChannelPassImagePort), SocketValueKind::Image}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    node.parameters.push_back(text(type, std::string(kChannelPassParameterRole), ""));
    node.parameters.push_back({"mode", std::string(type) + ".mode", ParameterValueKind::Integer,
                               true, false, kChannelShuffleAutoMode});
    node.parameters.push_back({std::string(kChannelNonFiniteParameterRole),
                               std::string(type) + ".non-finite", ParameterValueKind::Float64,
                               true, false, 0.0});
    return node;
}

NodeDefinition imageToChannelSetDefinition() {
    const auto type = kImageToChannelSetNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kImageToChannelSetImagePort), SocketValueKind::Image, true}},
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    node.parameters.push_back(text(type, std::string(kImageToChannelSetLayerParameterRole), "pass"));
    node.parameters.push_back(
        text(type, std::string(kImageToChannelSetChannelsParameterRole), "R,G,B,A"));
    node.parameters.push_back({std::string(kImageToChannelSetEncodingParameterRole),
                               std::string(type) + ".encoding", ParameterValueKind::Integer, true,
                               false, kImageToChannelSetDataEncoding});
    return node;
}

std::optional<std::vector<std::string>> splitImageToChannelSetNames(const std::string_view list) {
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto comma = list.find(',', start);
        auto token = list.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                         : comma - start);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);
        if (token.empty() || token.find('.') != std::string_view::npos || names.size() == 4)
            return std::nullopt;
        for (const auto& existing : names)
            if (existing == token)
                return std::nullopt;
        names.emplace_back(token);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    if (names.empty())
        return std::nullopt;
    return names;
}

NodeDefinition channelCopyDefinition() {
    const auto type = kChannelCopyNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true},
                         {std::string(kChannelCopyFromPort), SocketValueKind::ChannelSet, true}},
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    // Comma-separated channel patterns (names, `layer.*` globs or `@role`) copied from `from`.
    node.parameters.push_back(text(type, "select", "*"));
    return node;
}

NodeDefinition channelRemoveDefinition() {
    const auto type = kChannelRemoveNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true}},
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    node.parameters.push_back(text(type, "select", ""));
    // Keep only the matching channels instead of removing them.
    node.parameters.push_back({"keep", std::string(type) + ".keep", ParameterValueKind::Boolean,
                               true, false, false});
    return node;
}

bool isChannelParameterSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kImagePartParameterSchemaKey ||
           schemaKey == kImageLayerParameterSchemaKey ||
           schemaKey == kCompositionOutputLayersParameterSchemaKey ||
           schemaKey.starts_with("bloom.channel-shuffle.") ||
           schemaKey == "bloom.channel-copy.select" || schemaKey == "bloom.channel-remove.select" ||
           schemaKey == "bloom.channel-remove.keep" ||
           schemaKey == kCryptomatteLayerParameterSchemaKey ||
           schemaKey == kCryptomatteMattesParameterSchemaKey ||
           schemaKey.starts_with("bloom.channel-pass.") ||
           schemaKey.starts_with("bloom.image-to-channel-set.");
}

bool isChannelParameterValueValid(const std::string_view schemaKey,
                                  const ParameterValue& value) noexcept {
    if (schemaKey == kImagePartParameterSchemaKey || schemaKey == kImageLayerParameterSchemaKey)
        return boundedText(value, kMaximumChannelNameBytes);
    if (schemaKey == "bloom.channel-shuffle.red" || schemaKey == "bloom.channel-shuffle.green" ||
        schemaKey == "bloom.channel-shuffle.blue" || schemaKey == "bloom.channel-shuffle.alpha" ||
        schemaKey == "bloom.channel-shuffle.layer")
        return boundedText(value, kMaximumChannelNameBytes);
    if (schemaKey == "bloom.channel-shuffle.mode" || schemaKey == "bloom.channel-pass.mode") {
        const auto* mode = std::get_if<std::int64_t>(&value);
        return mode != nullptr && (*mode == kChannelShuffleColourMode ||
                                   *mode == kChannelShuffleRawMode ||
                                   *mode == kChannelShuffleAutoMode);
    }
    if (schemaKey == "bloom.channel-shuffle.non-finite" ||
        schemaKey == "bloom.channel-pass.non-finite") {
        const auto* number = std::get_if<double>(&value);
        // A Float32 sample: finite, and finite once rounded to Float32.
        return number != nullptr && std::isfinite(*number) &&
               std::abs(*number) <= static_cast<double>(std::numeric_limits<float>::max());
    }
    if (schemaKey == kChannelPassParameterSchemaKey ||
        schemaKey == "bloom.image-to-channel-set.layer")
        return boundedText(value, kMaximumChannelNameBytes);
    if (schemaKey == "bloom.image-to-channel-set.channels") {
        const auto* text = std::get_if<std::string>(&value);
        return boundedText(value, kMaximumChannelNameBytes) && text != nullptr &&
               splitImageToChannelSetNames(*text).has_value();
    }
    if (schemaKey == "bloom.image-to-channel-set.encoding") {
        const auto* encoding = std::get_if<std::int64_t>(&value);
        return encoding != nullptr && (*encoding == kImageToChannelSetDataEncoding ||
                                       *encoding == kImageToChannelSetColourEncoding);
    }
    if (schemaKey == "bloom.channel-shuffle.associated" || schemaKey == "bloom.channel-remove.keep")
        return std::holds_alternative<bool>(value);
    if (schemaKey == kCryptomatteLayerParameterSchemaKey)
        return boundedText(value, kMaximumChannelNameBytes);
    if (schemaKey == kCryptomatteMattesParameterSchemaKey)
        return boundedText(value, kMaximumCryptomatteMatteListBytes);
    if (schemaKey == "bloom.channel-copy.select" || schemaKey == "bloom.channel-remove.select" ||
        schemaKey == kCompositionOutputLayersParameterSchemaKey)
        return boundedText(value, kMaximumChannelPatternBytes);
    return false;
}

} // namespace bloom::document

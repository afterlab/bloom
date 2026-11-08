#include <bloom/document/depth_effect_nodes.hpp>

#include <bloom/document/asset.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/image_channel_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_set>
#include <variant>

namespace bloom::document {

namespace {

using Kind = DepthParameterKind;

ParameterValueKind valueKindOf(const Kind kind) noexcept {
    switch (kind) {
    case Kind::Choice:
    case Kind::Integer:
        return ParameterValueKind::Integer;
    case Kind::Boolean:
        return ParameterValueKind::Boolean;
    case Kind::Scalar:
        return ParameterValueKind::Float64;
    case Kind::Vector2:
        return ParameterValueKind::Vec2d;
    case Kind::Color:
        return ParameterValueKind::Color4d;
    case Kind::Curve:
        return ParameterValueKind::Path;
    case Kind::Text:
        return ParameterValueKind::String;
    }
    return ParameterValueKind::Float64;
}

ParameterValue defaultValueOf(const DepthParameterSpec& spec) {
    switch (spec.kind) {
    case Kind::Choice:
    case Kind::Integer:
        return static_cast<std::int64_t>(spec.value[0]);
    case Kind::Boolean:
        return spec.value[0] != 0.0;
    case Kind::Scalar:
        return spec.value[0];
    case Kind::Vector2:
        return Vec2d{spec.value[0], spec.value[1]};
    case Kind::Color:
        return core::Color4d{spec.value[0], spec.value[1], spec.value[2], spec.value[3]};
    case Kind::Curve:
        return defaultDepthFogCurve();
    case Kind::Text:
        return std::string{};
    }
    return 0.0;
}

template <typename T>
const T* constantOf(const Composition& composition, const NodeRecord& node,
                    const std::string_view role) {
    const auto binding = std::ranges::find(node.parameters, role, &ParameterBinding::role);
    if (binding == node.parameters.end())
        return nullptr;
    const auto* record = composition.parameters().find(binding->parameterId);
    if (record == nullptr)
        return nullptr;
    const auto* constant = std::get_if<ConstantValueSource>(&record->source);
    return constant == nullptr ? nullptr : std::get_if<T>(&constant->value);
}

const EdgeRecord* edgeInto(const CanonicalGraph& graph, const NodeId node,
                           const std::string_view port) {
    for (const auto& edge : graph.edges()) {
        const auto* destination = std::get_if<NodeInputRef>(&edge.destination);
        if (destination != nullptr && destination->nodeId == node && destination->port == port)
            return &edge;
    }
    return nullptr;
}

// The input a value-keeping node (a Reroute or a channel node) reads its values from.
std::optional<std::string_view> keptInput(const NodeRecord& node) {
    if (isRerouteNodeType(node.typeId))
        return kValuePortName;
    if (node.typeId == kChannelPassNodeType || node.typeId == kChannelShuffleNodeType ||
        node.typeId == kChannelCopyNodeType || node.typeId == kChannelRemoveNodeType)
        return kChannelSetPort;
    if (node.typeId == kImageToChannelSetNodeType)
        return kImageToChannelSetImagePort;
    if (node.typeId == kSeparateChannelsNodeType)
        return kSeparateChannelsInputPort;
    return std::nullopt;
}

} // namespace

std::string depthSchemaKey(const std::string_view typeId, const std::string_view role) {
    std::string key(typeId);
    key += '.';
    key += role;
    return key;
}

bool isDepthParameterValueValid(const std::string_view schemaKey,
                                const ParameterValue& value) noexcept {
    const auto* spec = depthParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    switch (spec->kind) {
    case Kind::Choice:
    case Kind::Integer: {
        const auto* v = std::get_if<std::int64_t>(&value);
        return v != nullptr && within(static_cast<double>(*v));
    }
    case Kind::Boolean:
        return std::holds_alternative<bool>(value);
    case Kind::Scalar: {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    case Kind::Vector2: {
        const auto* v = std::get_if<Vec2d>(&value);
        return v != nullptr && within(v->x) && within(v->y);
    }
    case Kind::Color: {
        const auto* v = std::get_if<core::Color4d>(&value);
        return v != nullptr && v->isValid() && within(v->red) && within(v->green) &&
               within(v->blue);
    }
    case Kind::Curve: {
        const auto* v = std::get_if<PathValue>(&value);
        return v != nullptr && v->anchorCount() <= kMaximumDepthCurvePoints &&
               isGradeCurveValueValid(GradeCurve::Master, *v);
    }
    case Kind::Text: {
        const auto* v = std::get_if<std::string>(&value);
        return v != nullptr && v->size() <= kMaximumDepthChannelBytes &&
               std::ranges::none_of(*v, [](const char c) {
                   return static_cast<unsigned char>(c) < 0x20U;
               });
    }
    }
    return false;
}

PathValue defaultDepthFogCurve() { return gradeCurvePath({Vec2d{0.0, 0.0}, Vec2d{1.0, 1.0}}); }

NodeDefinition depthEffectDefinition(const std::string_view typeId) {
    NodeDefinition definition{{std::string(typeId), kDepthEffectNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{std::string(kDepthEffectInputPort), SocketValueKind::Image}},
                              {{std::string(kDepthEffectOutputPort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    // The depth source: an Image, a Mask or a channel set (its channel picked by name).
    InputPortDefinition depth{std::string(kDepthEffectDepthPort), SocketValueKind::Image, false};
    depth.acceptsPixelKinds = true;
    definition.inputs.push_back(std::move(depth));
    definition.inputs.push_back({std::string(typeId == kZDefocusNodeType ? kZDefocusBokehPort
                                                                         : kDepthFogImagePort),
                                 SocketValueKind::Image, false});
    for (const auto& parameter : depthEffectParameters(typeId)) {
        definition.parameters.push_back({std::string(parameter.role),
                                         depthSchemaKey(typeId, parameter.role),
                                         valueKindOf(parameter.kind), true, parameter.animated,
                                         defaultValueOf(parameter)});
        if (!parameter.animated)
            continue;
        if (parameter.kind == Kind::Scalar)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Scalar, false});
        else if (parameter.kind == Kind::Vector2)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Vector2, false});
        else if (parameter.kind == Kind::Color)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Color, false});
    }
    return definition;
}

DepthDistance readDepth(const DepthReading& reading, const double value) noexcept {
    DepthDistance result;
    if (!std::isfinite(value)) {
        result.empty = true;
        return result;
    }
    switch (reading.interpretation) {
    case kDepthInterpretationZ:
        if (value <= 0.0 || value >= kEmptyDepthThreshold) {
            result.empty = true;
            return result;
        }
        result.distance = value;
        return result;
    case kDepthInterpretationInverse:
        if (value <= 0.0) {
            result.empty = true;
            return result;
        }
        result.distance = 1.0 / value;
        return result;
    default:
        break;
    }
    double t = std::clamp(value, 0.0, 1.0);
    if (reading.invert)
        t = 1.0 - t;
    if (reading.interpretation == kDepthInterpretationMist) {
        if (reading.mistFalloff == kMistFalloffQuadratic)
            t = std::sqrt(t);
        else if (reading.mistFalloff == kMistFalloffInverseQuadratic)
            t = t * t;
    }
    result.distance = reading.nearDistance + t * (reading.farDistance - reading.nearDistance);
    return result;
}

double sceneUnitsPerMetre(const std::int64_t units) noexcept {
    switch (units) {
    case kSceneUnitCentimetres:
        return 100.0;
    case kSceneUnitMillimetres:
        return 1000.0;
    case kSceneUnitFeet:
        return 1.0 / 0.3048;
    case kSceneUnitInches:
        return 1.0 / 0.0254;
    default:
        return 1.0;
    }
}

std::optional<DepthColorTransform> depthInputColorTransform(const Project& project,
                                                            const Composition& composition,
                                                            const NodeId node) {
    const auto& graph = composition.graph();
    const auto* edge = edgeInto(graph, node, kDepthEffectDepthPort);
    std::unordered_set<std::uint64_t> visited;
    while (edge != nullptr) {
        const auto* source = graph.findNode(edge->source.nodeId);
        if (source == nullptr || !visited.insert(source->id.value()).second)
            return std::nullopt;
        const bool image = source->typeId == kImageSourceNodeType;
        if (image || source->typeId == kVideoSourceNodeType) {
            // A source's channel set carries its data channels unconverted.
            if (edge->source.port != "image")
                return std::nullopt;
            const auto* space = constantOf<std::int64_t>(composition, *source, "colorSpace");
            const auto* id = constantOf<std::string>(composition, *source,
                                                     kImageInputColorSpaceIdParameterRole);
            AssetInterpretation interpretation;
            if (const auto* asset = constantOf<std::string>(composition, *source, "asset")) {
                std::uint64_t raw = 0;
                const auto parsed = std::from_chars(asset->data(), asset->data() + asset->size(), raw);
                if (parsed.ec == std::errc{} && parsed.ptr == asset->data() + asset->size())
                    if (const auto* record = project.findAsset(AssetId::fromRaw(raw)))
                        interpretation = record->interpretation;
            }
            const auto effective = effectiveAssetInputColor(
                interpretation, space == nullptr ? 0 : *space,
                id == nullptr ? std::string_view{} : std::string_view{*id});
            if (effective.inputColorSpaceId.empty() &&
                effective.colorSpace == AssetColorSpace::Raw)
                return std::nullopt;
            if (!effective.inputColorSpaceId.empty())
                return DepthColorTransform{source->id, effective.inputColorSpaceId};
            return DepthColorTransform{source->id,
                                       effective.colorSpace == AssetColorSpace::Srgb ? "sRGB"
                                       : effective.colorSpace == AssetColorSpace::Linear
                                           ? "Linear"
                                           : "Automatic"};
        }
        const auto upstream = keptInput(*source);
        if (!upstream)
            return std::nullopt;
        edge = edgeInto(graph, source->id, *upstream);
    }
    return std::nullopt;
}

} // namespace bloom::document

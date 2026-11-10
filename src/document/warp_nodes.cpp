#include <bloom/document/warp_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string warpSchemaKey(const std::string_view nodeType, const std::string_view role) {
    std::string key(nodeType);
    key += '.';
    key += role;
    return key;
}

bool isWarpParameterValueValid(const std::string_view schemaKey, const ParameterValue& value) noexcept {
    if (isWarpPointSchemaKey(schemaKey)) {
        const auto* corner = std::get_if<Vec2d>(&value);
        return corner != nullptr && std::isfinite(corner->x) && std::isfinite(corner->y) &&
               std::abs(corner->x) <= kMaximumCornerPinOffset &&
               std::abs(corner->y) <= kMaximumCornerPinOffset;
    }
    const auto* spec = warpParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    if (spec->kind == WarpParameterKind::Scalar) {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) && *number >= spec->minimum &&
               *number <= spec->maximum;
    }
    if (spec->kind == WarpParameterKind::Boolean)
        return std::holds_alternative<bool>(value);
    const auto* choice = std::get_if<std::int64_t>(&value);
    return choice != nullptr && static_cast<double>(*choice) >= spec->minimum &&
           static_cast<double>(*choice) <= spec->maximum;
}

namespace {

// The definition every warp shares: the parameters of `table` (a socket for each Scalar), under
// the schema keys of `nodeType`.
NodeDefinition warpDefinition(const std::string_view nodeType,
                              const std::span<const WarpParameterSpec> table, const bool mapInput) {
    std::vector<InputPortDefinition> inputs{{"input", SocketValueKind::Image}};
    if (mapInput)
        inputs.push_back({std::string(kWarpMapPort), SocketValueKind::Image, false});
    NodeDefinition definition{{std::string(nodeType), kWarpNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              std::move(inputs),
                              {{std::string(kWarpImagePort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Transform};
    for (const auto& parameter : table) {
        if (parameter.kind == WarpParameterKind::Boolean) {
            definition.parameters.push_back({std::string(parameter.role),
                                             warpSchemaKey(nodeType, parameter.role),
                                             ParameterValueKind::Boolean, true, false,
                                             ParameterValue{parameter.value != 0.0}});
            continue;
        }
        const bool scalar = parameter.kind == WarpParameterKind::Scalar;
        definition.parameters.push_back(
            {std::string(parameter.role), warpSchemaKey(nodeType, parameter.role),
             scalar ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true, scalar,
             scalar ? ParameterValue{parameter.value}
                    : ParameterValue{static_cast<std::int64_t>(parameter.value)}});
        if (scalar)
            definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace

NodeDefinition cornerPinDefinition() {
    auto definition = warpDefinition(kCornerPinNodeType, cornerPinParameters(), false);
    for (const auto role : cornerPinCornerRoles()) {
        definition.parameters.push_back({std::string(role),
                                         warpSchemaKey(kCornerPinNodeType, role),
                                         ParameterValueKind::Vec2d, true, true, Vec2d{}});
        definition.inputs.push_back({std::string(role), SocketValueKind::Vector2, false});
    }
    return definition;
}

NodeDefinition stabilizeDefinition() {
    auto definition = warpDefinition(kStabilizeNodeType, stabilizeParameters(), false);
    for (const auto role : stabilizePointRoles()) {
        definition.parameters.push_back({std::string(role), warpSchemaKey(kStabilizeNodeType, role),
                                         ParameterValueKind::Vec2d, true, true, Vec2d{}});
        definition.inputs.push_back({std::string(role), SocketValueKind::Vector2, false});
    }
    return definition;
}

NodeDefinition tileMirrorDefinition() {
    auto definition = warpDefinition(kTileMirrorNodeType, tileMirrorParameters(), false);
    for (const auto role : tileMirrorPointRoles()) {
        definition.parameters.push_back({std::string(role), warpSchemaKey(kTileMirrorNodeType, role),
                                         ParameterValueKind::Vec2d, true, true, Vec2d{}});
        definition.inputs.push_back({std::string(role), SocketValueKind::Vector2, false});
    }
    return definition;
}

NodeDefinition proceduralWarpDefinition() {
    auto definition = warpDefinition(kProceduralWarpNodeType, proceduralWarpParameters(), false);
    for (const auto role : proceduralWarpPointRoles()) {
        definition.parameters.push_back({std::string(role),
                                         warpSchemaKey(kProceduralWarpNodeType, role),
                                         ParameterValueKind::Vec2d, true, true, Vec2d{}});
        definition.inputs.push_back({std::string(role), SocketValueKind::Vector2, false});
    }
    return definition;
}

NodeDefinition stMapDefinition() {
    return warpDefinition(kStMapNodeType, stMapParameters(), true);
}

NodeDefinition displaceDefinition() {
    return warpDefinition(kDisplaceNodeType, displaceParameters(), true);
}

} // namespace bloom::document

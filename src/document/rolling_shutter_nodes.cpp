#include <bloom/document/rolling_shutter_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string rollingShutterSchemaKey(const std::string_view role) {
    std::string key(kRollingShutterNodeType);
    key += '.';
    key += role;
    return key;
}

bool isRollingShutterParameterValueValid(const std::string_view schemaKey,
                                         const ParameterValue& value) noexcept {
    if (schemaKey == kRollingShutterCameraOffsetSchemaKey) {
        const auto* offset = std::get_if<Vec2d>(&value);
        return offset != nullptr && std::isfinite(offset->x) && std::isfinite(offset->y) &&
               std::abs(offset->x) <= kMaximumRollingShutterCameraReach &&
               std::abs(offset->y) <= kMaximumRollingShutterCameraReach;
    }
    if (schemaKey == kRollingShutterCameraRollSchemaKey) {
        const auto* roll = std::get_if<double>(&value);
        return roll != nullptr && std::isfinite(*roll) &&
               isRollingShutterScalarWithinDomain(schemaKey, *roll);
    }
    const auto* spec = rollingShutterParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    if (spec->kind == RollingShutterParameterKind::Scalar) {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) && *number >= spec->minimum &&
               *number <= spec->maximum;
    }
    const auto* choice = std::get_if<std::int64_t>(&value);
    return choice != nullptr && static_cast<double>(*choice) >= spec->minimum &&
           static_cast<double>(*choice) <= spec->maximum;
}

NodeDefinition rollingShutterDefinition() {
    NodeDefinition definition{{std::string(kRollingShutterNodeType), kRollingShutterNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image},
                               {std::string(kRollingShutterMotionPort), SocketValueKind::Image, false}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Transform};
    for (const auto& parameter : rollingShutterParameters()) {
        const bool scalar = parameter.kind == RollingShutterParameterKind::Scalar;
        definition.parameters.push_back(
            {std::string(parameter.role), rollingShutterSchemaKey(parameter.role),
             scalar ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true,
             !parameter.constant,
             scalar ? ParameterValue{parameter.value}
                    : ParameterValue{static_cast<std::int64_t>(parameter.value)}});
        if (!parameter.constant)
            definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    definition.parameters.push_back({std::string(kRollingShutterCameraOffsetRole),
                                     std::string(kRollingShutterCameraOffsetSchemaKey),
                                     ParameterValueKind::Vec2d, true, true, Vec2d{}});
    definition.inputs.push_back(
        {std::string(kRollingShutterCameraOffsetRole), SocketValueKind::Vector2, false});
    definition.parameters.push_back({std::string(kRollingShutterCameraRollRole),
                                     std::string(kRollingShutterCameraRollSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{0.0}});
    definition.inputs.push_back(
        {std::string(kRollingShutterCameraRollRole), SocketValueKind::Scalar, false});
    return definition;
}

} // namespace bloom::document

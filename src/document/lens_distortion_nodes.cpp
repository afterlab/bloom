#include <bloom/document/lens_distortion_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string lensDistortionSchemaKey(const std::string_view role) {
    std::string key(kLensDistortionNodeType);
    key += '.';
    key += role;
    return key;
}

bool isLensDistortionParameterValueValid(const std::string_view schemaKey,
                                         const ParameterValue& value) noexcept {
    if (schemaKey == kLensDistortionCenterSchemaKey) {
        const auto* centre = std::get_if<Vec2d>(&value);
        return centre != nullptr && std::isfinite(centre->x) && std::isfinite(centre->y) &&
               std::abs(centre->x) <= kMaximumLensDistortionCenter &&
               std::abs(centre->y) <= kMaximumLensDistortionCenter;
    }
    const auto* spec = lensDistortionParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    if (spec->kind == LensDistortionParameterKind::Scalar) {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) && *number >= spec->minimum &&
               *number <= spec->maximum;
    }
    const auto* choice = std::get_if<std::int64_t>(&value);
    return choice != nullptr && static_cast<double>(*choice) >= spec->minimum &&
           static_cast<double>(*choice) <= spec->maximum;
}

NodeDefinition lensDistortionDefinition() {
    NodeDefinition definition{{std::string(kLensDistortionNodeType), kLensDistortionNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{std::string(kLensDistortionImagePort), SocketValueKind::Image},
                               {std::string(kLensDistortionStMapPort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Transform};
    for (const auto& parameter : lensDistortionParameters()) {
        const bool scalar = parameter.kind == LensDistortionParameterKind::Scalar;
        definition.parameters.push_back(
            {std::string(parameter.role), lensDistortionSchemaKey(parameter.role),
             scalar ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true, scalar,
             scalar ? ParameterValue{parameter.value}
                    : ParameterValue{static_cast<std::int64_t>(parameter.value)}});
        if (scalar)
            definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    definition.parameters.push_back({"center", std::string(kLensDistortionCenterSchemaKey),
                                     ParameterValueKind::Vec2d, true, true, Vec2d{}});
    definition.inputs.push_back({"center", SocketValueKind::Vector2, false});
    return definition;
}

} // namespace bloom::document

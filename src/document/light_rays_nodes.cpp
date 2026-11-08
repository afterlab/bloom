#include <bloom/document/light_rays_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

bool isLightRaysParameterValueValid(const std::string_view schemaKey,
                                    const ParameterValue& value) noexcept {
    if (schemaKey == kLightRaysCenterSchemaKey) {
        const auto* center = std::get_if<Vec2d>(&value);
        return center != nullptr && std::isfinite(center->x) && std::isfinite(center->y) &&
               std::abs(center->x) <= kMaximumLightRaysCenter &&
               std::abs(center->y) <= kMaximumLightRaysCenter;
    }
    if (schemaKey == kLightRaysSourceSchemaKey) {
        const auto* choice = std::get_if<std::int64_t>(&value);
        return choice != nullptr && (*choice == kLightRaysFromLuminance ||
                                     *choice == kLightRaysFromAlpha);
    }
    if (schemaKey == kLightRaysColorSchemaKey) {
        const auto* color = std::get_if<core::Color4d>(&value);
        const auto within = [](const double v) {
            return std::isfinite(v) && v >= 0.0 && v <= kMaximumLightRaysColor;
        };
        return color != nullptr && color->isValid() && within(color->red) &&
               within(color->green) && within(color->blue);
    }
    if (isLightRaysScalarSchemaKey(schemaKey)) {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) &&
               isLightRaysScalarWithinDomain(schemaKey, *number);
    }
    return false;
}

NodeDefinition lightRaysDefinition() {
    NodeDefinition definition{
        {std::string(kLightRaysNodeType), kLightRaysNodeSchemaVersion},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Image},
         {"center", SocketValueKind::Vector2, false},
         {"length", SocketValueKind::Scalar, false},
         {"falloff", SocketValueKind::Scalar, false},
         {"boost", SocketValueKind::Scalar, false}},
        {{"image", SocketValueKind::Image}},
        {},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Compositing};
    definition.parameters.push_back({"source", std::string(kLightRaysSourceSchemaKey),
                                     ParameterValueKind::Integer, true, false,
                                     ParameterValue{static_cast<std::int64_t>(kLightRaysFromLuminance)}});
    definition.parameters.push_back({"center", std::string(kLightRaysCenterSchemaKey),
                                     ParameterValueKind::Vec2d, true, true, Vec2d{}});
    definition.parameters.push_back({"length", std::string(kLightRaysLengthSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kLightRaysLengthDefault}});
    definition.parameters.push_back({"falloff", std::string(kLightRaysFalloffSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kLightRaysFalloffDefault}});
    definition.parameters.push_back({"boost", std::string(kLightRaysBoostSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kLightRaysBoostDefault}});
    definition.parameters.push_back({"tint", std::string(kLightRaysColorSchemaKey),
                                     ParameterValueKind::Color4d, true, false,
                                     ParameterValue{core::Color4d{1.0, 1.0, 1.0, 1.0}}});
    return definition;
}

} // namespace bloom::document

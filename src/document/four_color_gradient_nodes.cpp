#include <bloom/document/four_color_gradient_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <string>
#include <variant>

namespace bloom::document {

bool isFourColorGradientParameterValueValid(const std::string_view schemaKey,
                                            const ParameterValue& value) noexcept {
    if (isFourColorGradientPointSchemaKey(schemaKey)) {
        const auto* point = std::get_if<Vec2d>(&value);
        return point != nullptr && std::isfinite(point->x) && std::isfinite(point->y) &&
               std::abs(point->x) <= kMaximumFourColorGradientPoint &&
               std::abs(point->y) <= kMaximumFourColorGradientPoint;
    }
    if (isFourColorGradientColorSchemaKey(schemaKey)) {
        const auto* color = std::get_if<core::Color4d>(&value);
        const auto within = [](const double v) {
            return std::isfinite(v) && v >= 0.0 && v <= kMaximumFourColorGradientColor;
        };
        return color != nullptr && color->isValid() && within(color->red) &&
               within(color->green) && within(color->blue);
    }
    if (isFourColorGradientScalarSchemaKey(schemaKey)) {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) &&
               isFourColorGradientBlendWithinDomain(*number);
    }
    return false;
}

NodeDefinition fourColorGradientDefinition() {
    NodeDefinition definition{{std::string(kFourColorGradientNodeType),
                               kFourColorGradientNodeSchemaVersion},
                              NodeLoweringKind::FourColorGradient,
                              {},
                              {{std::string(kFourColorGradientImagePort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Sources};
    // The corners of a 1920 by 1080 frame pulled in to half way: the points sit inside the frame
    // for any composition of that shape or larger.
    constexpr std::array<Vec2d, 4> points{Vec2d{-480.0, 270.0}, Vec2d{480.0, 270.0},
                                          Vec2d{-480.0, -270.0}, Vec2d{480.0, -270.0}};
    constexpr std::array<core::Color4d, 4> colors{
        core::Color4d{1.0, 0.2, 0.2, 1.0}, core::Color4d{1.0, 0.8, 0.1, 1.0},
        core::Color4d{0.1, 0.5, 1.0, 1.0}, core::Color4d{0.2, 0.9, 0.4, 1.0}};
    for (std::size_t i = 0; i < kFourColorGradientPoints; ++i) {
        definition.inputs.push_back(
            {std::string(kFourColorGradientPointRoles[i]), SocketValueKind::Vector2, false});
    }
    for (std::size_t i = 0; i < kFourColorGradientPoints; ++i) {
        definition.inputs.push_back(
            {std::string(kFourColorGradientColorRoles[i]), SocketValueKind::Color, false});
    }
    definition.inputs.push_back({"blend", SocketValueKind::Scalar, false});
    for (std::size_t i = 0; i < kFourColorGradientPoints; ++i) {
        definition.parameters.push_back({std::string(kFourColorGradientPointRoles[i]),
                                         std::string(kFourColorGradientPointSchemaKeys[i]),
                                         ParameterValueKind::Vec2d, true, true,
                                         ParameterValue{points[i]}});
    }
    for (std::size_t i = 0; i < kFourColorGradientPoints; ++i) {
        definition.parameters.push_back({std::string(kFourColorGradientColorRoles[i]),
                                         std::string(kFourColorGradientColorSchemaKeys[i]),
                                         ParameterValueKind::Color4d, true, true,
                                         ParameterValue{colors[i]}});
    }
    definition.parameters.push_back({"blend", std::string(kFourColorGradientBlendSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kFourColorGradientBlendDefault}});
    return definition;
}

} // namespace bloom::document

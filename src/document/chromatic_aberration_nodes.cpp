#include <bloom/document/chromatic_aberration_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

bool isChromaticAberrationParameterValueValid(const std::string_view schemaKey,
                                              const ParameterValue& value) noexcept {
    if (schemaKey == kChromaticAberrationCenterSchemaKey) {
        const auto* centre = std::get_if<Vec2d>(&value);
        return centre != nullptr && std::isfinite(centre->x) && std::isfinite(centre->y) &&
               std::abs(centre->x) <= kMaximumChromaticAberrationCenter &&
               std::abs(centre->y) <= kMaximumChromaticAberrationCenter;
    }
    if (schemaKey == kChromaticAberrationAmountSchemaKey) {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) &&
               isChromaticAberrationScalarWithinDomain(schemaKey, *number);
    }
    return false;
}

NodeDefinition chromaticAberrationDefinition() {
    NodeDefinition definition{
        {std::string(kChromaticAberrationNodeType), kChromaticAberrationNodeSchemaVersion},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Image},
         {"amount", SocketValueKind::Scalar, false},
         {"center", SocketValueKind::Vector2, false}},
        {{"image", SocketValueKind::Image}},
        {},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Transform};
    definition.parameters.push_back({"amount", std::string(kChromaticAberrationAmountSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kChromaticAberrationAmountDefault}});
    definition.parameters.push_back({"center", std::string(kChromaticAberrationCenterSchemaKey),
                                     ParameterValueKind::Vec2d, true, true, Vec2d{}});
    return definition;
}

} // namespace bloom::document

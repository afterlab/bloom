#include <bloom/document/denoise_definition.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

bool isDenoiseParameterValueValid(const std::string_view schemaKey,
                                  const ParameterValue& value) noexcept {
    if (schemaKey == kDenoiseModeSchemaKey) {
        const auto* mode = std::get_if<std::int64_t>(&value);
        return mode != nullptr && *mode >= 0 && *mode < kDenoiseModeCount;
    }
    if (schemaKey == kDenoiseFramesSchemaKey) {
        const auto* frames = std::get_if<std::int64_t>(&value);
        return frames != nullptr && *frames >= kMinimumDenoiseFrames &&
               *frames <= kMaximumDenoiseFrames;
    }
    if (schemaKey == kDenoiseAutoStrengthSchemaKey)
        return std::holds_alternative<bool>(value);
    if (schemaKey == kDenoiseSampleFromSchemaKey || schemaKey == kDenoiseSampleToSchemaKey) {
        const auto* point = std::get_if<Vec2d>(&value);
        return point != nullptr && std::isfinite(point->x) && std::isfinite(point->y) &&
               isDenoiseScalarWithinDomain(schemaKey, point->x) &&
               isDenoiseScalarWithinDomain(schemaKey, point->y);
    }
    const auto* number = std::get_if<double>(&value);
    return number != nullptr && std::isfinite(*number) &&
           (schemaKey == kDenoiseRadiusSchemaKey || schemaKey == kDenoiseStrengthSchemaKey) &&
           isDenoiseScalarWithinDomain(schemaKey, *number);
}

NodeDefinition denoiseDefinition() {
    NodeDefinition definition{{std::string(kDenoiseNodeType), kDenoiseNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image},
                               {"radius", SocketValueKind::Scalar, false},
                               {"strength", SocketValueKind::Scalar, false}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    definition.parameters.push_back({"mode", std::string(kDenoiseModeSchemaKey),
                                     ParameterValueKind::Integer, true, false,
                                     ParameterValue{static_cast<std::int64_t>(
                                         DenoiseNodeMode::Bilateral)}});
    definition.parameters.push_back({"radius", std::string(kDenoiseRadiusSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kDefaultDenoiseRadius}});
    definition.parameters.push_back({"strength", std::string(kDenoiseStrengthSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kDefaultDenoiseStrength}});
    definition.parameters.push_back({"frames", std::string(kDenoiseFramesSchemaKey),
                                     ParameterValueKind::Integer, true, false,
                                     ParameterValue{kDefaultDenoiseFrames}});
    // Auto strength and its sample region are optional additions: a node saved without them reads
    // as Auto strength off over the default region.
    definition.parameters.push_back({"autoStrength", std::string(kDenoiseAutoStrengthSchemaKey),
                                     ParameterValueKind::Boolean, false, false,
                                     ParameterValue{false}});
    definition.parameters.push_back(
        {"sampleFrom", std::string(kDenoiseSampleFromSchemaKey), ParameterValueKind::Vec2d, false,
         false, ParameterValue{kDefaultDenoiseSampleFrom}});
    definition.parameters.push_back(
        {"sampleTo", std::string(kDenoiseSampleToSchemaKey), ParameterValueKind::Vec2d, false,
         false, ParameterValue{kDefaultDenoiseSampleTo}});
    return definition;
}

} // namespace bloom::document

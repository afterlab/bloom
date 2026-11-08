#include <bloom/document/auto_exposure_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string autoExposureSchemaKey(const std::string_view role) {
    std::string key(kAutoExposureNodeType);
    key += '.';
    key += role;
    return key;
}

bool isAutoExposureParameterValueValid(const std::string_view schemaKey,
                                       const ParameterValue& value) noexcept {
    const auto* spec = autoExposureParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    if (spec->kind == AutoExposureParameterKind::Scalar) {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    const auto* v = std::get_if<std::int64_t>(&value);
    return v != nullptr && within(static_cast<double>(*v));
}

NodeDefinition autoExposureDefinition() {
    NodeDefinition definition{{std::string(kAutoExposureNodeType), kAutoExposureNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    for (const auto& parameter : autoExposureParameters()) {
        const bool scalar = parameter.kind == AutoExposureParameterKind::Scalar;
        definition.parameters.push_back(
            {std::string(parameter.role), autoExposureSchemaKey(parameter.role),
             scalar ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true, scalar,
             scalar ? ParameterValue{parameter.value}
                    : ParameterValue{static_cast<std::int64_t>(parameter.value)}});
        if (scalar)
            definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace bloom::document

#include <bloom/document/codec_degrade_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string codecDegradeSchemaKey(const std::string_view role) {
    std::string key(kCodecDegradeNodeType);
    key += '.';
    key += role;
    return key;
}

bool isCodecDegradeParameterValueValid(const std::string_view schemaKey,
                                       const ParameterValue& value) noexcept {
    const auto* spec = codecDegradeParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    if (spec->kind == CodecDegradeParameterKind::Scalar) {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    if (spec->kind == CodecDegradeParameterKind::Boolean)
        return std::holds_alternative<bool>(value);
    const auto* v = std::get_if<std::int64_t>(&value);
    return v != nullptr && within(static_cast<double>(*v));
}

NodeDefinition codecDegradeDefinition() {
    NodeDefinition definition{{std::string(kCodecDegradeNodeType), kCodecDegradeNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    for (const auto& parameter : codecDegradeParameters()) {
        if (parameter.kind == CodecDegradeParameterKind::Boolean) {
            definition.parameters.push_back({std::string(parameter.role),
                                             codecDegradeSchemaKey(parameter.role),
                                             ParameterValueKind::Boolean, true, false,
                                             ParameterValue{parameter.value != 0.0}});
            continue;
        }
        const bool scalar = parameter.kind == CodecDegradeParameterKind::Scalar;
        definition.parameters.push_back(
            {std::string(parameter.role), codecDegradeSchemaKey(parameter.role),
             scalar ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true, scalar,
             scalar ? ParameterValue{parameter.value}
                    : ParameterValue{static_cast<std::int64_t>(parameter.value)}});
        if (scalar)
            definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace bloom::document

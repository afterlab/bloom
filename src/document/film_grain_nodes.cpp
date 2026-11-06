#include <bloom/document/film_grain_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string filmGrainSchemaKey(const std::string_view role) {
    std::string key(kFilmGrainNodeType);
    key += '.';
    key += role;
    return key;
}

bool isFilmGrainParameterValueValid(const std::string_view schemaKey,
                                    const ParameterValue& value) noexcept {
    const auto* spec = filmGrainParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    if (spec->kind == FilmGrainParameterKind::Scalar) {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    if (spec->kind == FilmGrainParameterKind::Boolean)
        return std::holds_alternative<bool>(value);
    const auto* v = std::get_if<std::int64_t>(&value);
    return v != nullptr && within(static_cast<double>(*v));
}

NodeDefinition filmGrainDefinition() {
    NodeDefinition definition{{std::string(kFilmGrainNodeType), kFilmGrainNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    for (const auto& parameter : filmGrainParameters()) {
        // Optional, so a node saved before it still validates and renders with it off.
        if (parameter.kind == FilmGrainParameterKind::Boolean) {
            definition.parameters.push_back({std::string(parameter.role),
                                             filmGrainSchemaKey(parameter.role),
                                             ParameterValueKind::Boolean, false, false,
                                             ParameterValue{parameter.value != 0.0}});
            continue;
        }
        const bool scalar = parameter.kind == FilmGrainParameterKind::Scalar;
        definition.parameters.push_back(
            {std::string(parameter.role), filmGrainSchemaKey(parameter.role),
             scalar ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true, scalar,
             scalar ? ParameterValue{parameter.value}
                    : ParameterValue{static_cast<std::int64_t>(parameter.value)}});
        if (scalar)
            definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace bloom::document

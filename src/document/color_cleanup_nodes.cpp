#include <bloom/document/color_cleanup_nodes.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

namespace {

using Kind = DepthParameterKind;

[[nodiscard]] ParameterValueKind valueKindOf(const Kind kind) noexcept {
    switch (kind) {
    case Kind::Boolean:
        return ParameterValueKind::Boolean;
    case Kind::Scalar:
        return ParameterValueKind::Float64;
    default:
        return ParameterValueKind::Integer;
    }
}

[[nodiscard]] ParameterValue defaultValueOf(const DepthParameterSpec& spec) {
    switch (spec.kind) {
    case Kind::Boolean:
        return spec.value[0] != 0.0;
    case Kind::Scalar:
        return spec.value[0];
    default:
        return static_cast<std::int64_t>(spec.value[0]);
    }
}

} // namespace

std::string colorCleanupSchemaKey(const std::string_view typeId, const std::string_view role) {
    std::string key(typeId);
    key += '.';
    key += role;
    return key;
}

bool isColorCleanupParameterValueValid(const std::string_view schemaKey,
                                       const ParameterValue& value) noexcept {
    const auto* spec = colorCleanupParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    switch (spec->kind) {
    case Kind::Choice:
    case Kind::Integer: {
        const auto* v = std::get_if<std::int64_t>(&value);
        return v != nullptr && within(static_cast<double>(*v));
    }
    case Kind::Boolean:
        return std::holds_alternative<bool>(value);
    case Kind::Scalar: {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    default:
        return false;
    }
}

NodeDefinition colorCleanupDefinition(const std::string_view typeId) {
    NodeDefinition definition{{std::string(typeId), kColorCleanupNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{std::string(kColorCleanupInputPort), SocketValueKind::Image}},
                              {{std::string(kColorCleanupImageOutputPort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Grade};
    for (const auto& parameter : colorCleanupParameters(typeId)) {
        definition.parameters.push_back({std::string(parameter.role),
                                         colorCleanupSchemaKey(typeId, parameter.role),
                                         valueKindOf(parameter.kind), true, parameter.animated,
                                         defaultValueOf(parameter)});
        if (parameter.kind == Kind::Scalar)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace bloom::document

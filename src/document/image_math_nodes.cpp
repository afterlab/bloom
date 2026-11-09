#include <bloom/document/image_math_nodes.hpp>

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

std::string imagePixelSchemaKey(const std::string_view typeId, const std::string_view role) {
    std::string key(typeId);
    key += '.';
    key += role;
    return key;
}

bool isImagePixelParameterValueValid(const std::string_view schemaKey,
                                     const ParameterValue& value) noexcept {
    const auto* spec = imagePixelParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    switch (spec->kind) {
    case Kind::Choice: {
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

NodeDefinition imagePixelDefinition(const std::string_view typeId) {
    NodeDefinition definition{{std::string(typeId), kImagePixelNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{std::string(kImagePixelInputPort), SocketValueKind::Image}},
                              {{std::string(kImagePixelOutputPort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              // Histogram and Image Compare publish an analysis image, not the input
                              // changed, so a mask over the input has nothing to select.
                              typeId == kHistogramNodeType || typeId == kImageCompareNodeType
                                  ? NodeCategory::Utilities
                                  : NodeCategory::Grade};
    if (typeId == kImageMathNodeType)
        definition.inputs.push_back(
            {std::string(kImagePixelSecondPort), SocketValueKind::Image, false});
    // #349 Image Compare has nothing to compare without its second image.
    if (typeId == kImageCompareNodeType)
        definition.inputs.push_back({std::string(kImagePixelSecondPort), SocketValueKind::Image, true});
    for (const auto& parameter : imagePixelParameters(typeId)) {
        definition.parameters.push_back({std::string(parameter.role),
                                         imagePixelSchemaKey(typeId, parameter.role),
                                         valueKindOf(parameter.kind), true, parameter.animated,
                                         defaultValueOf(parameter)});
        if (parameter.kind == Kind::Scalar)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace bloom::document

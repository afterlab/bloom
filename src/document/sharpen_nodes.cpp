#include <bloom/document/sharpen_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

static_assert(sharpen_parameters::kParameters[1].maximum == kMaximumBlurRadiusPixels);

std::string sharpenSchemaKey(const std::string_view role) {
    std::string key(kSharpenNodeType);
    key += '.';
    key += role;
    return key;
}

bool isSharpenParameterValueValid(const std::string_view schemaKey,
                                  const ParameterValue& value) noexcept {
    const auto* spec = sharpenParameterSpec(schemaKey);
    const auto* number = std::get_if<double>(&value);
    return spec != nullptr && number != nullptr && std::isfinite(*number) &&
           *number >= spec->minimum && *number <= spec->maximum;
}

NodeDefinition sharpenDefinition() {
    NodeDefinition definition{{std::string(kSharpenNodeType), kSharpenNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    for (const auto& parameter : sharpenParameters()) {
        definition.parameters.push_back({std::string(parameter.role), sharpenSchemaKey(parameter.role),
                                         ParameterValueKind::Float64, true, true,
                                         ParameterValue{parameter.value}});
        definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace bloom::document

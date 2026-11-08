#include <bloom/document/test_card_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string testCardSchemaKey(const std::string_view role) {
    std::string key(kTestCardNodeType);
    key += '.';
    key += role;
    return key;
}

bool isTestCardParameterValueValid(const std::string_view schemaKey,
                                   const ParameterValue& value) noexcept {
    const auto* spec = testCardParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    switch (spec->kind) {
    case TestCardParameterKind::Boolean:
        return std::holds_alternative<bool>(value);
    case TestCardParameterKind::Scalar: {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    case TestCardParameterKind::Choice:
    case TestCardParameterKind::Whole: {
        const auto* v = std::get_if<std::int64_t>(&value);
        return v != nullptr && within(static_cast<double>(*v));
    }
    }
    return false;
}

NodeDefinition testCardDefinition() {
    NodeDefinition definition{{std::string(kTestCardNodeType), kTestCardNodeSchemaVersion},
                              NodeLoweringKind::TestCard,
                              {},
                              {{std::string(kTestCardImagePort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Sources};
    for (const auto& parameter : testCardParameters()) {
        switch (parameter.kind) {
        case TestCardParameterKind::Boolean:
            definition.parameters.push_back({std::string(parameter.role),
                                             testCardSchemaKey(parameter.role),
                                             ParameterValueKind::Boolean, true, false,
                                             ParameterValue{parameter.value != 0.0}});
            break;
        case TestCardParameterKind::Scalar:
            definition.parameters.push_back({std::string(parameter.role),
                                             testCardSchemaKey(parameter.role),
                                             ParameterValueKind::Float64, true, false,
                                             ParameterValue{parameter.value}});
            break;
        case TestCardParameterKind::Choice:
        case TestCardParameterKind::Whole:
            definition.parameters.push_back(
                {std::string(parameter.role), testCardSchemaKey(parameter.role),
                 ParameterValueKind::Integer, true, false,
                 ParameterValue{static_cast<std::int64_t>(parameter.value)}});
            break;
        }
    }
    return definition;
}

} // namespace bloom::document

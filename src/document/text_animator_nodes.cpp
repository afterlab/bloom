// #758 Text Animator: the node's definition and document domains.
#include <bloom/document/text_animator_definition.hpp>

#include <bloom/core/utf8.hpp>

#include <cmath>
#include <string>

namespace bloom::document {

std::string textAnimatorSchemaKey(const std::string_view role) {
    return "bloom.text-animator." + std::string(role);
}

bool isTextAnimatorParameterValueValid(const std::string_view schemaKey,
                                       const ParameterValue& value) {
    const auto* spec = textAnimatorParameterSpec(schemaKey);
    if (spec == nullptr)
        return true;
    using Kind = TextAnimatorParameterKind;
    switch (spec->kind) {
    case Kind::Boolean:
        return std::holds_alternative<bool>(value);
    case Kind::Choice: {
        const auto* number = std::get_if<std::int64_t>(&value);
        return number != nullptr && *number >= 0 && static_cast<double>(*number) <= spec->maximum;
    }
    case Kind::Scalar: {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) && *number >= spec->minimum &&
               *number <= spec->maximum;
    }
    case Kind::Vec2: {
        const auto* vector = std::get_if<Vec2d>(&value);
        return vector != nullptr && std::isfinite(vector->x) && std::isfinite(vector->y) &&
               vector->x >= spec->minimum && vector->x <= spec->maximum &&
               vector->y >= spec->minimum && vector->y <= spec->maximum;
    }
    case Kind::Color: {
        const auto* color = std::get_if<core::Color4d>(&value);
        return color != nullptr && color->isValid();
    }
    case Kind::Text: {
        const auto* text = std::get_if<std::string>(&value);
        return text != nullptr && text->size() <= kTextAnimatorCursorBytes &&
               core::isValidUtf8(*text) && text->find_first_of("\r\n") == std::string::npos;
    }
    }
    return false;
}

NodeDefinition textAnimatorDefinition() {
    NodeDefinition definition{{std::string(kTextAnimatorNodeType), kTextAnimatorNodeSchemaVersion},
                              NodeLoweringKind::TextAnimator,
                              {{std::string(kTextAnimatorTextPort), SocketValueKind::Image, false}},
                              {{std::string(kTextAnimatorImagePort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Sources};
    using Kind = TextAnimatorParameterKind;
    for (const auto& spec : textAnimatorParameters()) {
        ParameterValue fallback;
        ParameterValueKind kind = ParameterValueKind::Float64;
        SocketValueKind socket = SocketValueKind::Scalar;
        switch (spec.kind) {
        case Kind::Boolean:
            kind = ParameterValueKind::Boolean;
            fallback = spec.value != 0.0;
            socket = SocketValueKind::Boolean;
            break;
        case Kind::Choice:
            kind = ParameterValueKind::Integer;
            fallback = static_cast<std::int64_t>(spec.value);
            socket = SocketValueKind::Integer;
            break;
        case Kind::Scalar:
            fallback = spec.value;
            break;
        case Kind::Vec2:
            kind = ParameterValueKind::Vec2d;
            fallback = Vec2d{spec.value, spec.value};
            socket = SocketValueKind::Vector2;
            break;
        case Kind::Color:
            kind = ParameterValueKind::Color4d;
            fallback = spec.color;
            socket = SocketValueKind::Color;
            break;
        case Kind::Text:
            kind = ParameterValueKind::String;
            fallback = std::string("|");
            socket = SocketValueKind::String;
            break;
        }
        if (spec.socketed)
            definition.inputs.push_back({std::string(spec.role), socket, false});
        definition.parameters.push_back({std::string(spec.role), textAnimatorSchemaKey(spec.role),
                                         kind, true, spec.animatable, std::move(fallback)});
    }
    return definition;
}

} // namespace bloom::document

// #752 Subtitles: the node's definition and document domains (docs/architecture/subtitles.md).
#include <bloom/document/subtitles_definition.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/font_id.hpp>

#include <cmath>
#include <string>

namespace bloom::document {

std::string subtitlesSchemaKey(const std::string_view role) {
    return "bloom.subtitles." + std::string(role);
}

core::Color4d subtitlesDefaultColor(const std::string_view role) noexcept {
    if (role == "outlineColor")
        return core::Color4d{0.0, 0.0, 0.0, 1.0};
    if (role == "boxColor")
        return core::Color4d{0.0, 0.0, 0.0, 0.6};
    return core::Color4d{1.0, 1.0, 1.0, 1.0};
}

bool isSubtitlesParameterValueValid(const std::string_view schemaKey,
                                    const ParameterValue& value) {
    const auto* spec = subtitlesParameterSpec(schemaKey);
    if (spec == nullptr)
        return true;
    using Kind = SubtitlesParameterKind;
    switch (spec->kind) {
    case Kind::Cues: {
        const auto* list = std::get_if<CueListValue>(&value);
        return list != nullptr && list->isValid();
    }
    case Kind::Font: {
        const auto* text = std::get_if<std::string>(&value);
        return text != nullptr && decodeFontId(*text).has_value();
    }
    case Kind::Scalar: {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) && *number >= spec->minimum &&
               *number <= spec->maximum;
    }
    case Kind::Whole:
    case Kind::Choice: {
        const auto* number = std::get_if<std::int64_t>(&value);
        return number != nullptr && static_cast<double>(*number) >= spec->minimum &&
               static_cast<double>(*number) <= spec->maximum;
    }
    case Kind::Boolean:
        return std::holds_alternative<bool>(value);
    case Kind::Color: {
        const auto* color = std::get_if<core::Color4d>(&value);
        return color != nullptr && color->isValid();
    }
    case Kind::Offset: {
        const auto* offset = std::get_if<Vec2d>(&value);
        return offset != nullptr && std::isfinite(offset->x) && std::isfinite(offset->y) &&
               std::abs(offset->x) <= spec->maximum && std::abs(offset->y) <= spec->maximum;
    }
    case Kind::Text: {
        const auto* text = std::get_if<std::string>(&value);
        if (text == nullptr || text->size() > kMaximumCueNameBytes || !core::isValidUtf8(*text))
            return false;
        // The language is empty (the list's own) or a language tag.
        return spec->role != "language" || text->empty() || isWellFormedLanguageTag(*text);
    }
    }
    return false;
}

NodeDefinition subtitlesDefinition() {
    NodeDefinition definition{{std::string(kSubtitlesNodeType), kSubtitlesNodeSchemaVersion},
                              NodeLoweringKind::Subtitles,
                              {{std::string(kSubtitlesCuesRole), SocketValueKind::CueList, false}},
                              {{std::string(kSubtitlesImagePort), SocketValueKind::Image},
                               {std::string(kSubtitlesCuesPort), SocketValueKind::CueList},
                               {std::string(kSubtitlesTextPort), SocketValueKind::String},
                               {std::string(kSubtitlesIndexPort), SocketValueKind::Integer},
                               {std::string(kSubtitlesProgressPort), SocketValueKind::Scalar}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Sources};
    using Kind = SubtitlesParameterKind;
    for (const auto& spec : subtitlesParameters()) {
        ParameterValue fallback;
        ParameterValueKind kind = ParameterValueKind::Float64;
        switch (spec.kind) {
        case Kind::Cues:
            kind = ParameterValueKind::CueList;
            fallback = CueListValue{};
            break;
        case Kind::Font:
            kind = ParameterValueKind::String;
            fallback = encodeFontId(defaultFontId());
            break;
        case Kind::Scalar:
            fallback = spec.value;
            break;
        case Kind::Whole:
        case Kind::Choice:
            kind = ParameterValueKind::Integer;
            fallback = static_cast<std::int64_t>(spec.value);
            break;
        case Kind::Boolean:
            kind = ParameterValueKind::Boolean;
            fallback = spec.value != 0.0;
            break;
        case Kind::Color:
            kind = ParameterValueKind::Color4d;
            fallback = subtitlesDefaultColor(spec.role);
            break;
        case Kind::Offset:
            kind = ParameterValueKind::Vec2d;
            fallback = Vec2d{};
            break;
        case Kind::Text:
            kind = ParameterValueKind::String;
            fallback = std::string{};
            break;
        }
        definition.parameters.push_back({std::string(spec.role), subtitlesSchemaKey(spec.role),
                                         kind, true, spec.animatable, std::move(fallback)});
    }
    return definition;
}

} // namespace bloom::document

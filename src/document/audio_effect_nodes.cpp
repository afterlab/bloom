#include <bloom/document/audio_effect_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <cmath>
#include <variant>

namespace bloom::document {

std::string audioEffectSchemaKey(const std::string_view typeId, const std::string_view role) {
    std::string key(typeId);
    key += '.';
    key += role;
    return key;
}

bool isAudioEffectParameterValueValid(const std::string_view schemaKey,
                                      const ParameterValue& value) noexcept {
    const auto* spec = audioEffectParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    if (spec->kind == AudioEffectParameterKind::Scalar) {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    const auto* v = std::get_if<std::int64_t>(&value);
    return v != nullptr && within(static_cast<double>(*v));
}

NodeDefinition audioEffectDefinition(const std::string_view typeId) {
    // Sources group beside the Audio source: the owner's tree (#554) has no audio effect group yet.
    NodeDefinition definition{{std::string(typeId), kAudioEffectNodeSchemaVersion},
                              NodeLoweringKind::AudioEffect,
                              {{std::string(kAudioEffectInputPort), SocketValueKind::Audio, false}},
                              {{std::string(kAudioEffectOutputPort), SocketValueKind::Audio}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Sources};
    for (const auto& parameter : audioEffectParameters(typeId)) {
        const bool scalar = parameter.kind == AudioEffectParameterKind::Scalar;
        definition.parameters.push_back(
            {std::string(parameter.role), audioEffectSchemaKey(typeId, parameter.role),
             scalar ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true, scalar,
             scalar ? ParameterValue{parameter.value}
                    : ParameterValue{static_cast<std::int64_t>(parameter.value)}});
        if (scalar)
            definition.inputs.push_back({std::string(parameter.role), SocketValueKind::Scalar, false});
    }
    return definition;
}

} // namespace bloom::document

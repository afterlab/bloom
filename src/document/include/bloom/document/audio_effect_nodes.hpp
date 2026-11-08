#pragma once

// #569 audio effect nodes: the durable vocabulary of the nodes that process an audio layer's sound.
// An effect has one Audio input and one Audio output, sits between an Audio or Video source and a
// Layer's audio input (several can follow one another), and runs on the mixer, one sample at a time,
// in playback and in export alike. Muting an effect bypasses it.
//
//   bloom.audio-equalizer   high-pass and low-pass filters and three bands: a low shelf, a peaking
//                           mid band and a high shelf.
//   bloom.audio-compressor  a feed-forward compressor with a soft knee.
//   bloom.audio-auto-level  a slow automatic gain that brings the running level toward a target.
//   bloom.audio-mono        folds the channels together.
//   bloom.audio-hiss        adds white noise, optionally high-passed.
//
// Every Scalar is animatable, drivable and has a socket of its own; the hiss seed is a constant.
// The parameter order of each node is the order of the matching role list in the media library
// (bloom/media/audio/effects.hpp), which the compiled plan relies on.
//
// docs/architecture/media-io.md, "Audio Effects", is the specification.
//
// Schema keys are "<node type>.<role>".

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kAudioEqualizerNodeType = "bloom.audio-equalizer";
inline constexpr std::string_view kAudioCompressorNodeType = "bloom.audio-compressor";
inline constexpr std::string_view kAudioAutoLevelNodeType = "bloom.audio-auto-level";
inline constexpr std::string_view kAudioMonoNodeType = "bloom.audio-mono";
inline constexpr std::string_view kAudioHissNodeType = "bloom.audio-hiss";
inline constexpr std::uint32_t kAudioEffectNodeSchemaVersion = 1;
inline constexpr std::string_view kAudioEffectInputPort = "audio";
inline constexpr std::string_view kAudioEffectOutputPort = "audio";

enum class AudioEffectParameterKind : std::uint8_t { Scalar, Count };

struct AudioEffectParameterSpec final {
    std::string_view role;
    AudioEffectParameterKind kind = AudioEffectParameterKind::Scalar;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    // The control's label and presentation: the span its slider covers, one displayed step, the
    // decimals shown, the unit and the display scale of a stored value.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 0;
    std::string_view unit{};
    double displayScale = 1.0;
};

namespace audio_effect_parameters {

using Kind = AudioEffectParameterKind;

inline constexpr std::array kEqualizer{
    AudioEffectParameterSpec{"highPass", Kind::Scalar, 20.0, 20.0, 2000.0, "High-pass", 20.0, 500.0,
                             1.0, 0, "Hz"},
    AudioEffectParameterSpec{"lowPass", Kind::Scalar, 20000.0, 1000.0, 20000.0, "Low-pass", 1000.0,
                             20000.0, 10.0, 0, "Hz"},
    AudioEffectParameterSpec{"lowGain", Kind::Scalar, 0.0, -24.0, 24.0, "Low gain", -12.0, 12.0,
                             0.1, 1, "dB"},
    AudioEffectParameterSpec{"lowFrequency", Kind::Scalar, 120.0, 20.0, 1000.0, "Low frequency",
                             40.0, 400.0, 1.0, 0, "Hz"},
    AudioEffectParameterSpec{"midGain", Kind::Scalar, 0.0, -24.0, 24.0, "Mid gain", -12.0, 12.0,
                             0.1, 1, "dB"},
    AudioEffectParameterSpec{"midFrequency", Kind::Scalar, 1000.0, 100.0, 8000.0, "Mid frequency",
                             200.0, 5000.0, 1.0, 0, "Hz"},
    AudioEffectParameterSpec{"midQ", Kind::Scalar, 1.0, 0.1, 10.0, "Mid width (Q)", 0.3, 5.0, 0.05,
                             2},
    AudioEffectParameterSpec{"highGain", Kind::Scalar, 0.0, -24.0, 24.0, "High gain", -12.0, 12.0,
                             0.1, 1, "dB"},
    AudioEffectParameterSpec{"highFrequency", Kind::Scalar, 6000.0, 1000.0, 16000.0,
                             "High frequency", 2000.0, 12000.0, 10.0, 0, "Hz"},
};

inline constexpr std::array kCompressor{
    AudioEffectParameterSpec{"threshold", Kind::Scalar, -18.0, -60.0, 0.0, "Threshold", -60.0, 0.0,
                             0.1, 1, "dB"},
    AudioEffectParameterSpec{"ratio", Kind::Scalar, 3.0, 1.0, 20.0, "Ratio", 1.0, 10.0, 0.1, 1,
                             ":1"},
    AudioEffectParameterSpec{"attack", Kind::Scalar, 10.0, 0.1, 200.0, "Attack", 0.1, 100.0, 0.1, 1,
                             "ms"},
    AudioEffectParameterSpec{"release", Kind::Scalar, 150.0, 5.0, 2000.0, "Release", 5.0, 1000.0,
                             1.0, 0, "ms"},
    AudioEffectParameterSpec{"knee", Kind::Scalar, 6.0, 0.0, 24.0, "Knee", 0.0, 24.0, 0.1, 1, "dB"},
    AudioEffectParameterSpec{"makeup", Kind::Scalar, 0.0, 0.0, 36.0, "Makeup gain", 0.0, 24.0, 0.1,
                             1, "dB"},
};

inline constexpr std::array kAutoLevel{
    AudioEffectParameterSpec{"target", Kind::Scalar, -20.0, -40.0, -6.0, "Target level", -40.0,
                             -6.0, 0.1, 1, "dB"},
    AudioEffectParameterSpec{"maxGain", Kind::Scalar, 18.0, 0.0, 40.0, "Maximum gain", 0.0, 30.0,
                             0.1, 1, "dB"},
    AudioEffectParameterSpec{"response", Kind::Scalar, 1.0, 0.05, 5.0, "Response", 0.05, 5.0, 0.05,
                             2, "s"},
    AudioEffectParameterSpec{"gate", Kind::Scalar, -55.0, -80.0, -30.0, "Noise gate", -80.0, -30.0,
                             0.1, 1, "dB"},
};

inline constexpr std::array kMono{
    AudioEffectParameterSpec{"amount", Kind::Scalar, 1.0, 0.0, 1.0, "Amount", 0.0, 1.0, 1.0, 0, "%",
                             100.0},
};

inline constexpr std::array kHiss{
    AudioEffectParameterSpec{"level", Kind::Scalar, -50.0, -90.0, 0.0, "Level", -90.0, -20.0, 0.1,
                             1, "dB"},
    AudioEffectParameterSpec{"tone", Kind::Scalar, 1000.0, 20.0, 16000.0, "Tone", 20.0, 8000.0, 10.0,
                             0, "Hz"},
    AudioEffectParameterSpec{"seed", Kind::Count, 0.0, 0.0, 65535.0, "Seed", 0.0, 65535.0, 1.0, 0},
};

} // namespace audio_effect_parameters

[[nodiscard]] constexpr bool isAudioEffectNodeType(const std::string_view typeId) noexcept {
    return typeId == kAudioEqualizerNodeType || typeId == kAudioCompressorNodeType ||
           typeId == kAudioAutoLevelNodeType || typeId == kAudioMonoNodeType ||
           typeId == kAudioHissNodeType;
}

// The parameters of the effect node `typeId`, in definition order; empty for another type.
[[nodiscard]] constexpr std::span<const AudioEffectParameterSpec>
audioEffectParameters(const std::string_view typeId) noexcept {
    if (typeId == kAudioEqualizerNodeType)
        return audio_effect_parameters::kEqualizer;
    if (typeId == kAudioCompressorNodeType)
        return audio_effect_parameters::kCompressor;
    if (typeId == kAudioAutoLevelNodeType)
        return audio_effect_parameters::kAutoLevel;
    if (typeId == kAudioMonoNodeType)
        return audio_effect_parameters::kMono;
    if (typeId == kAudioHissNodeType)
        return audio_effect_parameters::kHiss;
    return {};
}

inline constexpr std::array<std::string_view, 5> kAudioEffectNodeTypes{
    kAudioEqualizerNodeType, kAudioCompressorNodeType, kAudioAutoLevelNodeType,
    kAudioMonoNodeType, kAudioHissNodeType};

// The node type a schema key "<type>.<role>" belongs to, or empty.
[[nodiscard]] constexpr std::string_view audioEffectTypeOfSchemaKey(const std::string_view key) noexcept {
    for (const auto type : kAudioEffectNodeTypes)
        if (key.size() > type.size() + 1 && key.starts_with(type) && key[type.size()] == '.')
            return type;
    return {};
}

[[nodiscard]] constexpr const AudioEffectParameterSpec*
audioEffectParameterSpec(const std::string_view schemaKey) noexcept {
    const auto type = audioEffectTypeOfSchemaKey(schemaKey);
    if (type.empty())
        return nullptr;
    const auto role = schemaKey.substr(type.size() + 1);
    for (const auto& spec : audioEffectParameters(type))
        if (spec.role == role)
            return &spec;
    return nullptr;
}

[[nodiscard]] constexpr bool isAudioEffectScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = audioEffectParameterSpec(key);
    return spec != nullptr && spec->kind == AudioEffectParameterKind::Scalar;
}
// A constant (the hiss seed): no socket and no animation.
[[nodiscard]] constexpr bool isAudioEffectConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = audioEffectParameterSpec(key);
    return spec != nullptr && spec->kind != AudioEffectParameterKind::Scalar;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isAudioEffectScalarWithinDomain(const std::string_view key,
                                                             const double value) noexcept {
    const auto* spec = audioEffectParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document

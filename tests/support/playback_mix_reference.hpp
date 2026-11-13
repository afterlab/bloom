#pragma once

// What the playback mixer plays for an evaluated audio mix, for tests that compare another route
// (an export, a render) with playback. It is test support and not part of any module: a module's
// own tests include it where they may not name the media mixer themselves.
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::test {

// The interleaved stereo samples of `frames` frames from time zero. Every clip of `mix` reads the
// same stereo source, `plane` in both channels at `rate`, and carries the mix's level, range and
// effect chain as playback builds them from one evaluated mix. nullopt when the mixer refuses.
[[nodiscard]] inline std::optional<std::vector<float>>
playbackMixReference(const runtime::AudioMixDescription& mix, const std::vector<float>& plane,
                     const std::uint32_t rate, const std::size_t frames) {
    const auto buffer = std::make_shared<const media::audio::AudioBuffer>(
        media::audio::AudioBuffer{rate, 2, plane.size(), {plane, plane}});
    std::vector<media::audio::playback::AudioClip> clips;
    for (std::size_t index = 0; index < mix.clips.size(); ++index) {
        const auto& description = mix.clips[index];
        clips.push_back({.buffer = buffer,
                         .startTime = description.startTime,
                         .level = static_cast<float>(description.level),
                         .muted = description.muted,
                         .solo = description.solo,
                         .endTime = description.endTime,
                         .mapTime = {}});
        if (!description.timeMappings.empty())
            clips.back().mapTime = [description](const core::RationalTime time) {
                return runtime::mapAudioClipTime(description, time);
            };
        clips.back().effectKey = description.sourceNodeId.value() * 1'000'003ULL + index;
        for (const auto& effect : description.effects)
            clips.back().effects.push_back(media::audio::effects::makeEffectSettings(
                static_cast<media::audio::effects::EffectKind>(effect.kind), effect.values,
                effect.values));
    }
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* sink = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend), {rate, 2, 16384});
    engine.replaceClips(std::move(clips));
    if (engine.play(core::RationalTime{}) || engine.renderForTesting(frames))
        return std::nullopt;
    const auto captured = sink->captured();
    return std::vector<float>(captured.begin(), captured.end());
}

} // namespace bloom::test

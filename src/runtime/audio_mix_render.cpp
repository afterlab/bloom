#include <bloom/runtime/audio_mix_render.hpp>

#include <bloom/media/audio/effects.hpp>

#include <span>

namespace bloom::runtime {
namespace {

static_assert(static_cast<int>(AudioEffectKind::Equalizer) ==
                  static_cast<int>(media::audio::effects::EffectKind::Equalizer) &&
              static_cast<int>(AudioEffectKind::Compressor) ==
                  static_cast<int>(media::audio::effects::EffectKind::Compressor) &&
              static_cast<int>(AudioEffectKind::AutoLevel) ==
                  static_cast<int>(media::audio::effects::EffectKind::AutoLevel) &&
              static_cast<int>(AudioEffectKind::Mono) ==
                  static_cast<int>(media::audio::effects::EffectKind::Mono) &&
              static_cast<int>(AudioEffectKind::Hiss) ==
                  static_cast<int>(media::audio::effects::EffectKind::Hiss));

// #569: a clip's effect chain for the mixer, with the parameters at the block's end when they differ.
[[nodiscard]] std::vector<media::audio::effects::EffectSettings>
effectSettings(const AudioClipDescription& start, const AudioClipDescription* end, bool& moves) {
    std::vector<media::audio::effects::EffectSettings> settings;
    settings.reserve(start.effects.size());
    for (std::size_t index = 0; index < start.effects.size(); ++index) {
        const auto& effect = start.effects[index];
        const bool paired = end != nullptr && index < end->effects.size() &&
                            end->effects[index].kind == effect.kind &&
                            end->effects[index].values.size() == effect.values.size();
        settings.push_back(media::audio::effects::makeEffectSettings(
            static_cast<media::audio::effects::EffectKind>(effect.kind), effect.values,
            paired ? std::span<const double>(end->effects[index].values)
                   : std::span<const double>(effect.values)));
        moves = moves || (paired && end->effects[index].values != effect.values);
    }
    return settings;
}

} // namespace

std::vector<media::audio::playback::AudioClip>
audioMixClips(const AudioMixDescription& mix, const AudioMixDescription* endMix,
              const core::RationalTime time, const std::optional<core::RationalTime> endTime,
              const AudioMixBufferLookup& lookup) {
    // Level automation ramps linearly from this block's start to its end (#459), so a fade is
    // continuous instead of stepping once per block. Without an end mix (for example past a
    // representable time) the block holds its start level.
    const bool ramped = endMix != nullptr && endTime && endMix->clips.size() == mix.clips.size();
    std::vector<media::audio::playback::AudioClip> clips;
    clips.reserve(mix.clips.size());
    for (std::size_t index = 0; index < mix.clips.size(); ++index) {
        const auto& description = mix.clips[index];
        clips.push_back({.buffer = lookup(description),
                         .startTime = description.startTime,
                         .level = static_cast<float>(description.level),
                         .muted = description.muted,
                         .solo = description.solo,
                         .endTime = description.endTime,
                         .mapTime = {}});
        if (!description.timeMappings.empty())
            clips.back().mapTime = [description](core::RationalTime at) {
                return mapAudioClipTime(description, at);
            };
        if (!description.effects.empty()) {
            bool moves = false;
            clips.back().effectKey = description.sourceNodeId.value() * 1'000'003ULL + index;
            clips.back().effects =
                effectSettings(description, ramped ? &endMix->clips[index] : nullptr, moves);
            if (moves)
                clips.back().effectRamp = media::audio::playback::AudioEffectRamp{time, *endTime};
        }
        if (ramped) {
            const auto& end = endMix->clips[index];
            if (end.sourceNodeId == description.sourceNodeId &&
                end.assetId == description.assetId && end.audioStream == description.audioStream &&
                end.level != description.level)
                clips.back().levelRamp = media::audio::playback::AudioLevelRamp{
                    time, *endTime, static_cast<float>(end.level)};
        }
    }
    return clips;
}

} // namespace bloom::runtime

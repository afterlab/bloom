#pragma once

// The one place a mix description becomes mixer clips (#630). An export, and the sound an audio
// analysis reads, both render offline through media::audio::playback::AudioEngine block by block;
// this builds a block's clips from the mix evaluated at the block's start and, when it is there,
// at its end, so a level or an effect parameter that moves ramps across the block (#459, #569).
// Because both callers build their clips here, an analysis reads the samples an export writes.

#include <bloom/core/rational_time.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::runtime {

// The decoded audio of one clip's asset stream. It may throw to refuse the block; a null buffer
// plays as silence.
using AudioMixBufferLookup =
    std::function<std::shared_ptr<const media::audio::AudioBuffer>(const AudioClipDescription&)>;

// The clips of the block that starts at `time` with `mix`. `endMix` is the mix at `endTime`, or
// null when there is none (the block then holds its start values).
[[nodiscard]] std::vector<media::audio::playback::AudioClip>
audioMixClips(const AudioMixDescription& mix, const AudioMixDescription* endMix,
              core::RationalTime time, std::optional<core::RationalTime> endTime,
              const AudioMixBufferLookup& lookup);

} // namespace bloom::runtime

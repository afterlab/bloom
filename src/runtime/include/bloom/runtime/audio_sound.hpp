#pragma once

// #630: the rendered sound an audio analysis reads. An analysis node whose `audio` input carries
// more than a bare source (a source through its effects, a layer, a mix, a nested composition)
// reads that sound as the export would write it: the whole sound is rendered once, offline, at
// 48 kHz stereo through the code the export mixes with (audio_mix_render.hpp), into a scratch file
// in the per-user cache folder. Windows of it are then read like any source's.
//
// A rendered sound is kept under its identity: a digest of every source's content digest and of
// the mix evaluated at each block boundary (levels, switches, effect parameters, timing), so an
// equalizer or a level that changes is another sound, and the same sound compiled again is found
// again. The render is a job the store owns: a frame that needs it waits with its own
// cancellation, and a frame that is cancelled does not cancel the job. A job nobody waits for is
// dropped when another sound is asked for. Nothing is kept for a failed or cancelled render.
// docs/architecture/media-io.md, "Audio analysis".

#include <bloom/media/audio/audio.hpp>
#include <bloom/runtime/audio_analysis_source.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime {

inline constexpr std::uint32_t kAudioSoundRate = 48'000;
inline constexpr std::uint32_t kAudioSoundChannels = 2;
// The mix is evaluated at every block boundary, as the export evaluates it.
inline constexpr std::uint32_t kAudioSoundBlockFrames = 512;
// Rendered sounds kept at once; the least recently used retires with its scratch file.
inline constexpr std::size_t kAudioSoundEntries = 4;

struct PreparedAudioSound final {
    std::string digest;
    // Stereo at kAudioSoundRate from the composition's start; `buffer->stream` reads the scratch
    // file, so only a window of it is in memory.
    std::uint64_t frames = 0;
    std::shared_ptr<const media::audio::AudioBuffer> buffer;
};

enum class AudioSoundOutcome : std::uint8_t { Ready, Cancelled, Unavailable };
struct AudioSoundStatus final {
    AudioSoundOutcome outcome = AudioSoundOutcome::Unavailable;
    // For Unavailable: the reason, in words the artist can read.
    std::string detail;
    std::shared_ptr<const PreparedAudioSound> sound;
};

// The rendered sound of `sound`, or null when it is not prepared. Never blocks on I/O and never
// evaluates the mix: it finds only a sound whose identity a worker has already computed.
[[nodiscard]] std::shared_ptr<const PreparedAudioSound>
findAudioSound(const AudioAnalysisSound& sound);

// Renders `sound`, or joins the job that is rendering it, and waits. `plan` is the plan `sound`
// was compiled into (or one that nests it); `assetBase` resolves relative asset locators. Heavy:
// call it on a worker. `cancellation` ends the wait, not the job.
[[nodiscard]] AudioSoundStatus prepareAudioSound(
    const std::shared_ptr<const CompiledCompositionPlan>& plan,
    const std::shared_ptr<const AudioAnalysisSound>& sound, const std::filesystem::path& assetBase,
    const CancellationToken& cancellation);

enum class AudioSoundPhase : std::uint8_t { Idle, Identifying, Rendering };
struct AudioSoundActivity final {
    AudioSoundPhase phase = AudioSoundPhase::Idle;
    // Frames done and the frames of the whole sound, for the job that is running.
    std::uint64_t framesDone = 0;
    std::uint64_t framesTotal = 0;
    // Jobs waiting behind the running one.
    std::size_t queued = 0;
    // The reason the last job failed, until another job succeeds; empty when none failed.
    std::string lastFailure;
};
// What the store is doing now, for a progress readout. Never blocks on I/O.
[[nodiscard]] AudioSoundActivity audioSoundActivity();

struct AudioSoundStatistics final {
    std::size_t entries = 0;
    std::uint64_t scratchBytes = 0;
    std::uint64_t rendered = 0;
    // A job that found its sound already rendered under the same digest.
    std::uint64_t reused = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
};
[[nodiscard]] AudioSoundStatistics audioSoundStatistics();

// Cancels every job, waits for the worker to be idle and drops every rendered sound with its
// scratch file (cache purge, tests, shutdown).
void clearAudioSounds();

// The folder the scratch files are made in: by default userCacheDirectory("bloom") /
// "audio-analysis". A test points it at a private folder; nullopt restores the default.
void setAudioSoundDirectory(std::optional<std::filesystem::path> directory);

} // namespace bloom::runtime

#pragma once

// #631 to #635: what the table nodes (Audio Bands, Audio Sample, Audio Features, Audio Beat, Audio
// Pitch) read. Each reading is computed once for the whole sound from its analysis table
// (audio_sound.hpp, media/audio/sound_table.hpp), on a worker, and kept in the analysis cache
// under the sound's digest and the reading's own settings, so a frame's value is a function of the
// sound and the time only. Reading a prepared result never does I/O.
// docs/architecture/media-io.md, "Audio analysis".

#include <bloom/media/audio/sound_table.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/audio_analysis_source.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace bloom::runtime {

enum class AudioTableKind : std::uint8_t {
    // One value per row, 0 to 1: a band range's (or the whole level's) power, normalized and
    // smoothed. `columns` rows.
    Envelope,
    // Five values per row, one after another: brightness, noisiness, pan, width and the running
    // total (the seconds-weighted sum of the level, 0 to 1 against the 95th percentile, from the
    // start). `lanes` is 5 and `columns` the rows; values are [lane][row].
    Features,
    // The rows where a sound starts, ascending. `columns` hits.
    Hits,
    // `amplitude` is the tempo in beats a minute (0 for none) and `values` the beat times in
    // seconds.
    Beats,
    // Twelve shares per row: [row][pitch class]. `lanes` is 12 and `columns` the rows.
    Pitch,
    // The numbers of one row (kTableRowFloats), for a spectrum at one time.
    Row,
};
inline constexpr std::uint32_t kAudioFeatureLanes = 5;
inline constexpr std::uint32_t kAudioFeatureBrightness = 0;
inline constexpr std::uint32_t kAudioFeatureNoisiness = 1;
inline constexpr std::uint32_t kAudioFeaturePan = 2;
inline constexpr std::uint32_t kAudioFeatureWidth = 3;
inline constexpr std::uint32_t kAudioFeatureTotal = 4;

struct AudioTableQuery final {
    std::shared_ptr<const AudioAnalysisSource> source;
    AudioTableKind kind = AudioTableKind::Envelope;
    // Envelope and Features.
    media::audio::analysis::TableChannel channel = media::audio::analysis::TableChannel::Mix;
    // Envelope: the level of the whole signal, or the band range.
    bool level = false;
    double lowHz = media::audio::analysis::kTableLowHz;
    double highHz = media::audio::analysis::kTableHighHz;
    media::audio::analysis::TableEnvelopeSettings envelope{};
    media::audio::analysis::TableHitSettings hits{};
    media::audio::analysis::TableBeatSettings beats{};
    // Row: which.
    std::uint64_t row = 0;
};

// The cache identity of `query`, or empty while the sound has no digest yet (nothing prepared).
[[nodiscard]] std::string audioTableKey(const AudioTableQuery& query);

// The prepared reading, or null. Never blocks on I/O.
[[nodiscard]] std::shared_ptr<const AudioAnalysisResult> findAudioTable(const AudioTableQuery& query);

// Renders the sound when it is not there (audio_sound.hpp), computes the reading from its table
// and keeps it; a no-op when it is kept. Heavy: call it on a worker. `plan` is the plan the source
// was compiled into, or null to use the plan of the evaluation this thread runs inside.
[[nodiscard]] AudioAnalysisStatus
prepareAudioTable(const AudioTableQuery& query, const std::filesystem::path& assetBase,
                  const CancellationToken& cancellation,
                  const std::shared_ptr<const CompiledCompositionPlan>& plan = nullptr);

// Prepares `query` under this thread's innermost AudioAnalysisPreparation, or none when there is
// none (an evaluation outside a worker's preparation only reads what is prepared).
[[nodiscard]] std::optional<AudioAnalysisStatus> prepareAudioTableHere(const AudioTableQuery& query);

} // namespace bloom::runtime

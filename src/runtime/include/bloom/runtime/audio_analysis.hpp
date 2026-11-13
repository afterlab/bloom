#pragma once

// #347: prepared audio analysis. An analysis node (Audio Amplitude, Audio Waveform, Audio Spectrum)
// reads the numbers of a window of one source's audio at one exact time. The window is read and
// reduced on a worker (prepareAudioAnalysis, which the evaluators call before they sample the
// node), kept in a bounded cache under an exact identity (source revision, stream, timing, time and
// parameters), and read back without any I/O (findAudioAnalysis), so evaluating a frame on the
// interface thread, the CPU reference or the GPU preparation never decodes. Nothing is cached for a
// failed or cancelled analysis. docs/architecture/media-io.md, "Audio analysis".

#include <bloom/core/rational_time.hpp>
#include <bloom/media/audio/analysis.hpp>
#include <bloom/runtime/audio_analysis_source.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

enum class AudioAnalysisKind : std::uint8_t { Amplitude, Waveform, Spectrum };

struct AudioAnalysisQuery final {
    std::shared_ptr<const AudioAnalysisSource> source;
    AudioAnalysisKind kind = AudioAnalysisKind::Amplitude;
    // The time on the source node's own clock: the frame, before the source's start, loop and rate.
    core::RationalTime time{};
    // Amplitude and Waveform: the window around the time, in seconds; Amplitude also the measure.
    double windowSeconds = 0.05;
    media::audio::analysis::Measure measure = media::audio::analysis::Measure::Rms;
    // Waveform and Spectrum: the columns, which are the pixels across the plot.
    std::uint32_t columns = 0;
    bool separateChannels = false;
    media::audio::analysis::SpectrumSettings spectrum{};
};

struct AudioAnalysisResult final {
    // Amplitude: the linear level.
    double amplitude = 0.0;
    // Waveform: [column][lane][minimum, maximum]; Spectrum: [column] one level in [0, 1].
    std::uint32_t lanes = 0;
    std::uint32_t columns = 0;
    std::vector<float> values;
};

// The query Audio Amplitude reads at `time` plus `offsetSeconds`, with a window of `windowMs`
// milliseconds, or none when a value is outside its range.
[[nodiscard]] std::optional<AudioAnalysisQuery>
audioAmplitudeQuery(const CompiledValueAudioAmplitude& kernel, core::RationalTime time,
                    double windowMs, double offsetSeconds);

enum class AudioAnalysisOutcome : std::uint8_t { Ready, Cancelled, Unavailable };
struct AudioAnalysisStatus final {
    AudioAnalysisOutcome outcome = AudioAnalysisOutcome::Unavailable;
    // For Unavailable: the reason, in words the artist can read.
    std::string detail;
};

// The cache identity of `query`: the same string for the same analysis of the same source revision,
// whatever process or thread asks. Empty for a query that cannot be analyzed (no source).
[[nodiscard]] std::string audioAnalysisKey(const AudioAnalysisQuery& query);

// Reads, reduces and caches the analysis; a no-op when it is cached. Heavy: call it on a worker.
// `assetBase` resolves the asset's relative locator. A source whose file is missing, changed since
// import (its digest differs), or has no such audio stream is Unavailable.
// #630: a source that is not bare (effects, a layer, a mix) is read from its rendered sound
// (audio_sound.hpp), which is rendered here when it is not there; that needs `plan`, the plan the
// source was compiled into, and the query's time is then the composition's.
[[nodiscard]] AudioAnalysisStatus
prepareAudioAnalysis(const AudioAnalysisQuery& query, const std::filesystem::path& assetBase,
                     const CancellationToken& cancellation,
                     const std::shared_ptr<const CompiledCompositionPlan>& plan = nullptr);

// The cached analysis, or null when it is not prepared. Never blocks on I/O.
[[nodiscard]] std::shared_ptr<const AudioAnalysisResult>
findAudioAnalysis(const AudioAnalysisQuery& query);

// While one is alive on a thread, a value-graph evaluation on that thread may prepare the analysis
// it needs (the evaluators install one on their worker for the duration of a frame). Evaluations on
// a thread without one only read what is already prepared.
class AudioAnalysisPreparation final {
  public:
    // `plan` is the plan being evaluated: a rendered sound (#630) is evaluated against it.
    AudioAnalysisPreparation(std::shared_ptr<const CompiledCompositionPlan> plan,
                             std::filesystem::path assetBase, CancellationToken cancellation);
    ~AudioAnalysisPreparation();
    AudioAnalysisPreparation(const AudioAnalysisPreparation&) = delete;
    AudioAnalysisPreparation& operator=(const AudioAnalysisPreparation&) = delete;
    // Prepares `query` under this thread's innermost preparation, or none when there is none.
    [[nodiscard]] static std::optional<AudioAnalysisStatus> prepareHere(const AudioAnalysisQuery& query);
    // Runs `prepare` with this thread's innermost preparation (its asset base, cancellation and
    // plan), or returns none when there is none.
    [[nodiscard]] static std::optional<AudioAnalysisStatus> prepareWith(
        const std::function<AudioAnalysisStatus(
            const std::filesystem::path&, const CancellationToken&,
            const std::shared_ptr<const CompiledCompositionPlan>&)>& prepare);
    // The plan of this thread's innermost preparation, or null when there is none.
    [[nodiscard]] static const std::shared_ptr<const CompiledCompositionPlan>& planHere() noexcept;

  private:
    std::shared_ptr<const CompiledCompositionPlan> plan_;
    std::filesystem::path assetBase_;
    CancellationToken cancellation_;
    const AudioAnalysisPreparation* previous_ = nullptr;
};

struct AudioAnalysisStatistics final {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t computed = 0;
    std::uint64_t evicted = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::size_t openSources = 0;
    std::uint64_t opened = 0;
};
[[nodiscard]] AudioAnalysisStatistics audioAnalysisStatistics();
// Drops every cached analysis and every opened source (cache purge, tests).
void clearAudioAnalysis();
// The byte budget of the cache; the least recently used entries retire when it is exceeded.
inline constexpr std::size_t kAudioAnalysisCacheBytes = std::size_t{32} << 20;
inline constexpr std::size_t kAudioAnalysisOpenSources = 4;

// The source-clock seconds a composition `time` reads in `source`, or none when the source is
// silent then (a footage pass that has ended).
[[nodiscard]] std::optional<double> audioAnalysisSourceSeconds(const AudioAnalysisSource& source,
                                                               core::RationalTime time);

} // namespace bloom::runtime

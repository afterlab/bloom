// The Timing and Audio checks of a single-file movie or WAV export (issue #465): each compares
// what the reopened artifact reports with what the encoder was asked for, instead of passing
// whenever a duration exists or audio was requested.
#include <bloom/host/export_verification.hpp>

#include <limits>
#include <numeric>

namespace bloom::host {
namespace {

std::optional<ExportRationalValue> reduced(const std::int64_t numerator,
                                           const std::int64_t denominator) {
    if (denominator <= 0 || numerator < 0)
        return std::nullopt;
    const auto divisor = std::gcd(numerator, denominator);
    return ExportRationalValue{numerator / divisor, denominator / divisor};
}

// Frames over the frame rate for a movie; samples over the sample rate for WAV.
std::optional<ExportRationalValue> requestedDuration(const ExportStreamRequest& requested) {
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (requested.frames != 0) {
        const auto& rate = requested.frameRate;
        if (rate.numerator <= 0 || rate.denominator <= 0 ||
            requested.frames > limit / static_cast<std::uint64_t>(rate.denominator))
            return std::nullopt;
        return reduced(static_cast<std::int64_t>(requested.frames) * rate.denominator,
                       rate.numerator);
    }
    if (requested.audioSamples == 0 || requested.sampleRate == 0 || requested.audioSamples > limit)
        return std::nullopt;
    return reduced(static_cast<std::int64_t>(requested.audioSamples), requested.sampleRate);
}

// floor(duration * sampleRate): the sample count that exactly covers a duration.
std::optional<std::uint64_t> samplesCovering(const ExportRationalValue duration,
                                             const std::uint32_t sampleRate) {
    if (duration.numerator < 0 || duration.denominator <= 0 || sampleRate == 0)
        return std::nullopt;
    const auto numerator = static_cast<std::uint64_t>(duration.numerator);
    const auto denominator = static_cast<std::uint64_t>(duration.denominator);
    const auto whole = numerator / denominator, rest = numerator % denominator;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (whole > max / sampleRate || rest > max / sampleRate)
        return std::nullopt;
    const auto fraction = rest * sampleRate / denominator;
    if (whole * sampleRate > max - fraction)
        return std::nullopt;
    return whole * sampleRate + fraction;
}

} // namespace

ExportCheckResult exportTimingCheck(const ExportStreamRequest& requested,
                                    const ExportStreamObservation& observed) {
    const auto expected = requestedDuration(requested);
    const auto actual = reduced(observed.duration.numerator, observed.duration.denominator);
    ExportCheckResult result{ExportCheck::Timing,
                             ExportCheckOutcome::Failed,
                             "export.check.timing.duration-mismatch",
                             expected ? ExportCheckValue{*expected} : ExportCheckValue{},
                             observed.duration,
                             std::nullopt};
    if (!expected || !actual || *expected != *actual)
        return result;
    // A movie with sound: the audio must end where the video ends, to within one sample.
    if (requested.frames != 0 && requested.audioSamples != 0 &&
        samplesCovering(*actual, requested.sampleRate) != observed.audioSamples) {
        result.code = "export.check.timing.audio-duration-mismatch";
        return result;
    }
    result.outcome = ExportCheckOutcome::Passed;
    result.code = "export.check.timing";
    return result;
}

ExportCheckResult exportAudioCheck(const ExportStreamRequest& requested,
                                   const ExportStreamObservation& observed) {
    if (requested.audioSamples == 0)
        return {ExportCheck::Audio,
                observed.audioSamples == 0 ? ExportCheckOutcome::NotApplicable
                                           : ExportCheckOutcome::Failed,
                observed.audioSamples == 0 ? "export.check.audio" : "export.check.audio.unexpected",
                std::uint64_t{0},
                observed.audioSamples,
                std::nullopt};
    const bool matches = observed.audioSamples == requested.audioSamples;
    return {ExportCheck::Audio,
            matches ? ExportCheckOutcome::Passed : ExportCheckOutcome::Failed,
            matches ? "export.check.audio" : "export.check.audio.sample-count-mismatch",
            requested.audioSamples,
            observed.audioSamples,
            std::nullopt};
}

} // namespace bloom::host

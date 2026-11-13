#include "audio_table_values.hpp"

#include <bloom/document/image_math_parameters.hpp>
#include <bloom/media/audio/sound_table.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/audio_sound.hpp>
#include <bloom/runtime/audio_table_analysis.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::runtime::detail {
namespace {

namespace an = media::audio::analysis;
namespace choices = document::image_pixel_parameters;

constexpr double kRowsPerSecond = static_cast<double>(an::kTableRate) / an::kTableHopFrames;

[[nodiscard]] std::string_view nameOf(const AudioTableNode node) noexcept {
    switch (node) {
    case AudioTableNode::Bands:
        return "Audio Bands";
    case AudioTableNode::Sample:
        return "Audio Sample";
    case AudioTableNode::Features:
        return "Audio Features";
    case AudioTableNode::Beat:
        return "Audio Beat";
    case AudioTableNode::Pitch:
        break;
    }
    return "Audio Pitch";
}

[[nodiscard]] an::TableChannel channelOf(const std::int64_t stored) noexcept {
    return static_cast<an::TableChannel>(
        std::clamp<std::int64_t>(stored, choices::kAudioChannelMix, choices::kAudioChannelSide));
}

// Finds a reading, preparing it when this thread may. Remembers the first reason one is missing.
struct Readings final {
    std::string_view node;
    std::string summary;
    std::string detail;

    template <typename Query, typename Find, typename Prepare>
    [[nodiscard]] std::shared_ptr<const AudioAnalysisResult> get(const Query& query, Find find,
                                                                 Prepare prepare) {
        auto found = find(query);
        if (found)
            return found;
        if (const auto status = prepare(query)) {
            if (status->outcome == AudioAnalysisOutcome::Ready)
                found = find(query);
            else if (status->outcome == AudioAnalysisOutcome::Unavailable && summary.empty()) {
                summary = std::string(node) + " audio is unavailable";
                detail = status->detail;
            }
            if (found || status->outcome != AudioAnalysisOutcome::Ready)
                return found;
        }
        if (summary.empty()) {
            summary = std::string(node) + " is not prepared";
            detail = "The audio has not been analyzed yet; the node reads as silence until it is.";
        }
        return nullptr;
    }
    [[nodiscard]] std::shared_ptr<const AudioAnalysisResult> table(const AudioTableQuery& query) {
        return get(query, findAudioTable, prepareAudioTableHere);
    }
    [[nodiscard]] std::shared_ptr<const AudioAnalysisResult>
    window(const AudioAnalysisQuery& query) {
        return get(query, findAudioAnalysis, AudioAnalysisPreparation::prepareHere);
    }
};

// One lane of a [lane][row] reading at `seconds`.
[[nodiscard]] double laneAt(const AudioAnalysisResult& reading, const std::uint32_t lane,
                            const double seconds) noexcept {
    const auto rows = static_cast<std::size_t>(reading.columns);
    if (reading.values.size() < (static_cast<std::size_t>(lane) + 1) * rows)
        return 0.0;
    return an::tableCurveAt(std::span<const float>(reading.values).subspan(lane * rows, rows),
                            seconds);
}

// The value at `position` (0 to 1) across a list, between items in a straight line.
[[nodiscard]] double across(const std::vector<double>& list, const double position) noexcept {
    if (list.empty())
        return 0.0;
    const double at = std::clamp(position, 0.0, 1.0) * static_cast<double>(list.size() - 1);
    const auto index = static_cast<std::size_t>(at);
    if (index + 1 >= list.size())
        return list.back();
    return list[index] + (list[index + 1] - list[index]) * (at - static_cast<double>(index));
}

[[nodiscard]] std::uint64_t soundRows(const AudioAnalysisSource& source) noexcept {
    const double seconds = source.sound ? source.sound->duration.toSeconds() : 0.0;
    if (!(seconds > 0.0) || !std::isfinite(seconds))
        return 0;
    return an::tableRows(static_cast<std::uint64_t>(std::ceil(seconds * kAudioSoundRate)));
}

} // namespace

std::vector<CompiledValue> silentAudioTableOutputs(const AudioTableNode node) {
    const auto zeros = [](const std::size_t count) {
        return CompiledValue{CompiledNumberList{std::vector<double>(count, 0.0)}};
    };
    switch (node) {
    case AudioTableNode::Bands:
        return {CompiledValue{0.0}};
    case AudioTableNode::Sample:
        return {CompiledValue{0.0}, zeros(document::kAudioSampleSpectrumItems),
                zeros(document::kAudioSampleWaveformItems)};
    case AudioTableNode::Features:
        return std::vector<CompiledValue>(document::kAudioFeaturesOutputPorts.size(),
                                          CompiledValue{0.0});
    case AudioTableNode::Beat:
        return std::vector<CompiledValue>(document::kAudioBeatOutputPorts.size(),
                                          CompiledValue{0.0});
    case AudioTableNode::Pitch:
        break;
    }
    // No pitch class is the strongest: -1.
    return {zeros(an::kTablePitchClasses), CompiledValue{-1.0}, CompiledValue{0.0}};
}

AudioTableValues audioTableValues(const CompiledValueAudioTable& kernel,
                                  const std::span<const double> operands,
                                  const core::RationalTime time) {
    AudioTableValues result;
    result.outputs = silentAudioTableOutputs(kernel.node);
    if (!kernel.source)
        return result;
    Readings readings{nameOf(kernel.node), {}, {}};
    const auto choice = [&](const std::size_t index) {
        return index < kernel.choices.size() ? kernel.choices[index] : std::int64_t{0};
    };
    const auto operand = [&](const std::size_t index, const double low, const double high) {
        const double value = index < operands.size() ? operands[index] : low;
        return std::isfinite(value) ? std::clamp(value, low, high) : low;
    };
    const auto finish = [&] {
        result.summary = std::move(readings.summary);
        result.detail = std::move(readings.detail);
        return result;
    };
    AudioTableQuery query;
    query.source = kernel.source;
    switch (kernel.node) {
    case AudioTableNode::Bands: {
        // lowFrequency, highFrequency, range, floor, ceiling, rolling, attack, release, offset.
        query.kind = AudioTableKind::Envelope;
        query.channel = channelOf(choice(0));
        query.lowHz = operand(0, an::kTableLowHz, an::kTableHighHz);
        query.highHz = operand(1, an::kTableLowHz, an::kTableHighHz);
        if (!(query.highHz > query.lowHz)) {
            result.summary = "Audio Bands range is empty";
            result.detail = "The highest frequency must be above the lowest.";
            return result;
        }
        query.envelope.normalize = static_cast<an::TableNormalize>(std::clamp<std::int64_t>(
            choice(1), choices::kAudioNormalizePercentile, choices::kAudioNormalizeManual));
        query.envelope.rangeDb = operand(2, 1.0, 120.0);
        query.envelope.floorDb = operand(3, -120.0, 24.0);
        query.envelope.ceilingDb = operand(4, -120.0, 24.0);
        query.envelope.rollingSeconds = operand(5, 0.1, 600.0);
        query.envelope.attackSeconds = operand(6, 0.0, 10000.0) / 1000.0;
        query.envelope.releaseSeconds = operand(7, 0.0, 10000.0) / 1000.0;
        const double at = time.toSeconds() + operand(8, -86400.0, 86400.0);
        if (const auto curve = readings.table(query))
            result.outputs[0] = an::tableCurveAt(curve->values, at);
        return finish();
    }
    case AudioTableNode::Sample: {
        // position, floor, ceiling, window, offset; choices read, channel.
        const double position = operand(0, 0.0, 1.0);
        const double floor = operand(1, -120.0, 24.0);
        const double ceiling = std::max(operand(2, -120.0, 24.0), floor + 1.0);
        const double window = operand(3, 1.0, 1000.0) / 1000.0;
        const double at = time.toSeconds() + operand(4, -86400.0, 86400.0);
        const auto channel = channelOf(choice(1));
        std::vector<double> spectrum(document::kAudioSampleSpectrumItems, 0.0);
        std::vector<double> waveform(document::kAudioSampleWaveformItems, 0.0);
        const auto row = at >= 0.0 ? static_cast<std::uint64_t>(std::llround(at * kRowsPerSecond))
                                   : soundRows(*kernel.source);
        if (row < soundRows(*kernel.source)) {
            query.kind = AudioTableKind::Row;
            query.row = row;
            if (const auto values = readings.table(query))
                for (std::uint32_t band = 0; band < an::kTableBands; ++band) {
                    const double power = an::tableBandPower(values->values, channel,
                                                            an::tableBandEdge(band),
                                                            an::tableBandEdge(band + 1));
                    spectrum[band] = std::clamp(
                        (an::tablePowerDb(power) - floor) / (ceiling - floor), 0.0, 1.0);
                }
            const auto center = core::RationalTime::create(
                std::llround(at * static_cast<double>(kAudioSoundRate)),
                static_cast<std::int64_t>(kAudioSoundRate));
            AudioAnalysisQuery wave;
            wave.source = kernel.source;
            wave.kind = AudioAnalysisKind::Waveform;
            wave.windowSeconds = window;
            wave.columns = static_cast<std::uint32_t>(document::kAudioSampleWaveformItems);
            wave.separateChannels = false;
            if (center) {
                wave.time = *center;
                if (const auto columns = readings.window(wave);
                    columns && columns->values.size() >= waveform.size() * 2)
                    for (std::size_t index = 0; index < waveform.size(); ++index) {
                        // Each item is the sample of its span that is farthest from zero.
                        const double low = static_cast<double>(columns->values[index * 2]);
                        const double high = static_cast<double>(columns->values[index * 2 + 1]);
                        waveform[index] = std::fabs(high) >= std::fabs(low) ? high : low;
                    }
            }
        }
        result.outputs[0] = across(
            choice(0) == choices::kAudioSampleWaveform ? waveform : spectrum, position);
        result.outputs[1] = CompiledNumberList{std::move(spectrum)};
        result.outputs[2] = CompiledNumberList{std::move(waveform)};
        return finish();
    }
    case AudioTableNode::Features: {
        // sensitivity, gap, pulse, offset; choice channel.
        const double pulse = operand(2, 1.0, 10000.0) / 1000.0;
        const double at = time.toSeconds() + operand(3, -86400.0, 86400.0);
        query.kind = AudioTableKind::Hits;
        query.hits.sensitivity = operand(0, 1.0, 20.0);
        query.hits.minimumGapSeconds = operand(1, 10.0, 5000.0) / 1000.0;
        if (const auto hits = readings.table(query)) {
            // The hits at or before the time.
            const auto row = static_cast<float>(std::floor(at * kRowsPerSecond));
            const auto past = static_cast<std::size_t>(
                std::upper_bound(hits->values.begin(), hits->values.end(), row) -
                hits->values.begin());
            const double since =
                past == 0 ? std::max(at, 0.0)
                          : at - static_cast<double>(hits->values[past - 1]) / kRowsPerSecond;
            result.outputs[0] = past == 0 ? 0.0 : std::clamp(1.0 - since / pulse, 0.0, 1.0);
            result.outputs[1] = since;
            result.outputs[2] = static_cast<double>(past);
        }
        query.kind = AudioTableKind::Features;
        query.channel = channelOf(choice(0));
        if (const auto features = readings.table(query)) {
            result.outputs[3] = laneAt(*features, kAudioFeatureBrightness, at);
            result.outputs[4] = laneAt(*features, kAudioFeatureNoisiness, at);
            result.outputs[5] = laneAt(*features, kAudioFeaturePan, at);
            result.outputs[6] = laneAt(*features, kAudioFeatureWidth, at);
            // The total holds after the end of the sound.
            const double last = features->columns == 0
                                    ? 0.0
                                    : static_cast<double>(features->columns - 1) / kRowsPerSecond;
            result.outputs[7] = laneAt(*features, kAudioFeatureTotal, std::clamp(at, 0.0, last));
        }
        return finish();
    }
    case AudioTableNode::Beat: {
        // slowest, fastest, offset.
        query.kind = AudioTableKind::Beats;
        query.beats.minimumBpm = operand(0, 20.0, 400.0);
        query.beats.maximumBpm = std::max(operand(1, 21.0, 480.0), query.beats.minimumBpm + 1.0);
        const double at = time.toSeconds() + operand(2, -86400.0, 86400.0);
        const auto beats = readings.table(query);
        if (beats && beats->amplitude > 0.0 && !beats->values.empty()) {
            const auto past = static_cast<std::size_t>(
                std::upper_bound(beats->values.begin(), beats->values.end(),
                                 static_cast<float>(at)) -
                beats->values.begin());
            const double period = 60.0 / beats->amplitude;
            double phase = 0.0;
            if (past > 0) {
                const double from = static_cast<double>(beats->values[past - 1]);
                const double length = past < beats->values.size()
                                          ? static_cast<double>(beats->values[past]) - from
                                          : period;
                // After the last beat the phase runs on at the tempo.
                phase = length > 0.0 ? std::fmod((at - from) / length, 1.0) : 0.0;
            }
            result.outputs[0] = phase;
            result.outputs[1] = past > 0 ? 1.0 - phase : 0.0;
            result.outputs[2] = static_cast<double>(past);
            result.outputs[3] = beats->amplitude;
        }
        return finish();
    }
    case AudioTableNode::Pitch: {
        query.kind = AudioTableKind::Pitch;
        const double at = time.toSeconds() + operand(0, -86400.0, 86400.0);
        if (const auto pitch = readings.table(query)) {
            const auto rows = static_cast<std::size_t>(pitch->columns);
            std::vector<double> shares(an::kTablePitchClasses, 0.0);
            const double position = at * kRowsPerSecond;
            if (rows != 0 && position >= 0.0 && position <= static_cast<double>(rows - 1) &&
                pitch->values.size() >= rows * an::kTablePitchClasses) {
                const auto row = static_cast<std::size_t>(position);
                const auto next = std::min(row + 1, rows - 1);
                const double part = position - static_cast<double>(row);
                for (std::size_t note = 0; note < an::kTablePitchClasses; ++note) {
                    const double a =
                        static_cast<double>(pitch->values[row * an::kTablePitchClasses + note]);
                    const double b =
                        static_cast<double>(pitch->values[next * an::kTablePitchClasses + note]);
                    shares[note] = a + (b - a) * part;
                }
            }
            const auto strongest = std::max_element(shares.begin(), shares.end());
            if (*strongest > 0.0) {
                result.outputs[1] = static_cast<double>(strongest - shares.begin());
                result.outputs[2] = *strongest;
            }
            result.outputs[0] = CompiledNumberList{std::move(shares)};
        }
        return finish();
    }
    }
    return finish();
}

} // namespace bloom::runtime::detail

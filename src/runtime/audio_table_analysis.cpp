#include <bloom/runtime/audio_table_analysis.hpp>

#include <bloom/runtime/audio_sound.hpp>

#include "audio_analysis_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <new>
#include <vector>

namespace bloom::runtime {
namespace {

namespace an = media::audio::analysis;

std::string number(const double value) {
    char text[40];
    std::snprintf(text, sizeof text, "%a", value);
    return text;
}

// Calls `visit(row index, row)` for every row of the table, reading it in pieces. False when a
// read failed; nullopt when cancelled.
template <typename Visit>
[[nodiscard]] std::optional<bool> forEachRow(const AudioSoundTable& table,
                                             const CancellationToken& cancellation, Visit visit) {
    constexpr std::uint64_t kPiece = 256;
    std::vector<float> rows(kPiece * an::kTableRowFloats);
    for (std::uint64_t first = 0; first < table.rows(); first += kPiece) {
        if (cancellation.isCancellationRequested())
            return std::nullopt;
        const auto count = std::min<std::uint64_t>(kPiece, table.rows() - first);
        const auto piece = std::span<float>(rows).first(
            static_cast<std::size_t>(count) * an::kTableRowFloats);
        if (!table.read(first, piece))
            return false;
        for (std::uint64_t index = 0; index < count; ++index)
            visit(first + index,
                  std::span<const float>(piece).subspan(
                      static_cast<std::size_t>(index) * an::kTableRowFloats,
                      an::kTableRowFloats));
    }
    return true;
}

} // namespace

std::string audioTableKey(const AudioTableQuery& query) {
    if (!query.source || !query.source->sound || !query.source->sound->identity)
        return {};
    const auto digest = query.source->sound->identity->digest();
    if (digest.empty())
        return {};
    std::string key = "audio-table/1|k" + std::to_string(static_cast<int>(query.kind)) +
                      "|sound:" + digest;
    const auto envelope = [&] {
        key += "|n" + std::to_string(static_cast<int>(query.envelope.normalize)) + "," +
               number(query.envelope.rangeDb) + "," + number(query.envelope.rollingSeconds) + "," +
               number(query.envelope.floorDb) + "," + number(query.envelope.ceilingDb) + "|a" +
               number(query.envelope.attackSeconds) + "," + number(query.envelope.releaseSeconds);
    };
    switch (query.kind) {
    case AudioTableKind::Envelope:
        key += "|c" + std::to_string(static_cast<int>(query.channel));
        key += query.level ? std::string("|level")
                           : "|f" + number(query.lowHz) + "-" + number(query.highHz);
        envelope();
        break;
    case AudioTableKind::Features:
        key += "|c" + std::to_string(static_cast<int>(query.channel));
        break;
    case AudioTableKind::Hits:
        key += "|h" + number(query.hits.sensitivity) + "," + number(query.hits.minimumGapSeconds);
        break;
    case AudioTableKind::Beats:
        key += "|b" + number(query.beats.minimumBpm) + "-" + number(query.beats.maximumBpm);
        break;
    case AudioTableKind::Pitch:
        break;
    case AudioTableKind::Row:
        key += "|r" + std::to_string(query.row);
        break;
    }
    return key;
}

std::shared_ptr<const AudioAnalysisResult> findAudioTable(const AudioTableQuery& query) {
    const auto key = audioTableKey(query);
    return key.empty() ? nullptr : detail::findAnalysisEntry(key);
}

AudioAnalysisStatus prepareAudioTable(const AudioTableQuery& query,
                                      const std::filesystem::path& assetBase,
                                      const CancellationToken& cancellation,
                                      const std::shared_ptr<const CompiledCompositionPlan>& plan) {
    if (!query.source || !query.source->sound)
        return {AudioAnalysisOutcome::Unavailable, "No audio is connected"};
    const auto& owner = plan ? plan : AudioAnalysisPreparation::planHere();
    const auto prepared = prepareAudioSound(owner, query.source->sound, assetBase, cancellation);
    if (prepared.outcome == AudioSoundOutcome::Cancelled)
        return {AudioAnalysisOutcome::Cancelled, {}};
    if (prepared.outcome != AudioSoundOutcome::Ready || !prepared.sound || !prepared.sound->table)
        return {AudioAnalysisOutcome::Unavailable,
                prepared.detail.empty() ? "The audio could not be rendered" : prepared.detail};
    const auto key = audioTableKey(query);
    if (key.empty())
        return {AudioAnalysisOutcome::Unavailable, "No audio is connected"};
    if (detail::hasAnalysisEntry(key))
        return {AudioAnalysisOutcome::Ready, {}};
    const auto fail = [](std::string why) {
        detail::countAnalysisFailure();
        return AudioAnalysisStatus{AudioAnalysisOutcome::Unavailable, std::move(why)};
    };
    const auto cancel = [] {
        detail::countAnalysisCancelled();
        return AudioAnalysisStatus{AudioAnalysisOutcome::Cancelled, {}};
    };
    const auto& table = *prepared.sound->table;
    const auto rows = static_cast<std::size_t>(table.rows());
    auto result = std::make_shared<AudioAnalysisResult>();
    const auto unread = "The audio analysis table could not be read from the cache folder";
    try {
        std::optional<bool> walked = true;
        switch (query.kind) {
        case AudioTableKind::Envelope: {
            std::vector<double> power(rows);
            walked = forEachRow(table, cancellation,
                                [&](const std::uint64_t row, const std::span<const float> values) {
                                    power[static_cast<std::size_t>(row)] =
                                        query.level
                                            ? an::tableLevelPower(values, query.channel)
                                            : an::tableBandPower(values, query.channel,
                                                                 query.lowHz, query.highHz);
                                });
            if (walked && *walked) {
                result->values = an::tableEnvelope(power, query.envelope);
                result->lanes = 1;
                result->columns = static_cast<std::uint32_t>(rows);
            }
            break;
        }
        case AudioTableKind::Features: {
            result->values.assign(rows * kAudioFeatureLanes, 0.0F);
            std::vector<double> power(rows);
            walked = forEachRow(
                table, cancellation,
                [&](const std::uint64_t index, const std::span<const float> values) {
                    const auto row = static_cast<std::size_t>(index);
                    result->values[kAudioFeatureBrightness * rows + row] =
                        static_cast<float>(an::tableBrightness(values, query.channel));
                    result->values[kAudioFeatureNoisiness * rows + row] =
                        static_cast<float>(an::tableNoisiness(values, query.channel));
                    result->values[kAudioFeaturePan * rows + row] =
                        static_cast<float>(an::tablePan(values));
                    result->values[kAudioFeatureWidth * rows + row] =
                        static_cast<float>(an::tableWidth(values));
                    power[row] = an::tableLevelPower(values, query.channel);
                });
            if (walked && *walked) {
                // The running total: the level against the 95th percentile, summed in seconds.
                const auto level = an::tableEnvelope(power, {});
                const double hop = static_cast<double>(an::kTableHopFrames) / an::kTableRate;
                double total = 0.0;
                for (std::size_t row = 0; row < rows; ++row) {
                    result->values[kAudioFeatureTotal * rows + row] = static_cast<float>(total);
                    total += static_cast<double>(level[row]) * hop;
                }
                result->lanes = kAudioFeatureLanes;
                result->columns = static_cast<std::uint32_t>(rows);
            }
            break;
        }
        case AudioTableKind::Hits:
        case AudioTableKind::Beats: {
            std::vector<float> flux(rows);
            walked = forEachRow(table, cancellation,
                                [&](const std::uint64_t row, const std::span<const float> values) {
                                    flux[static_cast<std::size_t>(row)] = values[an::kTableFlux];
                                });
            if (!walked || !*walked)
                break;
            if (query.kind == AudioTableKind::Hits) {
                const auto hits = an::tableHits(flux, query.hits);
                result->values.reserve(hits.size());
                for (const auto row : hits)
                    result->values.push_back(static_cast<float>(row));
                result->columns = static_cast<std::uint32_t>(hits.size());
            } else {
                const auto beats = an::tableBeats(flux, query.beats);
                result->amplitude = beats.bpm;
                result->values.reserve(beats.beats.size());
                for (const double beat : beats.beats)
                    result->values.push_back(static_cast<float>(beat));
                result->columns = static_cast<std::uint32_t>(beats.beats.size());
            }
            result->lanes = 1;
            break;
        }
        case AudioTableKind::Pitch: {
            result->values.assign(rows * an::kTablePitchClasses, 0.0F);
            walked = forEachRow(
                table, cancellation,
                [&](const std::uint64_t row, const std::span<const float> values) {
                    const auto shares = an::tablePitchShares(values);
                    for (std::size_t note = 0; note < an::kTablePitchClasses; ++note)
                        result->values[static_cast<std::size_t>(row) * an::kTablePitchClasses +
                                       note] = static_cast<float>(shares[note]);
                });
            result->lanes = an::kTablePitchClasses;
            result->columns = static_cast<std::uint32_t>(rows);
            break;
        }
        case AudioTableKind::Row: {
            if (query.row >= table.rows())
                return fail("The time is outside the sound");
            result->values.assign(an::kTableRowFloats, 0.0F);
            walked = table.read(query.row, result->values);
            result->lanes = 1;
            result->columns = static_cast<std::uint32_t>(an::kTableRowFloats);
            break;
        }
        }
        if (!walked)
            return cancel();
        if (!*walked)
            return fail(unread);
    } catch (const std::bad_alloc&) {
        return fail("There is not enough memory to analyze the audio");
    }
    if (cancellation.isCancellationRequested())
        return cancel();
    detail::storeAnalysisEntry(key, std::move(result));
    return {AudioAnalysisOutcome::Ready, {}};
}

std::optional<AudioAnalysisStatus> prepareAudioTableHere(const AudioTableQuery& query) {
    return AudioAnalysisPreparation::prepareWith(
        [&](const std::filesystem::path& assetBase, const CancellationToken& cancellation,
            const std::shared_ptr<const CompiledCompositionPlan>& plan) {
            return prepareAudioTable(query, assetBase, cancellation, plan);
        });
}

} // namespace bloom::runtime

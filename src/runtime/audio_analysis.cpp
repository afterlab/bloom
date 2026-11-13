#include <bloom/runtime/audio_analysis.hpp>

#include <bloom/core/int128.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/audio_sound.hpp>
#include <bloom/runtime/footage_time.hpp>

#include "audio_source_open.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <new>

namespace bloom::runtime {
namespace {
constexpr std::uint64_t kMaximumChannels = 64;
} // namespace

namespace detail {
OpenedAssetAudio openAssetAudio(const document::AssetRecord& asset, const std::uint32_t audioStream,
                                const std::filesystem::path& base,
                                const std::function<bool()>& cancelled) {
    const auto path = media::resolveImagePath(asset.locator.path, asset.locator.relinkHint, base);
    OpenedAssetAudio opened;
    if (asset.kind == document::AssetKind::Audio) {
        switch (platform::verifySourceDigest(path, asset.contentDigest, cancelled)) {
        case platform::SourceCheck::Matches:
            break;
        case platform::SourceCheck::Cancelled:
            return {{}, {}, {}, true};
        case platform::SourceCheck::Changed:
            return {{}, {}, "The audio file changed since it was imported", false};
        case platform::SourceCheck::Missing:
            return {{}, {}, "The audio file is missing", false};
        }
        media::audio::AudioDecodeLimits limits;
        limits.sampleBudget = std::uint64_t{media::audio::AudioStream::kWindowFrames} * kMaximumChannels;
        auto decoded = media::audio::openWindowedAudio(path, limits,
                                                       media::audio::AudioStream::kWindowFrames, cancelled);
        if (!decoded)
            return decoded.error()->code == media::audio::AudioErrorCode::Cancelled
                       ? OpenedAssetAudio{{}, {}, {}, true}
                       : OpenedAssetAudio{{}, {}, media::audio::describeAudioError(*decoded.error()), false};
        opened.buffer = std::make_shared<const media::audio::AudioBuffer>(std::move(*decoded.value()));
    } else if (asset.kind == document::AssetKind::Video) {
        auto session = std::make_shared<media::video::VideoDecodeSession>(path);
        auto probed = session->probe(cancelled);
        auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
        if (probe == nullptr)
            return cancelled()
                       ? OpenedAssetAudio{{}, {}, {}, true}
                       : OpenedAssetAudio{{}, {}, std::get<media::provider::Unavailable>(probed).detail, false};
        if (probe->sourceDigest != asset.contentDigest)
            return {{}, {}, "The video file changed since it was imported", false};
        if (media::video::audioStreamAt(*probe, audioStream) == nullptr)
            return {{}, {}, "The video has no such audio stream", false};
        auto decoded = media::video::openWindowedVideoAudio(session, *probe, audioStream);
        auto* audio = std::get_if<media::audio::AudioBuffer>(&decoded);
        if (audio == nullptr)
            return cancelled()
                       ? OpenedAssetAudio{{}, {}, {}, true}
                       : OpenedAssetAudio{{}, {}, std::get<media::provider::Unavailable>(decoded).detail, false};
        opened.buffer = std::make_shared<const media::audio::AudioBuffer>(std::move(*audio));
        opened.session = std::move(session);
    } else {
        return {{}, {}, "The source has no audio", false};
    }
    return opened;
}
} // namespace detail

namespace {

namespace an = media::audio::analysis;


struct Entry final {
    std::shared_ptr<const AudioAnalysisResult> result;
    std::size_t bytes = 0;
    std::uint64_t used = 0;
};

struct Opened final {
    std::shared_ptr<const media::audio::AudioBuffer> buffer;
    // A video container's audio decodes through its session's retained worker.
    std::shared_ptr<media::video::VideoDecodeSession> session;
    std::uint64_t used = 0;
};

struct State final {
    std::mutex mutex;
    std::map<std::string, Entry> entries;
    std::map<std::string, Opened> opened;
    std::size_t bytes = 0;
    std::uint64_t tick = 0;
    AudioAnalysisStatistics statistics;
    // One source is opened at a time: opening a file hashes and indexes it.
    std::mutex openMutex;
};

State& state() {
    static State instance;
    return instance;
}

std::string number(const double value) {
    char text[40];
    std::snprintf(text, sizeof text, "%a", value);
    return text;
}

std::string time(const core::RationalTime value) {
    return std::to_string(value.numerator()) + "/" + std::to_string(value.denominator());
}

[[nodiscard]] std::optional<core::RationalTime> startTime(const std::int64_t frame,
                                                          const document::FrameRate rate) noexcept {
    return core::RationalTime::create(
        frame * static_cast<std::int64_t>(rate.denominator()),
        static_cast<std::int64_t>(rate.numerator()));
}

std::string sourceKey(const AudioAnalysisSource& source) {
    const auto hex = source.asset.contentDigest.toLowercaseHex();
    std::string key(hex.begin(), hex.end());
    key += "|s" + std::to_string(source.audioStream);
    return key;
}

[[nodiscard]] std::size_t resultBytes(const AudioAnalysisResult& result) noexcept {
    return sizeof(AudioAnalysisResult) + result.values.size() * sizeof(float) + 256;
}

struct OpenResult final {
    std::shared_ptr<const media::audio::AudioBuffer> buffer;
    std::string error;
    bool cancelled = false;
};

OpenResult openSource(const AudioAnalysisSource& source, const std::filesystem::path& base,
                      const CancellationToken& cancellation) {
    auto& s = state();
    const auto key = sourceKey(source);
    std::scoped_lock open(s.openMutex);
    {
        std::scoped_lock lock(s.mutex);
        const auto found = s.opened.find(key);
        if (found != s.opened.end()) {
            found->second.used = ++s.tick;
            return {found->second.buffer, {}, false};
        }
    }
    const auto loaded = detail::openAssetAudio(
        source.asset, source.audioStream, base,
        [&] { return cancellation.isCancellationRequested(); });
    if (!loaded.buffer)
        return {{}, loaded.error, loaded.cancelled};
    Opened opened;
    opened.buffer = loaded.buffer;
    opened.session = loaded.session;
    std::scoped_lock lock(s.mutex);
    opened.used = ++s.tick;
    ++s.statistics.opened;
    s.opened.insert_or_assign(key, opened);
    while (s.opened.size() > kAudioAnalysisOpenSources) {
        auto oldest = s.opened.begin();
        for (auto it = s.opened.begin(); it != s.opened.end(); ++it)
            if (it->second.used < oldest->second.used)
                oldest = it;
        s.opened.erase(oldest);
    }
    return {opened.buffer, {}, false};
}

} // namespace

std::optional<double> audioAnalysisSourceSeconds(const AudioAnalysisSource& source,
                                                 const core::RationalTime at) {
    const auto start = startTime(source.startFrame, source.compositionRate);
    if (!start)
        return std::nullopt;
    auto t = at;
    if (source.loopExtent) {
        const auto wrapped = wrapSourceAudioTime(t, *start, *source.loopExtent, source.loopMode);
        if (!wrapped)
            return std::nullopt;
        t = *wrapped;
    }
    if (source.footage) {
        const auto mapped = detail::footageAudioTime(*source.footage, *start, t);
        if (!mapped)
            return std::nullopt;
        t = *mapped;
    }
    return t.toSeconds() - start->toSeconds();
}

std::optional<AudioAnalysisQuery> audioAmplitudeQuery(const CompiledValueAudioAmplitude& kernel,
                                                      const core::RationalTime time,
                                                      const double windowMs,
                                                      const double offsetSeconds) {
    if (!std::isfinite(windowMs) || !std::isfinite(offsetSeconds) || windowMs < 1.0 ||
        windowMs > 60000.0 || std::abs(offsetSeconds) > 86400.0)
        return std::nullopt;
    // The offset in whole milliseconds, added to the time exactly.
    const auto millis = static_cast<std::int64_t>(std::llround(offsetSeconds * 1000.0));
    const core::Int128 numerator =
        core::Int128(time.numerator()) * 1000 + core::Int128(millis) * time.denominator();
    const core::Int128 denominator = core::Int128(time.denominator()) * 1000;
    constexpr core::Int128 limit = std::numeric_limits<std::int64_t>::max();
    if (numerator > limit || numerator < -limit || denominator > limit)
        return std::nullopt;
    const auto shifted = core::RationalTime::create(static_cast<std::int64_t>(numerator),
                                                    static_cast<std::int64_t>(denominator));
    if (!shifted)
        return std::nullopt;
    AudioAnalysisQuery query;
    query.source = kernel.source;
    query.kind = AudioAnalysisKind::Amplitude;
    query.time = *shifted;
    query.windowSeconds = windowMs / 1000.0;
    query.measure = kernel.measure == 0 ? media::audio::analysis::Measure::Peak
                                        : media::audio::analysis::Measure::Rms;
    return query;
}

namespace {
void appendSourceTiming(std::string& key, const AudioAnalysisSource& source) {
    key += "|start" + std::to_string(source.startFrame) + "@" +
           std::to_string(source.compositionRate.numerator()) + "/" +
           std::to_string(source.compositionRate.denominator());
    key += "|loop" + std::to_string(source.loopMode);
    if (source.loopExtent)
        key += "x" + time(source.loopExtent->duration) + "@" +
               std::to_string(source.loopExtent->frameRate.numerator()) + "/" +
               std::to_string(source.loopExtent->frameRate.denominator());
    if (source.footage)
        key += "|foot" + time(source.footage->pass) + "x" + std::to_string(source.footage->loops) +
               "*" + std::to_string(source.footage->numerator) + "/" +
               std::to_string(source.footage->denominator);
}

void appendQueryParameters(std::string& key, const AudioAnalysisQuery& query) {
    switch (query.kind) {
    case AudioAnalysisKind::Amplitude:
        key += "|w" + number(query.windowSeconds) + "|m" + std::to_string(static_cast<int>(query.measure));
        break;
    case AudioAnalysisKind::Waveform:
        key += "|w" + number(query.windowSeconds) + "|c" + std::to_string(query.columns) + "|" +
               (query.separateChannels ? "sep" : "mix");
        break;
    case AudioAnalysisKind::Spectrum:
        key += "|c" + std::to_string(query.columns) + "|n" + std::to_string(query.spectrum.fftSize) +
               "|f" + number(query.spectrum.lowHz) + "-" + number(query.spectrum.highHz) +
               (query.spectrum.logarithmic ? "log" : "lin") + "|d" + number(query.spectrum.floorDb) +
               "," + number(query.spectrum.ceilingDb);
        break;
    }
}
} // namespace

std::string audioAnalysisKey(const AudioAnalysisQuery& query) {
    if (!query.source)
        return {};
    const auto& source = *query.source;
    std::string key = "audio-analysis/1|k" + std::to_string(static_cast<int>(query.kind)) + "|";
    if (!source.bare) {
        // #630: a rendered sound is named by its digest, which a worker computes; until then the
        // analysis has no identity and is not prepared.
        const auto digest = source.sound && source.sound->identity
                                ? source.sound->identity->digest()
                                : std::string{};
        if (digest.empty())
            return {};
        key += "sound:" + digest;
    } else {
        key += sourceKey(source);
        appendSourceTiming(key, source);
    }
    key += "|t" + time(query.time);
    appendQueryParameters(key, query);
    return key;
}

std::shared_ptr<const AudioAnalysisResult> findAudioAnalysis(const AudioAnalysisQuery& query) {
    const auto key = audioAnalysisKey(query);
    if (key.empty())
        return nullptr;
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto found = s.entries.find(key);
    if (found == s.entries.end())
        return nullptr;
    found->second.used = ++s.tick;
    ++s.statistics.hits;
    return found->second.result;
}

AudioAnalysisStatus prepareAudioAnalysis(const AudioAnalysisQuery& query,
                                         const std::filesystem::path& assetBase,
                                         const CancellationToken& cancellation,
                                         const std::shared_ptr<const CompiledCompositionPlan>& plan) {
    auto& s = state();
    if (!query.source)
        return {AudioAnalysisOutcome::Unavailable, "No audio is connected"};
    // #630: more than a bare source is read as it is rendered. The render is the sound store's
    // job; this waits for it and then reads the window from the rendered samples.
    std::shared_ptr<const PreparedAudioSound> rendered;
    if (!query.source->bare) {
        // A caller that holds no plan of its own (a plot pass) reads the plan of the evaluation
        // it runs inside.
        const auto& owner = plan ? plan : AudioAnalysisPreparation::planHere();
        auto prepared = prepareAudioSound(owner, query.source->sound, assetBase, cancellation);
        if (prepared.outcome == AudioSoundOutcome::Cancelled)
            return {AudioAnalysisOutcome::Cancelled, {}};
        if (prepared.outcome != AudioSoundOutcome::Ready || !prepared.sound)
            return {AudioAnalysisOutcome::Unavailable,
                    prepared.detail.empty() ? "The audio could not be rendered" : prepared.detail};
        rendered = std::move(prepared.sound);
    }
    const auto key = audioAnalysisKey(query);
    if (key.empty())
        return {AudioAnalysisOutcome::Unavailable, "No audio is connected"};
    if (cancellation.isCancellationRequested())
        return {AudioAnalysisOutcome::Cancelled, {}};
    {
        std::scoped_lock lock(s.mutex);
        const auto found = s.entries.find(key);
        if (found != s.entries.end()) {
            found->second.used = ++s.tick;
            ++s.statistics.hits;
            return {AudioAnalysisOutcome::Ready, {}};
        }
    }
    const auto fail = [&](std::string detail) {
        std::scoped_lock lock(s.mutex);
        ++s.statistics.failed;
        return AudioAnalysisStatus{AudioAnalysisOutcome::Unavailable, std::move(detail)};
    };
    const auto cancel = [&]() {
        std::scoped_lock lock(s.mutex);
        ++s.statistics.cancelled;
        return AudioAnalysisStatus{AudioAnalysisOutcome::Cancelled, {}};
    };
    if (!(query.windowSeconds > 0.0) || query.windowSeconds > 60.0)
        return fail("The analysis window is outside 0 to 60 seconds");
    std::shared_ptr<const media::audio::AudioBuffer> held;
    std::optional<double> seconds;
    if (rendered) {
        // The rendered sound runs on the composition's clock from its start.
        held = rendered->buffer;
        const double at = query.time.toSeconds();
        if (at >= 0.0 && at * held->rate < static_cast<double>(held->frames))
            seconds = at;
    } else {
        const auto opened = openSource(*query.source, assetBase, cancellation);
        if (opened.cancelled)
            return cancel();
        if (!opened.buffer)
            return fail(opened.error.empty() ? "The audio could not be opened" : opened.error);
        held = opened.buffer;
        seconds = audioAnalysisSourceSeconds(*query.source, query.time);
    }
    const auto& buffer = *held;
    const auto center = seconds ? static_cast<std::int64_t>(std::llround(*seconds * buffer.rate)) : 0;
    const auto frames = static_cast<std::uint64_t>(
        std::max<long long>(1, std::llround(query.windowSeconds * buffer.rate)));
    const media::audio::CancelAudioWork cancelled = [&] {
        return cancellation.isCancellationRequested();
    };
    auto result = std::make_shared<AudioAnalysisResult>();
    const auto first = center - static_cast<std::int64_t>(frames / 2);
    try {
        switch (query.kind) {
        case AudioAnalysisKind::Amplitude: {
            if (!seconds) {
                result->amplitude = 0.0;
                break;
            }
            const auto value = an::amplitude(buffer, first, frames, query.measure, cancelled);
            if (!value) {
                if (value.error()->code == media::audio::AudioErrorCode::Cancelled)
                    return cancel();
                return fail(media::audio::describeAudioError(*value.error()));
            }
            result->amplitude = *value.value();
            break;
        }
        case AudioAnalysisKind::Waveform: {
            const std::uint32_t lanes =
                query.separateChannels ? std::min(buffer.channels, an::kMaximumLanes) : 1U;
            if (!seconds) {
                result->lanes = lanes;
                result->columns = query.columns;
                result->values.assign(static_cast<std::size_t>(query.columns) * lanes * 2, 0.0F);
                break;
            }
            const auto columns = an::waveform(buffer, first, frames, query.columns,
                                              query.separateChannels, cancelled);
            if (!columns) {
                if (columns.error()->code == media::audio::AudioErrorCode::Cancelled)
                    return cancel();
                return fail(media::audio::describeAudioError(*columns.error()));
            }
            result->lanes = columns.value()->lanes;
            result->columns = columns.value()->columns;
            result->values = std::move(columns.value()->values);
            break;
        }
        case AudioAnalysisKind::Spectrum: {
            result->lanes = 1;
            result->columns = query.columns;
            if (!seconds) {
                result->values.assign(query.columns, 0.0F);
                break;
            }
            const auto levels = an::spectrum(buffer, center, query.spectrum, query.columns, cancelled);
            if (!levels) {
                if (levels.error()->code == media::audio::AudioErrorCode::Cancelled)
                    return cancel();
                return fail(media::audio::describeAudioError(*levels.error()));
            }
            result->values = std::move(*levels.value());
            break;
        }
        }
    } catch (const std::bad_alloc&) {
        return fail("There is not enough memory to analyze the audio");
    }
    if (cancellation.isCancellationRequested())
        return cancel();
    const auto bytes = resultBytes(*result);
    std::scoped_lock lock(s.mutex);
    ++s.statistics.computed;
    s.entries.insert_or_assign(key, Entry{result, bytes, ++s.tick});
    s.bytes = 0;
    for (const auto& [name, entry] : s.entries)
        s.bytes += entry.bytes;
    // Retire the least recently used entries beyond the budget; the one just prepared stays.
    while (s.bytes > kAudioAnalysisCacheBytes && s.entries.size() > 1) {
        auto oldest = s.entries.end();
        for (auto it = s.entries.begin(); it != s.entries.end(); ++it)
            if (it->first != key && (oldest == s.entries.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == s.entries.end())
            break;
        s.bytes -= oldest->second.bytes;
        s.entries.erase(oldest);
        ++s.statistics.evicted;
    }
    return {AudioAnalysisOutcome::Ready, {}};
}

namespace {
thread_local const AudioAnalysisPreparation* currentPreparation = nullptr;
} // namespace

AudioAnalysisPreparation::AudioAnalysisPreparation(
    std::shared_ptr<const CompiledCompositionPlan> plan, std::filesystem::path assetBase,
    CancellationToken cancellation)
    : plan_(std::move(plan)), assetBase_(std::move(assetBase)),
      cancellation_(std::move(cancellation)), previous_(currentPreparation) {
    currentPreparation = this;
}

AudioAnalysisPreparation::~AudioAnalysisPreparation() { currentPreparation = previous_; }

const std::shared_ptr<const CompiledCompositionPlan>& AudioAnalysisPreparation::planHere() noexcept {
    static const std::shared_ptr<const CompiledCompositionPlan> none;
    return currentPreparation == nullptr ? none : currentPreparation->plan_;
}

std::optional<AudioAnalysisStatus> AudioAnalysisPreparation::prepareHere(const AudioAnalysisQuery& query) {
    if (currentPreparation == nullptr)
        return std::nullopt;
    return prepareAudioAnalysis(query, currentPreparation->assetBase_,
                                currentPreparation->cancellation_, currentPreparation->plan_);
}

AudioAnalysisStatistics audioAnalysisStatistics() {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    auto copy = s.statistics;
    copy.entries = s.entries.size();
    copy.bytes = s.bytes;
    copy.openSources = s.opened.size();
    return copy;
}

void clearAudioAnalysis() {
    auto& s = state();
    std::scoped_lock open(s.openMutex);
    std::scoped_lock lock(s.mutex);
    s.entries.clear();
    s.opened.clear();
    s.bytes = 0;
    s.statistics = {};
}

} // namespace bloom::runtime

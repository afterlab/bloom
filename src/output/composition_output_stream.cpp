#include <algorithm>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/media/audio/loudness.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/output/composition_output_stream.hpp>
#include <bloom/render/processing_host_storage.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace bloom::output {
namespace {
using namespace media::provider;
using AudioEngine = media::audio::playback::AudioEngine;
using NullBackend = media::audio::playback::NullBackend;
void require(bool condition, const char* detail, Error code = Error::InvalidValue) {
    if (!condition)
        throw Unavailable{code, detail};
}
void checked(const std::optional<Unavailable>& error) {
    if (error)
        throw Unavailable{error->reason, error->detail};
}
template <typename T> T checked(Result<T> result) {
    if (const auto* error = std::get_if<Unavailable>(&result))
        throw Unavailable{error->reason, error->detail};
    return std::get<T>(std::move(result));
}
template <typename T> T required(std::optional<T> value, const char* message) {
    if (!value)
        throw Unavailable{Error::InvalidValue, message};
    return std::move(*value);
}
Rational fraction(std::uint64_t numerator, std::uint64_t denominator) {
    const auto gcd = std::gcd(numerator, denominator);
    return {static_cast<std::int64_t>(numerator / gcd),
            static_cast<std::int64_t>(denominator / gcd)};
}

static_assert(static_cast<int>(runtime::AudioEffectKind::Equalizer) ==
                  static_cast<int>(media::audio::effects::EffectKind::Equalizer) &&
              static_cast<int>(runtime::AudioEffectKind::Compressor) ==
                  static_cast<int>(media::audio::effects::EffectKind::Compressor) &&
              static_cast<int>(runtime::AudioEffectKind::AutoLevel) ==
                  static_cast<int>(media::audio::effects::EffectKind::AutoLevel) &&
              static_cast<int>(runtime::AudioEffectKind::Mono) ==
                  static_cast<int>(media::audio::effects::EffectKind::Mono) &&
              static_cast<int>(runtime::AudioEffectKind::Hiss) ==
                  static_cast<int>(media::audio::effects::EffectKind::Hiss));

// #569: a clip's effect chain for the mixer, with the parameters at the block's end when they differ.
[[nodiscard]] std::vector<media::audio::effects::EffectSettings>
effectSettings(const runtime::AudioClipDescription& start, const runtime::AudioClipDescription* end,
               bool& moves) {
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

struct AudioBackingBytes final {
    std::uint64_t allocated = 0;
    std::uint64_t initialized = 0;
};

[[nodiscard]] std::optional<AudioBackingBytes>
measuredAudioBacking(const media::audio::AudioBuffer& buffer) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    AudioBackingBytes bytes{sizeof(buffer), sizeof(buffer)};
    const auto add = [](std::size_t count, std::size_t width, std::uint64_t& total) {
        if (count > (max - total) / width)
            return false;
        total += static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(width);
        return true;
    };
    if (!add(buffer.planes.capacity(), sizeof(std::vector<float>), bytes.allocated) ||
        !add(buffer.planes.size(), sizeof(std::vector<float>), bytes.initialized))
        return {};
    for (const auto& plane : buffer.planes) {
        if (!add(plane.capacity(), sizeof(float), bytes.allocated) ||
            !add(plane.size(), sizeof(float), bytes.initialized))
            return {};
    }
    // A windowed buffer's samples live in its stream (the playback window), not in planes.
    if (buffer.stream) {
        const auto streamBytes = buffer.stream->retainedBytes();
        if (streamBytes > max - bytes.allocated)
            return {};
        bytes.allocated += streamBytes;
        bytes.initialized += streamBytes;
    }
    return bytes;
}

struct OwnedAudioBuffer final {
    // The ledger reservation must retire after every buffer owner, including mixer clips.
    std::shared_ptr<ExportResourceReservationV1> reservation;
    std::shared_ptr<const media::audio::AudioBuffer> buffer;
};
} // namespace
struct CompositionOutputStreamV1::State {
    CompositionOutputSourceV1 source;
    EncodeSettingsV1 encoding;
    std::shared_ptr<ExportResourceReservationV1> queueReservation;
    platform::ProcessCancellation cancellation;
    std::unique_ptr<EncodeSessionV1> encoder;
    std::map<runtime::AudioStreamKey, OwnedAudioBuffer> audioBuffers;
    bool started = false;
    // An audio source failed to read or decode (#428); see loadSource.
    bool audioSourceFailed = false;
    State(CompositionOutputSourceV1 input, EncodeSettingsV1 settings, EncodeSessionOptionsV1 worker,
          std::shared_ptr<ExportResourceReservationV1> reservation,
          platform::ProcessCancellation cancel)
        : source(std::move(input)), encoding(std::move(settings)),
          queueReservation(std::move(reservation)), cancellation(std::move(cancel)),
          encoder(std::make_unique<EncodeSessionV1>(std::move(worker), cancellation)) {}
    const EncodeSettingsV1& settings() const { return encoding; }
    void checkpoint() const {
        require(!cancellation || !cancellation(), "Composition output cancelled", Error::Cancelled);
    }
    // Decodes one audio source for the offline mix. A read or decode failure names the source
    // and its codec and marks the audio source stage (#428), so the job does not blame the
    // encoder; cancellation and admission refusals keep their own meaning.
    std::map<runtime::AudioStreamKey, OwnedAudioBuffer>::iterator
    loadSource(runtime::TaskContext& context, const runtime::AudioClipDescription& description,
               const media::video::Cancel& cancel) {
        const runtime::AudioStreamKey key{description.assetId, description.audioStream};
        std::string codec;
        try {
            const auto* asset = source.snapshot.project().findAsset(description.assetId);
            require(asset != nullptr, "Audio source asset is missing", Error::Unavailable);
            const auto path = media::resolveImagePath(
                asset->locator.path, asset->locator.relinkHint, source.assetBaseDirectory);
            // On every failure path the decoder retires before its scratch reservation.
            std::shared_ptr<ExportResourceReservationV1> retainedClaim;
            std::shared_ptr<ExportResourceReservationV1> scratchClaim;
            const auto decoder = std::make_shared<media::video::VideoDecodeSession>(path);
            const auto probe = checked(decoder->probe(cancel));
            require(probe.sourceDigest == asset->contentDigest, "Audio source changed",
                    Error::SourceChanged);
            const auto* stream = media::video::audioStreamAt(probe, description.audioStream);
            require(stream != nullptr && stream->duration.denominator > 0,
                    "Audio stream unavailable", Error::Unavailable);
            codec = stream->codec;
            // Only the playback window is resident (#329), however long the audio is: the clip
            // decodes forward through this session's retained worker as the mix reads it.
            const auto audioProbe = media::video::videoAudioProbe(*stream);
            require(audioProbe.has_value(), "Audio source sample count is not representable",
                    Error::Unavailable);
            constexpr std::uint64_t decoderAllowance = 256U * 1024U;
            const std::uint64_t windowBytes = static_cast<std::uint64_t>(
                                                  media::video::kVideoAudioWindowFrames) *
                                              audioProbe->channels * sizeof(float);
            const std::uint64_t retained = windowBytes + decoderAllowance +
                                           sizeof(media::audio::AudioBuffer) +
                                           sizeof(media::audio::AudioStream) +
                                           audioProbe->channels * sizeof(std::vector<float>);
            const std::uint64_t scratchBytes = windowBytes;
            // #586: every refusal below is memory or processing capacity, reported as Busy. Only
            // an output that outgrows its byte limit is Oversized, which the job reports as a
            // full destination.
            if (queueReservation->processingPolicy()) {
                ExportResourceAdmissionStatusV1 status{};
                retainedClaim = queueReservation->reserveOwnedHostBacking(retained, status,
                                                                          context.cancellation());
                require(retainedClaim != nullptr,
                        "Audio backing admission refused: not enough free memory or processing "
                        "capacity for the decoded audio",
                        Error::Busy);
                scratchClaim =
                    queueReservation->reserveScratch(scratchBytes, status, context.cancellation());
                require(scratchClaim != nullptr,
                        "Audio decode scratch admission refused: not enough free memory or "
                        "processing capacity to decode the audio",
                        Error::Busy);
            } else {
                // Legacy callers without a processing root retain the existing ledger
                // envelope and cannot claim physical-sample backing credit.
                const auto total = retained + scratchBytes;
                require(total <= std::numeric_limits<std::uint64_t>::max() -
                                     queueReservation->chargedBytes(),
                        "Audio export reservation overflows", Error::Busy);
                require(queueReservation->expand(queueReservation->chargedBytes() + total) ==
                            ExportResourceAdmissionStatusV1::Reserved,
                        "Audio decode ledger reservation refused: not enough free memory for "
                        "the decoded audio",
                        Error::Busy);
            }
            auto buffer = checked(
                media::video::openWindowedVideoAudio(decoder, probe, description.audioStream));
            auto backing = std::make_unique<media::audio::AudioBuffer>(std::move(buffer));
            std::shared_ptr<const media::audio::AudioBuffer> published;
            if (retainedClaim) {
                const auto measured = measuredAudioBacking(*backing);
                require(measured.has_value(), "Audio backing byte count overflows", Error::Busy);
                published = render::adoptProcessingHostBacking(
                    std::move(backing), retainedClaim->ownedHostPhase(), measured->allocated,
                    measured->initialized);
                require(published != nullptr, "Audio backing exceeds its admitted allocation",
                        Error::Busy);
            } else {
                published = std::shared_ptr<const media::audio::AudioBuffer>(std::move(backing));
            }
            scratchClaim.reset();
            return audioBuffers
                .emplace(key, OwnedAudioBuffer{std::move(retainedClaim), std::move(published)})
                .first;
        } catch (const Unavailable& error) {
            if (error.reason == Error::Cancelled || error.reason == Error::Oversized ||
                error.reason == Error::Busy)
                throw;
            audioSourceFailed = true;
            throw Unavailable{error.reason,
                              "Audio asset " + std::to_string(description.assetId.value()) +
                                  (codec.empty() ? "" : " (" + codec + ")") +
                                  " couldn't be decoded for the export (" + error.detail + ")."};
        }
    }
    // One run of the offline mix over the programme, from the export's origin: the mixer, the clock
    // and the mix evaluated at the end of the last block (reused as the next block's start mix).
    // Pass one of a loudness export only measures; pass two is what the encoder is given.
    struct MixPass final {
        std::unique_ptr<AudioEngine> mixer;
        NullBackend* sink = nullptr;
        std::uint64_t position = 0;
        std::optional<runtime::AudioMixDescription> blockEndMix;
    };
    ~State() {
        // AudioEngine keeps shared clip buffers. Drop those before their ledger owners.
        pass.mixer.reset();
        audioBuffers.clear();
    }
    MixPass pass;
    // #570: the loudness stage of an export that normalizes the programme.
    std::optional<media::audio::loudness::Meter> measuredAfter;
    std::unique_ptr<media::audio::loudness::Limiter> limiter;
    std::optional<AudioLoudnessReportV1> loudnessResult;
    bool loudnessMeasured = false;
    bool limiterFlushed = false;
    std::uint64_t emitted = 0;

    void startPass(MixPass& next) {
        auto backend = std::make_unique<NullBackend>();
        next.sink = backend.get();
        next.mixer = std::make_unique<AudioEngine>(
            std::move(backend),
            AudioEngine::Config{settings().sampleRate, settings().channels, 16384});
        next.position = 0;
        next.blockEndMix.reset();
        require(!next.mixer->play(source.origin), "Offline mixer failed");
    }
    // Renders the next `count` frames of the programme: interleaved stereo float.
    std::vector<float> renderBlock(MixPass& run, runtime::TaskContext& context,
                                   const std::uint32_t count) {
        const auto cancel = [&] {
            return (cancellation && cancellation()) || context.isCancellationRequested();
        };
        const auto sampleTime = [&](const std::uint64_t sample) {
            const auto origin = source.origin;
            return core::RationalTime::create(origin.numerator() * settings().sampleRate +
                                                  static_cast<std::int64_t>(sample) *
                                                      origin.denominator(),
                                              origin.denominator() * settings().sampleRate);
        };
        const auto time = required(sampleTime(run.position), "Audio sample timestamp overflow");
        const auto mix = run.blockEndMix
                             ? std::move(*run.blockEndMix)
                             : required(runtime::CpuCompositionEvaluator{}.evaluateAudioMix(
                                            source.plan, time, context.cancellation()),
                                        "Composition audio graph is unavailable");
        run.blockEndMix.reset();
        // Level automation ramps linearly from this block's start to its end (#459), so a
        // fade is continuous instead of stepping once per block. Without an end mix (for
        // example past a representable time) the block holds its start level.
        const auto endTime = sampleTime(run.position + count);
        std::optional<runtime::AudioMixDescription> endMix;
        if (endTime)
            endMix = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(source.plan, *endTime,
                                                                         context.cancellation());
        const bool ramped = endMix && endMix->clips.size() == mix.clips.size();
        std::vector<media::audio::playback::AudioClip> clips;
        clips.reserve(mix.clips.size());
        for (std::size_t index = 0; index < mix.clips.size(); ++index) {
            const auto& description = mix.clips[index];
            auto found = audioBuffers.find({description.assetId, description.audioStream});
            if (found == audioBuffers.end())
                found = loadSource(context, description, cancel);
            require(found->second.buffer != nullptr, "Audio source buffer unavailable",
                    Error::Unavailable);
            clips.push_back({.buffer = found->second.buffer,
                             .startTime = description.startTime,
                             .level = static_cast<float>(description.level),
                             .muted = description.muted,
                             .solo = description.solo,
                             .endTime = description.endTime,
                             .mapTime = {}});
            if (!description.timeMappings.empty())
                clips.back().mapTime = [description](core::RationalTime at) {
                    return runtime::mapAudioClipTime(description, at);
                };
            if (!description.effects.empty()) {
                bool moves = false;
                clips.back().effectKey = description.sourceNodeId.value() * 1'000'003ULL + index;
                clips.back().effects = effectSettings(
                    description, ramped ? &endMix->clips[index] : nullptr, moves);
                if (moves)
                    clips.back().effectRamp =
                        media::audio::playback::AudioEffectRamp{time, *endTime};
            }
            if (ramped) {
                const auto& end = endMix->clips[index];
                if (end.sourceNodeId == description.sourceNodeId &&
                    end.assetId == description.assetId &&
                    end.audioStream == description.audioStream && end.level != description.level)
                    clips.back().levelRamp = media::audio::playback::AudioLevelRamp{
                        time, *endTime, static_cast<float>(end.level)};
            }
        }
        run.mixer->replaceClips(std::move(clips));
        run.sink->clearCaptured();
        require(!run.mixer->renderForTesting(count), "Offline mix failed");
        const auto captured = run.sink->captured();
        require(captured.size() == static_cast<std::size_t>(count) * 2U,
                "Offline mix sample count differs");
        run.position += count;
        run.blockEndMix = std::move(endMix);
        return {captured.begin(), captured.end()};
    }
    void deliver(const std::vector<float>& interleaved) {
        const auto count = interleaved.size() / 2;
        if (count == 0)
            return;
        AudioBlock block;
        block.sampleRate = settings().sampleRate;
        block.channelLayout = {"FL", "FR"};
        block.pts = fraction(emitted, settings().sampleRate);
        block.channels.resize(2, std::vector<float>(count));
        for (std::size_t i = 0; i < count; ++i)
            for (std::size_t c = 0; c < 2; ++c)
                block.channels[c][i] = interleaved[i * 2 + c];
        checked(encoder->audio(std::move(block)));
        emitted += count;
    }
    // #570 pass one: the whole programme through the same mixer, into the meter alone. The mix is
    // deterministic, so pass two hears exactly what was measured.
    void measureProgramme(runtime::TaskContext& context) {
        const auto total = settings().audioSamples;
        media::audio::loudness::Meter meter(settings().sampleRate, 2);
        require(meter.valid(), "Loudness measurement is unavailable for this sample rate",
                Error::Unavailable);
        MixPass measuring;
        startPass(measuring);
        std::uint64_t blocks = 0;
        while (measuring.position < total) {
            checkpoint();
            const auto count =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(512, total - measuring.position));
            const auto block = renderBlock(measuring, context, count);
            require(meter.add(block), "Loudness measurement failed");
            if (++blocks % 256 == 0)
                context.reportProgress({"Measuring loudness", "", measuring.position, total});
        }
        measuring.mixer.reset();
        const auto before = meter.measurement();
        const auto& wanted = *source.loudness;
        const media::audio::loudness::Target target{wanted.targetLufs, wanted.truePeakDbtp};
        AudioLoudnessReportV1 report;
        report.targetLufs = wanted.targetLufs;
        report.truePeakLimitDbtp = wanted.truePeakDbtp;
        report.measured = before.loudnessMeasured;
        report.measuredLufs = before.integratedLufs;
        report.measuredTruePeakDbtp = before.truePeakDbtp;
        report.gainDb = media::audio::loudness::gainToTarget(before, target);
        loudnessResult = report;
        limiter = std::make_unique<media::audio::loudness::Limiter>(
            settings().sampleRate, 2, report.gainDb, wanted.truePeakDbtp);
        measuredAfter.emplace(settings().sampleRate, 2);
        loudnessMeasured = true;
    }
    void audio(runtime::TaskContext& context, std::uint64_t through) {
        if (settings().audioCodec.empty())
            return;
        const bool normalize = source.loudness.has_value();
        if (normalize && !loudnessMeasured)
            measureProgramme(context);
        if (!pass.mixer)
            startPass(pass);
        const auto total = settings().audioSamples;
        // The limiter looks ahead, so the mix runs that far past what the encoder is owed.
        const auto wanted = normalize ? std::min<std::uint64_t>(total, through + limiter->latencyFrames())
                                      : through;
        std::vector<float> limited;
        while (pass.position < wanted) {
            checkpoint();
            const auto count = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(512, wanted - pass.position));
            const auto block = renderBlock(pass, context, count);
            if (!normalize) {
                deliver(block);
                continue;
            }
            limited.clear();
            limiter->process(block, limited);
            require(measuredAfter->add(limited), "Loudness measurement failed");
            deliver(limited);
        }
        if (normalize && !limiterFlushed && pass.position >= total) {
            limited.clear();
            limiter->flush(limited);
            require(measuredAfter->add(limited), "Loudness measurement failed");
            deliver(limited);
            limiterFlushed = true;
            const auto after = measuredAfter->measurement();
            loudnessResult->finalMeasured = after.loudnessMeasured;
            loudnessResult->finalLufs = after.integratedLufs;
            loudnessResult->finalTruePeakDbtp = after.truePeakDbtp;
            loudnessResult->limiterReductionDb = limiter->maximumReductionDb();
        }
    }
};
CompositionOutputStreamV1::CompositionOutputStreamV1(
    CompositionOutputSourceV1 source, EncodeSettingsV1 settings, EncodeSessionOptionsV1 worker,
    std::shared_ptr<ExportResourceReservationV1> reservation, platform::ProcessCancellation cancel)
    : state_(std::make_unique<State>(std::move(source), std::move(settings), std::move(worker),
                                     std::move(reservation), std::move(cancel))) {}
CompositionOutputStreamV1::~CompositionOutputStreamV1() = default;
std::optional<Unavailable> CompositionOutputStreamV1::writeFrame(const render::Rgba32fImage& image,
                                                                 std::uint64_t frame,
                                                                 std::uint64_t samplesThrough,
                                                                 runtime::TaskContext& context) {
    try {
        auto& s = *state_;
        s.checkpoint();
        if (!s.started) {
            checked(s.encoder->begin(s.encoding));
            s.started = true;
        }
        if (!s.encoding.videoCodec.empty()) {
            auto prepared = checked(prepareMediaRgba16V1(
                image,
                fraction(frame * static_cast<std::uint64_t>(s.encoding.rate.denominator),
                         static_cast<std::uint64_t>(s.encoding.rate.numerator)),
                s.cancellation, s.source.display.get()));
            // The Profiler's encode stage (#423): handing the frame to the encoder worker.
            core::telemetry::StageTimer encodeTime(core::telemetry::Stage::Encode);
            checked(s.encoder->video(std::move(prepared)));
        }
        s.audio(context, samplesThrough);
        return {};
    } catch (const Unavailable& error) {
        return error;
    }
}
std::optional<Unavailable> CompositionOutputStreamV1::writePreparedFrame(
    FrameProduct prepared, const std::uint64_t samplesThrough, runtime::TaskContext& context) {
    try {
        auto& s = *state_;
        s.checkpoint();
        if (!s.started) {
            checked(s.encoder->begin(s.encoding));
            s.started = true;
        }
        require(!s.encoding.videoCodec.empty(), "Prepared frames need a video stream");
        {
            core::telemetry::StageTimer encodeTime(core::telemetry::Stage::Encode);
            checked(s.encoder->video(std::move(prepared)));
        }
        s.audio(context, samplesThrough);
        return {};
    } catch (const Unavailable& error) {
        return error;
    }
}
Result<EncodeQcV1> CompositionOutputStreamV1::finish() { return state_->encoder->finish(); }
Result<EncodedChunkV1> CompositionOutputStreamV1::read(std::uint64_t offset) {
    return state_->encoder->read(offset);
}
std::optional<Unavailable> CompositionOutputStreamV1::close() { return state_->encoder->close(); }
std::optional<AudioLoudnessReportV1> CompositionOutputStreamV1::loudnessReport() const {
    return state_->loudnessResult;
}
bool CompositionOutputStreamV1::audioSourceFailed() const noexcept {
    return state_->audioSourceFailed;
}
bool CompositionOutputStreamV1::platformEncoderRefused() const noexcept {
    return state_->encoder->platformEncoderRefused();
}
const Handshake& CompositionOutputStreamV1::handshake() const {
    return state_->encoder->handshake();
}
EncodeWorkerMemoryV1 CompositionOutputStreamV1::workerMemory() const {
    return state_->encoder->workerMemory();
}
const MediaProviderProvenanceV1& CompositionOutputStreamV1::provenance() const {
    return state_->encoder->provenance();
}
Result<MediaQcEvidenceV1> makeMediaQcEvidenceV1(const MediaOutputAnalysisV1& analysis,
                                                const EncodeQcV1& qc, core::Sha256Digest approval,
                                                const Handshake* served) {
    try {
        MediaQcEvidenceV1 evidence;
        evidence.artifact = qc.artifact;
        evidence.colourTags = qc.colour;
        evidence.container = qc.container;
        evidence.artifactBytes = qc.bytes;
        evidence.snapshot = approval;
        evidence.approval = evidence.snapshot;
        evidence.preset = analysis.digest;
        const bool hardware =
            analysis.settings.videoCodec == "h264" &&
            analysis.implementationNote.find("encoder=vaapi") != std::string::npos;
        // The provider that actually served the attempt (ADR 0035); without one, the FFmpeg
        // worker's declaration for the analysed route.
        const auto hello = served != nullptr && !served->declarations.empty()
                               ? *served
                               : ffmpegHandshake(hardware);
        const bool videotoolbox = hello.execution.provider == "bloom.videotoolbox";
        evidence.execution = checked(digest(hello.execution));
        // Bind all declared ordered export components; no delivery or independent-reader claim.
        PipelineQualificationV1 pipeline;
        pipeline.purpose = Purpose::Export;
        pipeline.profile = "media4-export-v1";
        pipeline.determinism = analysis.determinism;
        pipeline.toleranceProfile = analysis.toleranceProfile;
        pipeline.reopenPolicy = "same-provider-not-independent";
        pipeline.qcProfile = "first-last-layout-time-pcm-v1";
        pipeline.conversionVersions = {"rgba16-ocio-display-v1", "audio-engine-mix-v1"};
        pipeline.result = QcResult::Pass;
        for (const auto role :
             {Role::VideoEncode, Role::AudioEncode, Role::Mux, Role::ReopenDecode}) {
            if ((role == Role::VideoEncode && analysis.settings.videoCodec.empty()) ||
                (role == Role::AudioEncode && analysis.settings.audioCodec.empty()))
                continue;
            const auto matches = [&](const auto& d) {
                return d.capability.role == role &&
                       (role == Role::VideoEncode
                            ? mediaCodecFamilyV1(d.capability.codec) ==
                                      mediaCodecFamilyV1(analysis.settings.videoCodec) &&
                                  d.capability.profile == analysis.settings.profile
                        : role == Role::AudioEncode
                            ? d.capability.codec == analysis.settings.audioCodec
                            : d.capability.container == analysis.settings.container);
            };
            auto found = std::ranges::find_if(hello.declarations, matches);
            require(found != hello.declarations.end(), "Export component not in pinned manifest",
                    Error::IdentityMismatch);
            pipeline.steps.push_back({checked(digest(found->capability)),
                                      checked(digest(found->execution)),
                                      checked(digest(found->evidence))});
        }
        evidence.pipeline = checked(digest(pipeline));
        evidence.tool = hello.execution.provider;
        // On macOS the version names the system encoder that ran (issue #455).
        evidence.version =
            videotoolbox ? std::string("VideoToolbox") + (qc.encoder.empty() ? "" : " " + qc.encoder)
                         : hello.execution.sdk;
        evidence.profile = "media4-export-v1";
        evidence.coverage = "all frame timestamps/count; stream layout; duration; first/last "
                            "pixels; every PCM sample; staged digest";
        evidence.result = QcResult::Pass;
        evidence.frameCount = qc.frames;
        evidence.audioSamples = qc.audioSamples;
        evidence.duration = qc.duration;
        evidence.firstFrame = qc.firstFrame;
        evidence.lastFrame = qc.lastFrame;
        evidence.audioDigest = qc.audio;
        evidence.determinism = analysis.determinism;
        evidence.toleranceProfile = analysis.toleranceProfile;
        evidence.implementationNote = analysis.implementationNote;
        // The ProRes note names the implementation that actually encoded the artifact.
        if (mediaCodecFamilyV1(analysis.settings.videoCodec) == "prores")
            evidence.implementationNote =
                videotoolbox ? kProResVideoToolboxExportNote : kProResFfmpegExportNote;
        evidence.maximumError = qc.maximumError;
        evidence.meanError = qc.meanError;
        evidence.percentileMaximumError = qc.percentileMaximumError;
        (void)checked(digest(evidence));
        return evidence;
    } catch (const Unavailable& error) {
        return error;
    }
}
} // namespace bloom::output

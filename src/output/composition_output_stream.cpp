#include <algorithm>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/media/audio/loudness.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/output/composition_output_stream.hpp>
#include <bloom/output/export_audio_sources.hpp>
#include <bloom/platform/source_store.hpp>
#include <bloom/runtime/audio_mix_render.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
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

// One audio source's decoded buffer and the claim that admitted its bytes. The mix and the mixer's
// clips share the buffer through an alias of this holder, so the claim, declared first, retires
// after the buffer, whoever drops the last owner (#765).
struct AdmittedAudioBuffer final {
    std::shared_ptr<ExportResourceReservationV1> claim;
    std::shared_ptr<const media::audio::AudioBuffer> buffer;
};

// #771: a codec that does not return the samples it was given, so its decoded sound can peak above
// what the limiter held.
[[nodiscard]] bool lossyAudioCodec(const std::string& codec) noexcept {
    return codec == "aac" || codec == "opus";
}
// The most trial encodes one export runs, and the margin each correction adds under the overshoot
// it measured, in dB.
constexpr std::uint32_t kTrialEncodes = 6;
constexpr double kTrialMarginDb = 0.05;
// A trial encode is the export's own audio settings without the picture: the bundled FFmpeg
// encoder, writing to memory, never to the export's staged file, and not reported as the export's
// worker.
[[nodiscard]] EncodeSessionOptionsV1 trialOptions(const EncodeSessionOptionsV1& worker) {
    auto trial = worker;
    trial.platformEncoder = PlatformEncoderV1::Skipped;
    trial.stagedOutput.reset();
    trial.launched = {};
    trial.sending = {};
    trial.memorySampler = {};
    return trial;
}
} // namespace
struct CompositionOutputStreamV1::State {
    CompositionOutputSourceV1 source;
    EncodeSettingsV1 encoding;
    std::shared_ptr<ExportResourceReservationV1> queueReservation;
    platform::ProcessCancellation cancellation;
    // #771: the worker options of the audio-only trial encodes (declared before `encoder`, which
    // takes the originals).
    EncodeSessionOptionsV1 trialWorker;
    std::unique_ptr<EncodeSessionV1> encoder;
    // #765: the audio sources the mix holds decoded now. Only a source a block plays is opened, and
    // one no block has played for a second of programme retires (releaseIdleAudio), so what the
    // export holds follows what the range plays, not the number of layers.
    struct ResidentAudio final {
        std::shared_ptr<const media::audio::AudioBuffer> buffer;
        // The mix pass, and the programme sample through which a block of it last played this.
        std::uint64_t pass = 0;
        std::uint64_t playedThrough = 0;
    };
    std::map<runtime::AudioStreamKey, ResidentAudio> audioBuffers;
    // Every audio source opened for the mix, for sourceRoutingNotes: its name and its decode
    // session, weak because the buffers own the sessions, with the notes it had when it retired.
    struct OpenedAudioSource final {
        std::string name;
        std::weak_ptr<media::video::VideoDecodeSession> decoder;
        std::vector<std::string> notes;
    };
    std::map<runtime::AudioStreamKey, OpenedAudioSource> audioSources;
    std::uint64_t passSerial = 0;
    bool started = false;
    // An audio source failed to read or decode (#428); see loadSource.
    bool audioSourceFailed = false;
    State(CompositionOutputSourceV1 input, EncodeSettingsV1 settings, EncodeSessionOptionsV1 worker,
          std::shared_ptr<ExportResourceReservationV1> reservation,
          platform::ProcessCancellation cancel)
        : source(std::move(input)), encoding(std::move(settings)),
          queueReservation(std::move(reservation)), cancellation(std::move(cancel)),
          trialWorker(trialOptions(worker)),
          encoder(std::make_unique<EncodeSessionV1>(std::move(worker), cancellation)) {}
    const EncodeSettingsV1& settings() const { return encoding; }
    void checkpoint() const {
        require(!cancellation || !cancellation(), "Composition output cancelled", Error::Cancelled);
    }
    // Decodes one audio source for the offline mix. A read or decode failure names the source
    // and its codec and marks the audio source stage (#428), so the job does not blame the
    // encoder; cancellation and admission refusals keep their own meaning.
    std::shared_ptr<const media::audio::AudioBuffer>
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
            decoder->setSourceStore(platform::processSourceStore(),
                                    exportAudioLocalCopiesV1(asset->interpretation));
            audioSources[key] = {path.filename().string(), decoder, {}};
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
            // #765: both claims are children of the export's own reservation. Under a processing
            // root they share its phase, so an audio source takes host bytes but never a request
            // slot of its own: the number of layers cannot exhaust the root's slots. Without a
            // root they count against the export's ledger allowance. Each returns its bytes when
            // it retires.
            const auto admit = [&](const char* refused, const std::uint64_t bytes,
                                   const char* purpose) {
                ExportResourceAdmissionStatusV1 status{};
                auto claim =
                    queueReservation->reserveScratch(bytes, status, context.cancellation());
                if (claim)
                    return claim;
                checkpoint();
                require(!context.isCancellationRequested(), "Composition output cancelled",
                        Error::Cancelled);
                ExportAudioAdmissionRefusalV1 refusal{.refused = refused,
                                                      .asset = description.assetId,
                                                      .bytes = bytes,
                                                      .purpose = purpose,
                                                      .status = status,
                                                      .allowance = queueReservation->jobAllowance(),
                                                      .serviceAllowance =
                                                          queueReservation->serviceAllowance(),
                                                      .hostMemory = std::nullopt,
                                                      .decodedSources = audioBuffers.size()};
                if (const auto policy = queueReservation->processingPolicy()) {
                    const auto capacity = policy->usableCapacity();
                    if (capacity.hostReady)
                        refusal.hostMemory =
                            ExportAudioHostMemoryV1{.freeBytes = capacity.hostFreeBytes,
                                                    .usableBytes = capacity.hostUsableBytes};
                }
                throw Unavailable{Error::Busy, describeExportAudioAdmissionRefusalV1(refusal)};
            };
            retainedClaim =
                admit("Audio backing admission refused", retained, "its decoded audio window");
            scratchClaim =
                admit("Audio decode scratch admission refused", scratchBytes, "decoding its audio");
            auto buffer = checked(
                media::video::openWindowedVideoAudio(decoder, probe, description.audioStream));
            auto backing = std::make_shared<const media::audio::AudioBuffer>(std::move(buffer));
            const auto measured = measuredAudioBacking(*backing);
            require(measured.has_value() && measured->allocated <= retained,
                    "Audio backing exceeds its admitted allocation", Error::Busy);
            scratchClaim.reset();
            auto holder = std::make_shared<AdmittedAudioBuffer>(
                AdmittedAudioBuffer{retainedClaim, std::move(backing)});
            const auto* raw = holder->buffer.get();
            return {std::move(holder), raw};
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
        std::uint64_t serial = 0;
    };
    // #765: retires every decoded source that no block of this pass has played for a second of
    // programme, or that only an earlier pass played. The second keeps a source that pauses
    // briefly from being opened again; a source the mix plays later opens again then.
    void releaseIdleAudio(const MixPass& run, const std::uint64_t through) {
        const std::uint64_t idle = settings().sampleRate;
        for (auto entry = audioBuffers.begin(); entry != audioBuffers.end();) {
            const auto& resident = entry->second;
            if (resident.pass == run.serial && resident.playedThrough + idle > through) {
                ++entry;
                continue;
            }
            if (const auto opened = audioSources.find(entry->first); opened != audioSources.end())
                if (const auto decoder = opened->second.decoder.lock())
                    opened->second.notes = decoder->routingNotes();
            entry = audioBuffers.erase(entry);
        }
    }
    ~State() {
        // AudioEngine keeps shared clip buffers; each buffer's claim retires with its last owner.
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
        next.serial = ++passSerial;
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
        // The sources this block reads as they are decoded, to ask each afterwards whether its
        // decode failed.
        std::vector<std::pair<document::AssetId, std::shared_ptr<const media::audio::AudioBuffer>>>
            streamed;
        // The clips are built where the audio analysis builds its own (#630), so an analysis reads
        // the samples this export writes.
        // #765: a clip reads its source only in a block it plays in. Every other clip keeps its
        // place in the mix with no buffer, which the mixer plays as the silence it would have
        // read, so its source is neither opened nor held for this block.
        const auto through = run.position + count;
        auto clips = runtime::audioMixClips(
            mix, endMix ? &*endMix : nullptr, time, endTime,
            [&](const runtime::AudioClipDescription& description)
                -> std::shared_ptr<const media::audio::AudioBuffer> {
                // A block whose end is not representable plays every clip, as before.
                if (endTime &&
                    !exportAudioClipSoundsV1(
                        description, source.snapshot.project().findAsset(description.assetId), time,
                        *endTime, settings().sampleRate))
                    return nullptr;
                const runtime::AudioStreamKey key{description.assetId, description.audioStream};
                auto found = audioBuffers.find(key);
                if (found == audioBuffers.end())
                    found =
                        audioBuffers
                            .emplace(key, ResidentAudio{loadSource(context, description, cancel)})
                            .first;
                auto& resident = found->second;
                require(resident.buffer != nullptr, "Audio source buffer unavailable",
                        Error::Unavailable);
                resident.pass = run.serial;
                resident.playedThrough = through;
                if (resident.buffer->stream)
                    streamed.emplace_back(description.assetId, resident.buffer);
                return resident.buffer;
            });
        run.mixer->replaceClips(std::move(clips));
        releaseIdleAudio(run, through);
        run.sink->clearCaptured();
        require(!run.mixer->renderForTesting(count), "Offline mix failed");
        // The mixer plays silence for a source it cannot read, which is right for a device and
        // wrong for a file: a movie whose audio stops decoding must not publish as silence.
        for (const auto& [asset, buffer] : streamed)
            if (buffer->stream->diagnostic()) {
                audioSourceFailed = true;
                throw Unavailable{Error::Unavailable,
                                  "Audio asset " + std::to_string(asset.value()) +
                                      " couldn't be decoded for the export (its audio stream "
                                      "stopped decoding)."};
            }
        const auto captured = run.sink->captured();
        require(captured.size() == static_cast<std::size_t>(count) * 2U,
                "Offline mix sample count differs");
        run.position += count;
        run.blockEndMix = std::move(endMix);
        return {captured.begin(), captured.end()};
    }
    // Sends interleaved stereo to `session` as the block that starts at programme frame `at`;
    // returns the frames sent.
    std::uint64_t send(EncodeSessionV1& session, const std::vector<float>& interleaved,
                       const std::uint64_t at) const {
        const auto count = interleaved.size() / 2;
        if (count == 0)
            return 0;
        AudioBlock block;
        block.sampleRate = settings().sampleRate;
        block.channelLayout = {"FL", "FR"};
        block.pts = fraction(at, settings().sampleRate);
        block.channels.resize(2, std::vector<float>(count));
        for (std::size_t i = 0; i < count; ++i)
            for (std::size_t c = 0; c < 2; ++c)
                block.channels[c][i] = interleaved[i * 2 + c];
        checked(session.audio(std::move(block)));
        return count;
    }
    void deliver(const std::vector<float>& interleaved) {
        emitted += send(*encoder, interleaved, emitted);
    }
    // #771 trial encode: the whole programme through the gain and a limiter at `ceilingDbtp`,
    // encoded alone with the export's codec and settings, decoded back by the worker's verifying
    // reopen and measured there with the reference library. Returns the decoded true peak.
    double trialEncode(runtime::TaskContext& context, const double gainDb, const double ceilingDbtp,
                       const std::uint32_t trial) {
        const auto total = settings().audioSamples;
        auto trialSettings = settings();
        trialSettings.videoCodec.clear();
        trialSettings.profile.clear();
        trialSettings.frames = 0;
        trialSettings.subtitles.clear();
        EncodeSessionV1 session(trialWorker, cancellation);
        checked(session.begin(trialSettings));
        media::audio::loudness::Limiter trialLimiter(settings().sampleRate, 2, gainDb, ceilingDbtp);
        MixPass run;
        startPass(run);
        std::uint64_t sent = 0;
        std::uint64_t blocks = 0;
        std::vector<float> limited;
        const std::string step =
            "Checking the true peak after encoding (trial " + std::to_string(trial + 1) + ")";
        while (run.position < total) {
            checkpoint();
            const auto count =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(512, total - run.position));
            const auto block = renderBlock(run, context, count);
            limited.clear();
            trialLimiter.process(block, limited);
            sent += send(session, limited, sent);
            if (++blocks % 256 == 0)
                context.reportProgress({step, "", run.position, total});
        }
        run.mixer.reset();
        limited.clear();
        trialLimiter.flush(limited);
        sent += send(session, limited, sent);
        require(sent == total, "Trial encode sample count differs");
        const auto qc = checked(session.finish());
        checked(session.close());
        require(qc.audioMeasured && qc.audioSamples == total,
                "The trial encode's sound was not measured", Error::Unavailable);
        return qc.audioTruePeakDbtp;
    }
    // #771: a lossy codec's decoded sound peaks between and above the samples it was given, so the
    // limiter's ceiling for it is found by encoding: while a trial's decoded true peak is over the
    // limit, the ceiling comes down by the overshoot (and a small margin) and the trial runs again.
    // The bundled encoders are deterministic, so the export's own audio stream, given the same
    // samples, decodes to the last trial's sound. A platform encoder may differ; the export checks
    // its delivered file as well.
    double encoderCeiling(runtime::TaskContext& context, const double gainDb, const double limit,
                          double ceiling, AudioLoudnessReportV1& report) {
        for (std::uint32_t trial = 0; trial < kTrialEncodes; ++trial) {
            checkpoint();
            const auto peak = trialEncode(context, gainDb, ceiling, trial);
            report.trialEncodes = trial + 1;
            if (trial == 0)
                report.firstTrialTruePeakDbtp = peak;
            report.trialTruePeakDbtp = peak;
            if (!(peak > limit))
                return ceiling;
            ceiling -= peak - limit + kTrialMarginDb;
        }
        throw Unavailable{
            Error::Unavailable,
            "The " + settings().audioCodec + " encoder kept the sound's true peak at " +
                std::to_string(report.trialTruePeakDbtp) + " dBTP, above the " +
                std::to_string(limit) + " dBTP limit, after " + std::to_string(kTrialEncodes) +
                " trial encodes with the limiter lowered each time."};
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
        auto ceiling = wanted.truePeakDbtp - std::max(0.0, wanted.encoderHeadroomDb);
        if (lossyAudioCodec(settings().audioCodec) && wanted.trialEncodes)
            ceiling = encoderCeiling(context, report.gainDb, wanted.truePeakDbtp, ceiling, report);
        report.limiterCeilingDbtp = ceiling;
        loudnessResult = report;
        limiter = std::make_unique<media::audio::loudness::Limiter>(settings().sampleRate, 2,
                                                                    report.gainDb, ceiling);
        measuredAfter.emplace(settings().sampleRate, 2);
        loudnessMeasured = true;
    }
    void prepareLoudness(runtime::TaskContext& context) {
        if (!settings().audioCodec.empty() && source.loudness && !loudnessMeasured)
            measureProgramme(context);
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
            // #570/#771: the programme is measured (and a lossy codec's trial encodes run) before
            // the export's encoder starts, so its worker never waits on them.
            s.prepareLoudness(context);
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
            // #570/#771: the programme is measured (and a lossy codec's trial encodes run) before
            // the export's encoder starts, so its worker never waits on them.
            s.prepareLoudness(context);
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
std::vector<std::string> CompositionOutputStreamV1::sourceRoutingNotes() const {
    std::vector<std::string> notes;
    for (const auto& [key, opened] : state_->audioSources) {
        const auto decoder = opened.decoder.lock();
        for (auto& note : decoder ? decoder->routingNotes() : opened.notes)
            notes.push_back(opened.name + ": " + std::move(note));
    }
    return notes;
}
bool CompositionOutputStreamV1::platformEncoderRefused() const noexcept {
    return state_->encoder->platformEncoderRefused();
}
bool CompositionOutputStreamV1::platformEncoderFailed() const noexcept {
    return state_->encoder->platformEncoderFailed();
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
        // On macOS and Windows the version names the system encoder that ran (issues #455 and
        // #370).
        const bool mediaFoundation = hello.execution.provider == "bloom.mediafoundation";
        evidence.version =
            videotoolbox
                ? std::string("VideoToolbox") + (qc.encoder.empty() ? "" : " " + qc.encoder)
            : mediaFoundation && !qc.encoder.empty() ? hello.execution.sdk + "; " + qc.encoder
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

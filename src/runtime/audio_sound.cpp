#include <bloom/runtime/audio_sound.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/core/worker_thread.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/audio/sound_table.hpp>
#include <bloom/platform/cache_directory.hpp>
#include <bloom/platform/positional_file.hpp>
#include <bloom/runtime/audio_mix_render.hpp>
#include <bloom/runtime/audio_stream_key.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include "audio_source_open.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <system_error>
#include <vector>

namespace bloom::runtime {
namespace {

using AudioEngine = media::audio::playback::AudioEngine;
using NullBackend = media::audio::playback::NullBackend;

// The scratch file of one rendered sound: interleaved stereo float. The file is removed with the
// sound; on Linux and macOS its name is removed as soon as it is open, so nothing is left behind
// by a process that ends without cleaning up.
class ScratchFile final {
  public:
    static std::shared_ptr<ScratchFile> create(const std::filesystem::path& directory,
                                               const std::string& name) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        auto path = directory / name;
        auto file = platform::PositionalFile::open(path, true);
        if (!file)
            return nullptr;
        auto scratch = std::shared_ptr<ScratchFile>(new ScratchFile(std::move(file), path));
#if !defined(_WIN32)
        std::filesystem::remove(path, error);
        scratch->path_.clear();
#endif
        return scratch;
    }
    ~ScratchFile() {
        file_.reset();
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    [[nodiscard]] const platform::PositionalFile& file() const noexcept { return *file_; }

  private:
    ScratchFile(std::unique_ptr<platform::PositionalFile> file, std::filesystem::path path)
        : file_(std::move(file)), path_(std::move(path)) {}
    std::unique_ptr<platform::PositionalFile> file_;
    std::filesystem::path path_;
};

class ScratchStream final : public media::audio::AudioStream {
  public:
    ScratchStream(std::shared_ptr<ScratchFile> scratch, const std::uint64_t frames)
        : AudioStream(probeOf(frames), kWindowFrames), scratch_(std::move(scratch)) {}

  protected:
    media::audio::AudioResult<std::size_t>
    fill(const std::uint64_t firstFrame, const std::span<float> output,
         const media::audio::CancelAudioWork&) noexcept override {
        const auto wanted = output.size() / kAudioSoundChannels;
        if (firstFrame >= probe().frames)
            return media::audio::AudioResult<std::size_t>::success(0);
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(wanted, probe().frames - firstFrame));
        const auto bytes = std::as_writable_bytes(output.first(count * kAudioSoundChannels));
        if (!scratch_->file().readAt(firstFrame * kAudioSoundChannels * sizeof(float), bytes))
            return media::audio::AudioResult<std::size_t>::failure(
                media::audio::AudioError::codeOnly(media::audio::AudioErrorCode::FileReadFailed));
        return media::audio::AudioResult<std::size_t>::success(count);
    }

  private:
    static media::audio::AudioProbe probeOf(const std::uint64_t frames) {
        media::audio::AudioProbe probe;
        probe.container = media::audio::AudioContainer::Wav;
        probe.rate = kAudioSoundRate;
        probe.channels = kAudioSoundChannels;
        probe.frames = frames;
        probe.duration = core::RationalTime::create(static_cast<std::int64_t>(frames),
                                                    static_cast<std::int64_t>(kAudioSoundRate))
                             .value_or(core::RationalTime{});
        return probe;
    }
    std::shared_ptr<ScratchFile> scratch_;
};

class ScratchTable final : public AudioSoundTable {
  public:
    ScratchTable(std::shared_ptr<ScratchFile> scratch, const std::uint64_t rows)
        : scratch_(std::move(scratch)), rows_(rows) {}
    [[nodiscard]] std::uint64_t rows() const noexcept override { return rows_; }
    [[nodiscard]] bool read(const std::uint64_t first, const std::span<float> values) const override {
        const std::uint64_t count = values.size() / media::audio::analysis::kTableRowFloats;
        if (values.size() % media::audio::analysis::kTableRowFloats != 0 || first > rows_ ||
            count > rows_ - first)
            return false;
        return scratch_->file().readAt(
            first * media::audio::analysis::kTableRowFloats * sizeof(float),
            std::as_writable_bytes(values));
    }

  private:
    std::shared_ptr<ScratchFile> scratch_;
    std::uint64_t rows_ = 0;
};

// The bytes one block boundary adds to a sound's identity.
class IdentityWriter final {
  public:
    void integer(const std::uint64_t value) { raw(&value, sizeof value); }
    void real(const double value) { raw(&value, sizeof value); }
    void time(const core::RationalTime value) {
        integer(static_cast<std::uint64_t>(value.numerator()));
        integer(static_cast<std::uint64_t>(value.denominator()));
    }
    void rate(const document::FrameRate value) {
        integer(static_cast<std::uint64_t>(value.numerator()));
        integer(static_cast<std::uint64_t>(value.denominator()));
    }
    void optionalTime(const std::optional<core::RationalTime>& value) {
        integer(value ? 1 : 0);
        if (value)
            time(*value);
    }
    void reals(const std::vector<double>& values) {
        integer(values.size());
        for (const double value : values)
            real(value);
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    void clear() noexcept { bytes_.clear(); }

  private:
    void raw(const void* data, const std::size_t size) {
        const auto* first = static_cast<const std::byte*>(data);
        bytes_.insert(bytes_.end(), first, first + size);
    }
    std::vector<std::byte> bytes_;
};

// A clip as the mixer will hear it at `at`. A time mapping is written by its plain members and by
// the content time it gives at this boundary, so a Time Remap curve that changes is another sound.
void writeClip(IdentityWriter& out, const AudioClipDescription& clip, const core::RationalTime at) {
    out.integer(clip.sourceNodeId.value());
    out.integer(clip.assetId.value());
    out.integer(clip.audioStream);
    out.time(clip.startTime);
    out.time(clip.endTime);
    out.real(clip.level);
    out.integer((clip.muted ? 1U : 0U) | (clip.solo ? 2U : 0U));
    out.integer(clip.timeMappings.size());
    for (const auto& mapping : clip.timeMappings) {
        out.real(mapping.offset);
        out.real(mapping.scale);
        out.integer(static_cast<std::uint64_t>(mapping.loopMode));
        out.time(mapping.duration);
        out.rate(mapping.frameRate);
        out.time(mapping.inPoint);
        out.time(mapping.outPoint);
        out.integer(mapping.layerTime ? 1 : 0);
    }
    if (!clip.timeMappings.empty())
        out.optionalTime(mapAudioClipTime(clip, at));
    out.integer(static_cast<std::uint64_t>(clip.sourceLoopMode));
    out.integer(clip.sourceLoopExtent ? 1 : 0);
    if (clip.sourceLoopExtent) {
        out.time(clip.sourceLoopExtent->duration);
        out.rate(clip.sourceLoopExtent->frameRate);
    }
    out.integer(clip.footage ? 1 : 0);
    if (clip.footage) {
        out.time(clip.footage->pass);
        out.integer(clip.footage->loops);
        out.integer(static_cast<std::uint64_t>(clip.footage->numerator));
        out.integer(static_cast<std::uint64_t>(clip.footage->denominator));
    }
    out.integer(clip.effects.size());
    for (const auto& effect : clip.effects) {
        out.integer(static_cast<std::uint64_t>(effect.kind));
        out.reals(effect.values);
    }
}

[[nodiscard]] std::optional<core::RationalTime> sampleTime(const std::uint64_t sample) {
    return core::RationalTime::create(static_cast<std::int64_t>(sample),
                                      static_cast<std::int64_t>(kAudioSoundRate));
}

[[nodiscard]] std::uint64_t soundFrames(const AudioAnalysisSound& sound) {
    const double seconds = sound.duration.toSeconds();
    if (!(seconds > 0.0) || !std::isfinite(seconds))
        return 0;
    return static_cast<std::uint64_t>(std::ceil(seconds * kAudioSoundRate));
}

// The plan that holds `sound`'s mix: `plan` itself or one nested in it.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
owningPlan(const std::shared_ptr<const CompiledCompositionPlan>& plan,
           const AudioAnalysisSound& sound) {
    if (!plan)
        return nullptr;
    for (const auto& mix : plan->analysisAudioMixes())
        if (mix == sound.mix)
            return plan;
    for (const auto& nested : plan->nestedPlans())
        if (auto found = owningPlan(nested, sound))
            return found;
    return nullptr;
}

enum class JobState : std::uint8_t { Queued, Running, Ready, Failed, Cancelled };

struct Job final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::shared_ptr<const AudioAnalysisSound> sound;
    std::filesystem::path assetBase;
    std::atomic<bool> cancel{false};
    std::atomic<std::uint64_t> framesDone{0};
    std::uint64_t framesTotal = 0;
    std::atomic<AudioSoundPhase> phase{AudioSoundPhase::Identifying};
    // Guarded by the store's mutex.
    JobState state = JobState::Queued;
    std::size_t waiters = 0;
    std::string detail;
    std::shared_ptr<const PreparedAudioSound> result;
};

struct Entry final {
    std::shared_ptr<const PreparedAudioSound> sound;
    std::uint64_t used = 0;
};

struct Store final {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Job>> queue;
    std::shared_ptr<Job> running;
    std::map<std::string, Entry> ready;
    std::uint64_t tick = 0;
    std::uint64_t serial = 0;
    AudioSoundStatistics statistics;
    std::string lastFailure;
    std::optional<std::filesystem::path> directory;
    core::WorkerThread worker;
    bool stop = false;
    bool swept = false;

    ~Store() {
        {
            const std::scoped_lock lock(mutex);
            stop = true;
            if (running)
                running->cancel.store(true);
        }
        changed.notify_all();
        if (worker.joinable())
            worker.join();
    }
};

Store& store() {
    static Store instance;
    return instance;
}

struct Failure final {
    bool cancelled = false;
    std::string detail;
};

// The identity of the job's sound, or why there is none.
[[nodiscard]] std::optional<Failure> identify(Job& job, std::string& digest) {
    const auto& sound = *job.sound;
    core::Sha256Hasher hasher;
    IdentityWriter out;
    out.integer(1); // The identity's version.
    out.integer(kAudioSoundRate);
    out.integer(kAudioSoundBlockFrames);
    out.integer(job.framesTotal);
    const CpuCompositionEvaluator evaluator{};
    std::map<document::AssetId, bool> written;
    for (std::uint64_t position = 0;; position += kAudioSoundBlockFrames) {
        if (job.cancel.load())
            return Failure{true, {}};
        const auto at = sampleTime(std::min(position, job.framesTotal));
        if (!at)
            return Failure{false, "The sound is too long to analyze"};
        const auto mix = evaluator.evaluateAudioMix(job.plan, *at, {}, &sound.mix);
        out.integer(mix ? mix->clips.size() : ~std::uint64_t{0});
        if (mix)
            for (const auto& clip : mix->clips) {
                writeClip(out, clip, *at);
                if (!written.emplace(clip.assetId, true).second)
                    continue;
                // An asset's content digest joins the identity the first time a clip plays it.
                const document::AssetRecord* found = nullptr;
                if (sound.assets)
                    for (const auto& asset : *sound.assets)
                        if (asset.id == clip.assetId)
                            found = &asset;
                out.integer(found != nullptr ? 1 : 0);
                if (found != nullptr) {
                    const auto hex = found->contentDigest.toLowercaseHex();
                    for (const char character : hex)
                        out.integer(static_cast<unsigned char>(character));
                }
            }
        if (!hasher.update(out.bytes()))
            return Failure{false, "The sound is too long to analyze"};
        out.clear();
        job.framesDone.store(std::min(position, job.framesTotal));
        if (position >= job.framesTotal)
            break;
    }
    const auto hex = hasher.finalize().toLowercaseHex();
    digest.assign(hex.begin(), hex.end());
    return std::nullopt;
}

// Renders the job's sound into a scratch file.
[[nodiscard]] std::optional<Failure> render(Job& job, const std::filesystem::path& directory,
                                            const std::string& name,
                                            std::shared_ptr<const PreparedAudioSound>& result,
                                            const std::string& digest) {
    const auto& sound = *job.sound;
    const auto scratch = ScratchFile::create(directory, name);
    if (!scratch)
        return Failure{false, "The audio analysis scratch file could not be made in the cache "
                              "folder"};
    const auto cancelled = [&job] { return job.cancel.load(); };
    struct Opened final {
        std::shared_ptr<const media::audio::AudioBuffer> buffer;
        std::shared_ptr<media::video::VideoDecodeSession> session;
    };
    std::map<AudioStreamKey, Opened> buffers;
    std::optional<Failure> failure;
    const auto lookup =
        [&](const AudioClipDescription& clip) -> std::shared_ptr<const media::audio::AudioBuffer> {
        const AudioStreamKey key{clip.assetId, clip.audioStream};
        if (const auto found = buffers.find(key); found != buffers.end())
            return found->second.buffer;
        if (failure)
            return nullptr;
        const auto* assets = sound.assets.get();
        const document::AssetRecord* asset = nullptr;
        if (assets != nullptr)
            for (const auto& candidate : *assets)
                if (candidate.id == clip.assetId)
                    asset = &candidate;
        if (asset == nullptr) {
            failure = Failure{false, "An audio source's asset is missing"};
            return nullptr;
        }
        auto opened = detail::openAssetAudio(*asset, clip.audioStream, job.assetBase, cancelled);
        if (!opened.buffer) {
            failure = Failure{opened.cancelled,
                              opened.error.empty() ? "The audio could not be opened" : opened.error};
            return nullptr;
        }
        return buffers.emplace(key, Opened{opened.buffer, opened.session}).first->second.buffer;
    };
    auto backend = std::make_unique<NullBackend>();
    auto* const sink = backend.get();
    AudioEngine mixer(std::move(backend),
                      AudioEngine::Config{kAudioSoundRate, kAudioSoundChannels, 16384});
    if (mixer.play(core::RationalTime{}))
        return Failure{false, "The offline mixer could not start"};
    const CpuCompositionEvaluator evaluator{};
    std::optional<AudioMixDescription> blockEndMix;
    for (std::uint64_t position = 0; position < job.framesTotal;) {
        if (cancelled())
            return Failure{true, {}};
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(kAudioSoundBlockFrames, job.framesTotal - position));
        const auto time = sampleTime(position);
        if (!time)
            return Failure{false, "The sound is too long to analyze"};
        auto mix = blockEndMix ? std::move(blockEndMix)
                               : evaluator.evaluateAudioMix(job.plan, *time, {}, &sound.mix);
        blockEndMix.reset();
        if (!mix)
            return Failure{false, "The audio graph could not be evaluated"};
        const auto endTime = sampleTime(position + count);
        std::optional<AudioMixDescription> endMix;
        if (endTime)
            endMix = evaluator.evaluateAudioMix(job.plan, *endTime, {}, &sound.mix);
        auto clips = audioMixClips(*mix, endMix ? &*endMix : nullptr, *time, endTime, lookup);
        if (failure)
            return failure;
        mixer.replaceClips(std::move(clips));
        sink->clearCaptured();
        if (mixer.renderForTesting(count))
            return Failure{false, "The offline mix failed"};
        // The mixer plays silence for a source it cannot read; an analysis of silence would be a
        // wrong answer, so a source that stopped decoding fails the sound, as it fails an export.
        for (const auto& [key, opened] : buffers)
            if (opened.buffer->stream && opened.buffer->stream->diagnostic())
                return Failure{false, "Audio asset " + std::to_string(key.asset.value()) +
                                          " stopped decoding"};
        const auto captured = sink->captured();
        if (captured.size() != static_cast<std::size_t>(count) * kAudioSoundChannels)
            return Failure{false, "The offline mix returned the wrong number of samples"};
        if (!scratch->file().writeAt(position * kAudioSoundChannels * sizeof(float),
                                     std::as_bytes(captured)))
            return Failure{false, "The audio analysis scratch file could not be written (the "
                                  "cache volume may be full)"};
        position += count;
        blockEndMix = std::move(endMix);
        job.framesDone.store(position);
    }
    // The mixer keeps the source buffers; it retires before the sessions they decode through.
    mixer.clearClips();
    // #631: the analysis table, from the samples just written.
    namespace an = media::audio::analysis;
    job.framesDone.store(0);
    job.phase.store(AudioSoundPhase::Analyzing);
    const auto tableScratch = ScratchFile::create(directory, name + ".table");
    if (!tableScratch)
        return Failure{false, "The audio analysis scratch file could not be made in the cache "
                              "folder"};
    const auto tableReader = [&](const std::int64_t first, const std::span<float> output) {
        std::fill(output.begin(), output.end(), 0.0F);
        const auto total = static_cast<std::int64_t>(job.framesTotal);
        const auto count = static_cast<std::int64_t>(output.size() / kAudioSoundChannels);
        const std::int64_t begin = std::max<std::int64_t>(first, 0);
        const std::int64_t end = std::min<std::int64_t>(first + count, total);
        if (end <= begin)
            return true;
        return scratch->file().readAt(
            static_cast<std::uint64_t>(begin) * kAudioSoundChannels * sizeof(float),
            std::as_writable_bytes(
                output.subspan(static_cast<std::size_t>(begin - first) * kAudioSoundChannels,
                               static_cast<std::size_t>(end - begin) * kAudioSoundChannels)));
    };
    const auto tableSink = [&](const std::uint64_t row, const std::span<const float> values) {
        job.framesDone.store(std::min<std::uint64_t>(row * an::kTableHopFrames, job.framesTotal));
        return tableScratch->file().writeAt(row * an::kTableRowFloats * sizeof(float),
                                            std::as_bytes(values));
    };
    const auto rows = an::buildTable(job.framesTotal, tableReader, tableSink, cancelled);
    if (!rows) {
        if (rows.error()->code == media::audio::AudioErrorCode::Cancelled)
            return Failure{true, {}};
        return Failure{false, rows.error()->code == media::audio::AudioErrorCode::AllocationFailure
                                  ? "There is not enough memory to analyze the audio"
                                  : "The audio analysis scratch file could not be read or written "
                                    "(the cache volume may be full)"};
    }
    auto prepared = std::make_shared<PreparedAudioSound>();
    prepared->table = std::make_shared<ScratchTable>(tableScratch, *rows.value());
    prepared->digest = digest;
    prepared->frames = job.framesTotal;
    auto buffer = std::make_shared<media::audio::AudioBuffer>();
    buffer->rate = kAudioSoundRate;
    buffer->channels = kAudioSoundChannels;
    buffer->frames = job.framesTotal;
    buffer->stream = std::make_shared<ScratchStream>(scratch, job.framesTotal);
    prepared->buffer = std::move(buffer);
    result = std::move(prepared);
    return std::nullopt;
}

// Once per process: scratch files a process left behind when it ended without cleaning up (only
// Windows keeps their names). A file another running process still holds cannot be removed there
// and is left alone.
void sweepStaleScratch(Store& s, const std::filesystem::path& directory) {
    {
        const std::scoped_lock lock(s.mutex);
        if (s.swept)
            return;
        s.swept = true;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name.starts_with("sound-") && (name.ends_with(".pcm") || name.ends_with(".table"))) {
            std::error_code ignored;
            std::filesystem::remove(it->path(), ignored);
        }
    }
}

void retireBeyondBudget(Store& s, const std::string& keep) {
    while (s.ready.size() > kAudioSoundEntries) {
        auto oldest = s.ready.end();
        for (auto it = s.ready.begin(); it != s.ready.end(); ++it)
            if (it->first != keep && (oldest == s.ready.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == s.ready.end())
            return;
        s.ready.erase(oldest);
    }
}

void run(Store& s, const std::shared_ptr<Job>& job) {
    std::optional<Failure> failure;
    std::shared_ptr<const PreparedAudioSound> result;
    bool reused = false;
    try {
        std::string digest = job->sound->identity->digest();
        if (digest.empty()) {
            failure = identify(*job, digest);
            if (!failure)
                job->sound->identity->setDigest(digest);
        }
        if (!failure) {
            std::filesystem::path directory;
            std::string name;
            {
                const std::scoped_lock lock(s.mutex);
                if (const auto found = s.ready.find(digest); found != s.ready.end()) {
                    found->second.used = ++s.tick;
                    result = found->second.sound;
                    reused = true;
                }
                if (s.directory)
                    directory = *s.directory;
                name = "sound-" + digest.substr(0, 16) + "-" + std::to_string(++s.serial) + ".pcm";
            }
            if (!result) {
                if (directory.empty()) {
                    if (const auto root = platform::userCacheDirectory("bloom"))
                        directory = *root / "audio-analysis";
                }
                if (directory.empty()) {
                    failure = Failure{false, "There is no cache folder for the audio analysis"};
                } else {
                    sweepStaleScratch(s, directory);
                    job->framesDone.store(0);
                    job->phase.store(AudioSoundPhase::Rendering);
                    failure = render(*job, directory, name, result, digest);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        failure = Failure{false, "There is not enough memory to analyze the audio"};
    } catch (const std::exception& error) {
        failure = Failure{false, error.what()};
    }
    {
        const std::scoped_lock lock(s.mutex);
        if (failure) {
            job->state = failure->cancelled ? JobState::Cancelled : JobState::Failed;
            job->detail = failure->detail;
            if (failure->cancelled) {
                ++s.statistics.cancelled;
            } else {
                ++s.statistics.failed;
                s.lastFailure = failure->detail;
            }
        } else {
            job->state = JobState::Ready;
            job->result = result;
            s.lastFailure.clear();
            if (reused) {
                ++s.statistics.reused;
            } else {
                ++s.statistics.rendered;
                s.ready.insert_or_assign(result->digest, Entry{result, ++s.tick});
                retireBeyondBudget(s, result->digest);
            }
        }
        s.running.reset();
    }
    // The job's plan and sound are released here, off the store's lock.
    job->plan.reset();
    s.changed.notify_all();
}

void work(Store& s) {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(s.mutex);
            s.changed.wait(lock, [&] { return s.stop || !s.queue.empty(); });
            if (s.stop)
                return;
            job = s.queue.front();
            s.queue.pop_front();
            job->state = JobState::Running;
            s.running = job;
        }
        run(s, job);
    }
}

} // namespace

std::shared_ptr<const PreparedAudioSound> findAudioSound(const AudioAnalysisSound& sound) {
    if (!sound.identity)
        return nullptr;
    const auto digest = sound.identity->digest();
    if (digest.empty())
        return nullptr;
    auto& s = store();
    const std::scoped_lock lock(s.mutex);
    const auto found = s.ready.find(digest);
    if (found == s.ready.end())
        return nullptr;
    found->second.used = ++s.tick;
    return found->second.sound;
}

AudioSoundStatus prepareAudioSound(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                   const std::shared_ptr<const AudioAnalysisSound>& sound,
                                   const std::filesystem::path& assetBase,
                                   const CancellationToken& cancellation) {
    if (!sound || !sound->identity)
        return {AudioSoundOutcome::Unavailable, "No audio is connected", nullptr};
    if (auto found = findAudioSound(*sound))
        return {AudioSoundOutcome::Ready, {}, std::move(found)};
    if (cancellation.isCancellationRequested())
        return {AudioSoundOutcome::Cancelled, {}, nullptr};
    auto owner = owningPlan(plan, *sound);
    if (!owner)
        return {AudioSoundOutcome::Unavailable, "The sound does not belong to this composition",
                nullptr};
    const auto frames = soundFrames(*sound);
    if (frames == 0)
        return {AudioSoundOutcome::Unavailable, "The composition has no duration", nullptr};
    auto& s = store();
    std::shared_ptr<Job> job;
    {
        std::unique_lock lock(s.mutex);
        const auto same = [&](const std::shared_ptr<Job>& candidate) {
            return candidate && candidate->sound->identity == sound->identity &&
                   !candidate->cancel.load();
        };
        if (same(s.running))
            job = s.running;
        for (const auto& queued : s.queue)
            if (!job && same(queued))
                job = queued;
        if (!job) {
            // Another sound is wanted: a job nobody waits for is dropped (the artist moved on).
            if (s.running && s.running->waiters == 0)
                s.running->cancel.store(true);
            std::erase_if(s.queue, [&](const std::shared_ptr<Job>& queued) {
                if (queued->waiters != 0)
                    return false;
                ++s.statistics.cancelled;
                return true;
            });
            job = std::make_shared<Job>();
            job->plan = std::move(owner);
            job->sound = sound;
            job->assetBase = assetBase;
            job->framesTotal = frames;
            s.queue.push_back(job);
            if (!s.worker.joinable())
                s.worker = core::WorkerThread([&s] { work(s); });
        }
        ++job->waiters;
        s.changed.notify_all();
        for (;;) {
            if (job->state == JobState::Ready || job->state == JobState::Failed ||
                job->state == JobState::Cancelled)
                break;
            if (cancellation.isCancellationRequested()) {
                --job->waiters;
                return {AudioSoundOutcome::Cancelled, {}, nullptr};
            }
            s.changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        --job->waiters;
        if (job->state == JobState::Ready)
            return {AudioSoundOutcome::Ready, {}, job->result};
        if (job->state == JobState::Cancelled)
            return {AudioSoundOutcome::Cancelled, {}, nullptr};
        return {AudioSoundOutcome::Unavailable, job->detail, nullptr};
    }
}

AudioSoundActivity audioSoundActivity() {
    auto& s = store();
    const std::scoped_lock lock(s.mutex);
    AudioSoundActivity activity;
    if (s.running) {
        activity.phase = s.running->phase.load();
        activity.framesDone = s.running->framesDone.load();
        activity.framesTotal = s.running->framesTotal;
    }
    activity.queued = s.queue.size();
    activity.lastFailure = s.lastFailure;
    return activity;
}

AudioSoundStatistics audioSoundStatistics() {
    auto& s = store();
    const std::scoped_lock lock(s.mutex);
    auto copy = s.statistics;
    copy.entries = s.ready.size();
    copy.scratchBytes = 0;
    for (const auto& [digest, entry] : s.ready)
        copy.scratchBytes +=
            entry.sound->frames * kAudioSoundChannels * sizeof(float) +
            (entry.sound->table ? entry.sound->table->rows() *
                                      media::audio::analysis::kTableRowFloats * sizeof(float)
                                : 0U);
    return copy;
}

void clearAudioSounds() {
    auto& s = store();
    std::unique_lock lock(s.mutex);
    for (const auto& queued : s.queue) {
        queued->state = JobState::Cancelled;
        ++s.statistics.cancelled;
    }
    s.queue.clear();
    if (s.running)
        s.running->cancel.store(true);
    s.changed.notify_all();
    s.changed.wait(lock, [&] { return !s.running; });
    s.ready.clear();
    s.statistics = {};
    s.lastFailure.clear();
}

void setAudioSoundDirectory(std::optional<std::filesystem::path> directory) {
    auto& s = store();
    const std::scoped_lock lock(s.mutex);
    s.directory = std::move(directory);
}

} // namespace bloom::runtime

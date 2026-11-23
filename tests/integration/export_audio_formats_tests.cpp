// Issue #428: export audio source formats end to end through the export queue and the media
// worker. 16-bit, 24-bit and 32-bit float WAV sources export through every audio-carrying preset
// family, PCM containers keep 24 bits for a deeper source, and a source the export audio path
// cannot decode is refused by the source-aware audio preflight before anything renders, with a
// sentence naming the stage and the input. Issue #424: a failed run keeps its frames' route
// provenance instead of counting every frame as CPU reference. Issue #450: a 5.1 source exports
// to stereo through the ITU-R BS.775 downmix instead of losing every channel past the second.
// Issue #459: an automated level exports as a continuous ramp, not a step every mix block.
// Issue #569: an audio effect chain exports the samples the playback mixer plays.
// Issue #785: an output that uses cached frames reuses frames an earlier export evaluated, with the
// same delivered file as a fresh render.
#include "fixed_memory_budget.hpp"
#include "movie_difference.hpp"
#include "unique_temp_dir.hpp"
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/export_frame_cache.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/media/audio/loudness.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/output/export_audio_sources.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include "playback_mix_reference.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace bloom;
using document::ExportFormat;
using host::ExportJobState;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::filesystem::path scratch(const std::string& name) {
    auto path = bloom::test_support::processTempDirectory() /
                ("bloom-export-audio-" + name + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

document::Snapshot currentSnapshot(const host::ProjectSession& session) {
    const auto decoded = session.decodedSnapshot();
    return decoded.snapshot();
}

// The WAV encodings the tests write: integer PCM, IEEE float, IMA ADPCM (which the import
// decoder reads but the bundled FFmpeg build has no decoder for), and WAVE_FORMAT_EXTENSIBLE
// float 5.1 (FL FR FC LFE BL BR) holding the constants in kSurroundLevels, and float stereo
// holding kConstantLevel.
enum class WavEncoding { Pcm16, Pcm24, Float32, ImaAdpcm, Surround51, ConstantStereo, Transients };
constexpr float kConstantLevel = 0.5F;
constexpr std::array<float, 6> kSurroundLevels{0.1F, 0.2F, 0.3F, 0.4F, 0.05F, 0.15F};

void put(std::vector<char>& out, const std::uint64_t value, const int bytes) {
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
}

// #771: a steady bed of tone and noise with a loud noise burst every 125 ms, the kind of programme
// a true-peak limiter has to hold down and a lossy codec rebuilds with peaks above what it was
// given.
double transientSample(const std::uint32_t frame, const std::uint32_t channel) {
    constexpr double pi = 3.141592653589793;
    const auto t = static_cast<double>(frame) / 48000.0;
    // Noise from a hash of the frame (no state, so any frame is the same in every run), smoothed by
    // a short moving average into the band every codec keeps.
    const auto noise = [](const std::uint32_t at, const std::uint32_t lane) {
        std::uint32_t state = at * 2654435761U + (lane + 1U) * 40503U;
        state ^= state >> 15U;
        state *= 2246822519U;
        state ^= state >> 13U;
        return static_cast<double>(state % 20001U) / 10000.0 - 1.0;
    };
    double smoothed = 0.0;
    for (std::uint32_t tap = 0; tap < 3U; ++tap)
        smoothed += noise(frame + tap, channel) / 3.0;
    // A bed of tones and noise, different in each channel.
    double value = 0.06 * std::sin(2.0 * pi * (220.0 + 110.0 * channel) * t) + 0.05 * smoothed;
    // Every 125 ms a burst of the same noise decaying over 8 ms, much louder than the bed: dense
    // peaks the limiter has to hold down hard.
    const auto phase = frame % 6000U;
    value += 0.9 * std::exp(-static_cast<double>(phase) / 384.0) * noise(frame, channel + 2U);
    return value;
}

// One second of a 440 Hz stereo tone at 48 kHz, of constant 5.1 levels for Surround51, or of
// transientSample for Transients.
std::filesystem::path writeWav(const std::filesystem::path& path, const WavEncoding encoding) {
    constexpr std::uint32_t rate = 48000;
    const std::uint32_t channels = encoding == WavEncoding::Surround51 ? 6 : 2;
    std::vector<char> data;
    std::uint16_t format = 1, bits = 16, blockAlign = 4;
    std::vector<char> extra;
    if (encoding == WavEncoding::ImaAdpcm) {
        // 256-byte blocks of 505 frames; a zeroed block header and zero nibbles are valid.
        format = 0x11;
        bits = 4;
        blockAlign = static_cast<std::uint16_t>(256 * channels);
        put(extra, 505, 2);
        data.assign(static_cast<std::size_t>(blockAlign) * 96, '\0');
    } else if (encoding == WavEncoding::Surround51) {
        format = 0xFFFE;
        bits = 32;
        blockAlign = static_cast<std::uint16_t>(channels * 4);
        put(extra, 32, 2);   // valid bits
        put(extra, 0x3F, 4); // FL FR FC LFE BL BR
        for (const auto byte : {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00,
                                0xAA, 0x00, 0x38, 0x9B, 0x71}) // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            extra.push_back(static_cast<char>(byte));
        for (std::uint32_t frame = 0; frame < rate; ++frame)
            for (const auto level : kSurroundLevels) {
                std::uint32_t raw = 0;
                std::memcpy(&raw, &level, sizeof raw);
                put(data, raw, 4);
            }
    } else {
        const bool floating = encoding == WavEncoding::Float32 ||
                              encoding == WavEncoding::ConstantStereo ||
                              encoding == WavEncoding::Transients;
        format = floating ? 3 : 1;
        bits = encoding == WavEncoding::Pcm16 ? 16 : encoding == WavEncoding::Pcm24 ? 24 : 32;
        blockAlign = static_cast<std::uint16_t>(channels * bits / 8);
        for (std::uint32_t frame = 0; frame < rate; ++frame) {
            const double tone =
                encoding == WavEncoding::ConstantStereo
                    ? static_cast<double>(kConstantLevel)
                    : 0.25 * std::sin(2.0 * 3.141592653589793 * 440.0 * frame / double(rate));
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const double value =
                    encoding == WavEncoding::Transients ? transientSample(frame, channel) : tone;
                if (floating) {
                    const auto sample = static_cast<float>(value);
                    std::uint32_t raw = 0;
                    std::memcpy(&raw, &sample, sizeof raw);
                    put(data, raw, 4);
                } else {
                    const auto scale = encoding == WavEncoding::Pcm16 ? 32767.0 : 8388607.0;
                    const auto sample = static_cast<std::int64_t>(std::lround(value * scale));
                    put(data, static_cast<std::uint64_t>(sample), bits / 8);
                }
            }
        }
    }
    std::vector<char> fmt;
    put(fmt, format, 2);
    put(fmt, channels, 2);
    put(fmt, rate, 4);
    put(fmt, encoding == WavEncoding::ImaAdpcm ? rate * blockAlign / 505 : rate * blockAlign, 4);
    put(fmt, blockAlign, 2);
    put(fmt, bits, 2);
    if (!extra.empty()) {
        put(fmt, extra.size(), 2);
        fmt.insert(fmt.end(), extra.begin(), extra.end());
    }
    std::vector<char> file{'R', 'I', 'F', 'F'};
    put(file, 4 + 8 + fmt.size() + 8 + data.size(), 4);
    file.insert(file.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put(file, fmt.size(), 4);
    file.insert(file.end(), fmt.begin(), fmt.end());
    file.insert(file.end(), {'d', 'a', 't', 'a'});
    put(file, data.size(), 4);
    file.insert(file.end(), data.begin(), data.end());
    std::ofstream(path, std::ios::binary)
        .write(file.data(), static_cast<std::streamsize>(file.size()));
    return path;
}

host::ExportEncoderStatus linuxEncoders() {
    host::ExportEncoderStatus status;
    status.probed = true;
    status.mediaWorker = true;
    status.h264Software = output::H264EncoderV1::OpenH264;
    status.openh264 = host::ExportEncoderStatus::OpenH264::Installed;
    return status;
}

struct Fixture {
    std::filesystem::path folder;
    host::ProjectSessionIdentitySource identity;
    host::ProjectSession session;
    runtime::NodeDefinitionRegistry registry;
    runtime::SnapshotCompiler compiler{registry};
    runtime::TaskScheduler scheduler;
    host::PublicationCoordinator publications = [] {
        auto result = host::PublicationCoordinator::create();
        if (!result)
            throw std::runtime_error("publication coordinator");
        return std::move(*result);
    }();
    platform::StagedArtifactCoordinator artifacts = [] {
        auto result = platform::StagedArtifactCoordinator::create({});
        if (!result)
            throw std::runtime_error("artifact coordinator");
        return std::move(result).takeCoordinator();
    }();
    std::shared_ptr<output::ExportResourceLedgerV1> ledger =
        std::make_shared<output::ExportResourceLedgerV1>();
    std::shared_ptr<host::GpuExportProvider> gpu;
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement =
        host::SequenceExportRetirementOwnerV1::create();
    std::shared_ptr<host::ExportPresetCatalogue> presets;
    std::unique_ptr<host::ExportQueueService> service;
    document::AssetId audio;

    static host::ProjectSession makeSession(host::ProjectSessionIdentitySource& identity,
                                            const std::filesystem::path& folder,
                                            const std::uint32_t width, const std::uint32_t height) {
        auto format = document::CompositionFormat::create(width, height);
        auto created =
            document::makeNewProject("Audio", "Main", core::RationalTime::fromInteger(1), *format);
        auto path = host::ProjectDisplayPath::create(folder / "audio.bloom");
        auto result = host::ProjectSession::createDecoded(
            identity, {.project = std::move(created.project),
                       .colorSettings = document::makeBloomNeutralColorSettingsV1({}),
                       .editability = host::DecodedProjectEditability::Editable,
                       .displayPath = std::move(path),
                       .persistedAllocatorHighWater = std::nullopt});
        if (!result)
            throw std::runtime_error("session fixture");
        return std::move(result).takeSession();
    }

    // The composition carries one audio layer reading `source`.
    // `compilable == false` leaves the compiler's registry empty, so no composition compiles.
    // `media`, when given, is the source instead of a written WAV: a copy of that file.
    // `width` x `height` is the composition's size (DNxHR needs more than the default).
    Fixture(const std::string& name, const WavEncoding source, const bool compilable = true,
            const std::filesystem::path& media = {}, const std::uint32_t width = 64,
            const std::uint32_t height = 32)
        : folder(scratch(name)), session(makeSession(identity, folder, width, height)) {
        if (compilable && !runtime::registerBuiltInNodeDefinitions(registry))
            throw std::runtime_error("registry");
        registry.freeze();
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
        options.enabled = false;
        gpu = host::GpuExportProvider::create(std::move(options));
        gpu->prepare(scheduler);
        presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = linuxEncoders()});
        service = std::make_unique<host::ExportQueueService>(
            host::ExportQueueServiceDependencies{.session = session,
                                                 .scheduler = scheduler,
                                                 .compiler = compiler,
                                                 .publications = publications,
                                                 .artifacts = artifacts,
                                                 .ledger = ledger,
                                                 .gpu = gpu,
                                                 .retirement = retirement,
                                                 .presets = presets,
                                                 .limits = {},
                                                 .displayProcessor = nullptr,
                                                 .scratchDirectory = folder / "scratch",
                                                 .backend = nullptr});
        std::filesystem::create_directories(folder / "scratch");
        std::filesystem::create_directories(folder / "Exports");
        auto wav = folder / "tone.wav";
        if (media.empty()) {
            wav = writeWav(wav, source);
        } else {
            wav = folder / media.filename();
            std::filesystem::copy_file(media, wav);
        }
        commands::Transaction import("Import");
        import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{wav}, folder);
        if (!session.execute(std::move(import)).changed())
            throw std::runtime_error("import the audio source");
        audio = currentSnapshot(session).project().assets().back().id;
        commands::Transaction layer("Add audio layer");
        layer.emplace<commands::AddDroppedLayers>(composition(),
                                                  std::vector<commands::LayerDropItem>{audio},
                                                  commands::LayerDropPlacement{});
        if (!session.execute(std::move(layer)).changed())
            throw std::runtime_error("add the audio layer");
    }
    ~Fixture() {
        service->beginShutdown(host::ExportStopCause::ApplicationQuit);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!service->quiescent() && std::chrono::steady_clock::now() < deadline) {
            service->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        service.reset();
        scheduler.beginShutdown();
        retirement->beginShutdown();
        while (!scheduler.isQuiescent() || !retirement->retirementComplete())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!retirement->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        (void)gpu->shutdownAndWait(std::chrono::seconds(10));
        std::filesystem::remove_all(folder);
    }

    document::CompositionId composition() const {
        return currentSnapshot(session).project().compositions().front().id();
    }

    document::ExportJobId addJob(document::ExportOutputSettings settings, std::string path) {
        document::ExportJob job;
        job.composition = composition();
        job.render.range = {document::ExportRangeKind::Composition, 0, 0};
        document::ExportOutput output;
        output.presetId = "test";
        output.settings = std::move(settings);
        output.destination = {"project-relative", std::move(path),
                              document::ExportOverwritePolicy::Replace};
        job.outputs = {output};
        commands::Transaction transaction("Add export job");
        transaction.emplace<commands::AddExportJob>(job);
        const auto result = session.execute(std::move(transaction));
        if (!result.changed())
            throw std::runtime_error("add job");
        return *result.command->outputId<document::ExportJobId>(commands::kExportJobOutput);
    }

    std::optional<host::ExportJobView> view(const document::ExportJobId id) const {
        service->pump();
        for (const auto& job : service->snapshot()->jobs)
            if (job.id == id)
                return job;
        return std::nullopt;
    }

    template <typename Predicate> bool pumpUntil(Predicate&& done, const int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            service->pump();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    // Starts the job and waits for its end; the finished view.
    std::optional<host::ExportJobView> run(const document::ExportJobId job,
                                           const std::string& name) {
        expect(pumpUntil([&] { return service->canStart(job).available; }, 20),
               name + ": the job becomes startable");
        const document::ExportJobId ids[] = {job};
        expect(service->start(ids).accepted, name + ": start");
        expect(pumpUntil([&] { return host::isTerminal(view(job)->state); }, 120),
               name + ": the job finishes");
        // A failed job's log holds the technical detail under its plain sentence.
        if (const auto finished = view(job); finished && finished->failure)
            for (const auto& entry : service->log(job, 0).entries)
                std::cerr << "  " << name << " log: " << entry.code << ": " << entry.message
                          << '\n';
        return view(job);
    }
};

// The audio codec a published file carries, read from its bytes (src/host may not reach the media
// reader): the WAV format chunk's sample size, the QuickTime sample entry ('sowt' is 16-bit and
// 'in24' 24-bit little-endian PCM, 'mp4a' AAC), or WebM's Opus codec ID.
std::string audioCodecOf(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (path.extension() == ".wav") {
        const auto fmt = bytes.find("fmt ");
        if (fmt == std::string::npos || fmt + 24 > bytes.size())
            return {};
        const auto bits = static_cast<unsigned char>(bytes[fmt + 22]);
        return bits == 16 ? "pcm_s16le" : bits == 24 ? "pcm_s24le" : "bits=" + std::to_string(bits);
    }
    for (const auto& [marker, codec] : {std::pair<std::string, std::string>{"sowt", "pcm_s16le"},
                                        {"in24", "pcm_s24le"},
                                        {"mp4a", "aac"},
                                        {"A_OPUS", "opus"}})
        if (bytes.find(marker) != std::string::npos)
            return codec;
    // QuickTime's general PCM sample entry ('lpcm', sound description version 2), which the macOS
    // writer uses: the sample size and the format flags are fields of the description.
    if (const auto entry = bytes.find("lpcm");
        entry != std::string::npos && entry + 60 <= bytes.size()) {
        const auto field = [&](const std::size_t offset) {
            std::uint32_t value = 0;
            for (std::size_t index = 0; index < 4; ++index)
                value =
                    (value << 8U) | static_cast<unsigned char>(bytes[entry + 4 + offset + index]);
            return value;
        };
        const auto bits = field(48), flags = field(52);
        const bool floating = (flags & 1U) != 0, bigEndian = (flags & 2U) != 0;
        return std::string(floating ? "pcm_f" : "pcm_s") + std::to_string(bits) +
               (bigEndian ? "be" : "le");
    }
    return {};
}

// Done, or done with warnings (a review deliverable warns that it is not archival).
bool published(const std::optional<host::ExportJobView>& view) {
    return view &&
           (view->state == ExportJobState::Done || view->state == ExportJobState::DoneWithWarnings);
}

// The channel count and one frame of a published 24-bit PCM WAV, read from its chunks.
std::pair<unsigned, std::vector<double>> wavFrame(const std::filesystem::path& path,
                                                  const std::size_t frame) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u = [&](const std::size_t at, const unsigned count) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= std::uint64_t{static_cast<unsigned char>(bytes[at + i])} << (8U * i);
        return value;
    };
    unsigned channels = 0, bits = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const auto id = bytes.substr(at, 4);
        const auto size = static_cast<std::size_t>(u(at + 4, 4));
        if (id == "fmt " && at + 24 <= bytes.size()) {
            channels = static_cast<unsigned>(u(at + 10, 2));
            bits = static_cast<unsigned>(u(at + 22, 2));
        } else if (id == "data" && channels != 0 && bits == 24) {
            const auto first = at + 8 + frame * channels * 3;
            if (first + channels * 3 > bytes.size())
                return {channels, {}};
            std::vector<double> samples;
            for (unsigned c = 0; c < channels; ++c) {
                auto raw = static_cast<std::int32_t>(u(first + c * 3, 3) << 8) >> 8;
                samples.push_back(static_cast<double>(raw) / 8388608.0);
            }
            return {channels, samples};
        }
        at += 8 + size + (size & 1U);
    }
    return {channels, {}};
}

// Every sample of a published 24-bit PCM WAV, interleaved, and its channel count.
std::pair<unsigned, std::vector<double>> wavSamples(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u = [&](const std::size_t at, const unsigned count) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= std::uint64_t{static_cast<unsigned char>(bytes[at + i])} << (8U * i);
        return value;
    };
    unsigned channels = 0, bits = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const auto id = bytes.substr(at, 4);
        const auto size = static_cast<std::size_t>(u(at + 4, 4));
        if (id == "fmt " && at + 24 <= bytes.size()) {
            channels = static_cast<unsigned>(u(at + 10, 2));
            bits = static_cast<unsigned>(u(at + 22, 2));
        } else if (id == "data" && channels != 0 && bits == 24) {
            std::vector<double> samples;
            for (std::size_t sample = at + 8; sample + 3 <= std::min(bytes.size(), at + 8 + size);
                 sample += 3) {
                const auto raw = static_cast<std::int32_t>(u(sample, 3) << 8) >> 8;
                samples.push_back(static_cast<double>(raw) / 8388608.0);
            }
            return {channels, samples};
        }
        at += 8 + size + (size & 1U);
    }
    return {channels, {}};
}

// #459: an automated audio level exports as a continuous ramp. The level of a constant source
// is keyed 0 at 0 s and 1 at 0.5 s; the export mix evaluates the graph once per 512-sample block,
// and used to hold each block's start level, a 0.0107 jump every block (a zipper). Now adjacent
// samples differ by the ramp's own slope, about 2e-5.
void testLevelAutomationRampsWithinBlocks() {
    Fixture f("level-ramp", WavEncoding::ConstantStereo);
    const auto snapshot = currentSnapshot(f.session);
    const auto& graph = snapshot.project().findComposition(f.composition())->graph();
    std::optional<document::ParameterId> level;
    for (const auto& node : graph.nodes())
        if (node.typeId == document::kAudioSourceNodeType)
            for (const auto& binding : node.parameters)
                if (binding.role == "level")
                    level = binding.parameterId;
    expect(level.has_value(), "level ramp: the audio layer has a level parameter");
    if (!level)
        return;
    commands::Transaction automate("Automate level");
    automate.emplace<commands::CreateAnimationForParameter>(f.composition(), *level,
                                                            core::RationalTime{});
    automate.emplace<commands::SetKeyframeAtTimeForParameter>(f.composition(), *level,
                                                              core::RationalTime{}, 0.0);
    automate.emplace<commands::SetKeyframeAtTimeForParameter>(
        f.composition(), *level, *core::RationalTime::create(1, 2), 1.0);
    expect(f.session.execute(std::move(automate)).changed(), "level ramp: the level is keyed");
    document::ExportOutputSettings settings;
    settings.format = ExportFormat::Wav;
    settings.codecProfile = "s24";
    settings.includeAudio = true;
    const std::string file = "Exports/level-ramp.wav";
    const auto finished = f.run(f.addJob(settings, file), "level ramp");
    if (finished && finished->failure)
        std::cerr << "  level ramp: " << finished->failure->reason << '\n';
    expect(published(finished), "level ramp: exports and publishes");
    const auto [channels, samples] = wavSamples(f.folder / file);
    expect(channels == 2 && samples.size() >= 2 * 24000, "level ramp: a stereo WAV of the ramp");
    if (channels != 2 || samples.size() < 2 * 24000)
        return;
    double largest = 0.0;
    for (std::size_t frame = 1; frame < 24000; ++frame)
        largest = std::max(largest, std::abs(samples[frame * 2] - samples[(frame - 1) * 2]));
    expect(largest < 1e-3, "level ramp: the fade is continuous, the largest sample step is " +
                               std::to_string(largest));
    expect(std::abs(samples[12000 * 2] - 0.5 * static_cast<double>(kConstantLevel)) < 1e-3,
           "level ramp: a quarter second in, the level is halfway (got " +
               std::to_string(samples[12000 * 2]) + ")");
}

// #569: the export queue's own mix (`CompositionOutputStream`) runs a layer's effect chain and
// writes the samples the playback mixer plays for the same chain. The reference is the mixer fed
// once with the clips of the evaluated mix, as playback feeds it; the export re-evaluates the mix
// and replaces its clips every block, and its file is 24-bit PCM, so the two agree to within the
// quantization step.
void testEffectChainExportsAsPlayback() {
    Fixture f("effects", WavEncoding::Float32);
    const auto graphOf = [&](const document::Snapshot& snapshot) -> const document::CanonicalGraph& {
        return snapshot.project().findComposition(f.composition())->graph();
    };
    const auto run = [&](commands::Transaction transaction, const std::string& what) {
        expect(f.session.execute(std::move(transaction)).changed(), "effects: " + what);
    };
    // Wires a new effect between the layer and whatever fed its audio input.
    const auto addEffect = [&](const std::string_view type) {
        commands::Transaction add("Add effect");
        add.emplace<commands::AddNode>(f.composition(), std::string(type),
                                       document::Vec2d{0.0, 0.0});
        run(std::move(add), "the effect node is added");
        const auto snapshot = currentSnapshot(f.session);
        const auto& graph = graphOf(snapshot);
        document::NodeId effect, layer;
        for (const auto& node : graph.nodes()) {
            if (node.typeId == type)
                effect = node.id;
            if (node.typeId == document::kLayerOutputNodeType)
                layer = node.id;
        }
        const document::InputPortRef layerAudio =
            document::NodeInputRef{layer, std::string(document::kLayerOutputAudioInputPort)};
        const auto fed = std::ranges::find_if(
            graph.edges(), [&](const auto& edge) { return edge.destination == layerAudio; });
        if (fed == graph.edges().end())
            throw std::runtime_error("effects: the layer has no audio input");
        const auto upstream = fed->source;
        commands::Transaction wire("Wire effect");
        wire.emplace<commands::DisconnectInput>(f.composition(), layerAudio);
        wire.emplace<commands::ConnectPorts>(f.composition(), upstream,
                                             document::NodeInputRef{effect, "audio"});
        wire.emplace<commands::ConnectPorts>(f.composition(),
                                             document::OutputPortRef{effect, "audio"}, layerAudio);
        run(std::move(wire), "the effect is wired into the layer");
        return effect;
    };
    const auto set = [&](const document::NodeId node, const std::string& role, const double value) {
        commands::Transaction transaction("Set effect parameter");
        transaction.emplace<commands::SetNodeParameterValue>(f.composition(), node, role,
                                                             document::ParameterValue{value});
        run(std::move(transaction), role + " is set");
    };
    const auto equalizer = addEffect(document::kAudioEqualizerNodeType);
    set(equalizer, "highPass", 150.0);
    set(equalizer, "midGain", 5.0);
    const auto compressor = addEffect(document::kAudioCompressorNodeType);
    set(compressor, "threshold", -20.0);
    set(compressor, "ratio", 4.0);
    const auto hiss = addEffect(document::kAudioHissNodeType);
    set(hiss, "level", -45.0);

    document::ExportOutputSettings settings;
    settings.format = ExportFormat::Wav;
    settings.codecProfile = "s24";
    settings.includeAudio = true;
    const std::string file = "Exports/effects.wav";
    const auto finished = f.run(f.addJob(settings, file), "effects");
    if (finished && finished->failure)
        std::cerr << "  effects: " << finished->failure->reason << '\n';
    expect(published(finished), "effects: exports and publishes");
    const auto [channels, samples] = wavSamples(f.folder / file);
    constexpr std::size_t frames = 48000;
    expect(channels == 2 && samples.size() == 2 * frames,
           "effects: a stereo WAV of the one-second programme (got " +
               std::to_string(samples.size() / 2) + " frames)");
    if (channels != 2 || samples.size() != 2 * frames)
        return;

    // The playback side: the source as the fixture wrote it, and the mix evaluated once.
    std::vector<float> plane(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        plane[frame] = static_cast<float>(
            0.25 * std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(frame) / 48000.0));
    const auto compiled = f.compiler.compile({currentSnapshot(f.session), f.composition()}, {});
    expect(compiled.plan != nullptr, "effects: the composition compiles");
    if (!compiled.plan)
        return;
    const auto mix =
        runtime::CpuCompositionEvaluator{}.evaluateAudioMix(compiled.plan, core::RationalTime{});
    expect(mix && mix->clips.size() == 1 && mix->clips.front().effects.size() == 3,
           "effects: the mix carries the layer's three effects");
    if (!mix || mix->clips.size() != 1)
        return;
    const auto reference = test::playbackMixReference(*mix, plane, 48000, frames);
    expect(reference.has_value() && reference->size() == 2 * frames,
           "effects: the playback mixer renders every frame of the programme");
    if (!reference || reference->size() != 2 * frames)
        return;
    const auto& played = *reference;
    double worst = 0.0, changed = 0.0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        worst = std::max(worst, std::abs(samples[index] - static_cast<double>(played[index])));
        changed = std::max(changed,
                           std::abs(samples[index] - static_cast<double>(plane[index / 2])));
    }
    // One 24-bit step is 1.2e-7.
    expect(worst < 2.5e-7, "effects: the export equals playback to the 24-bit step (worst " +
                               std::to_string(worst * 1e6) + "e-6)");
    expect(changed > 0.02, "effects: the chain changed the source (largest change " +
                               std::to_string(changed) + ")");
}

// #450: the export mix downmixes a 5.1 source to its stereo output with ITU-R BS.775 levels
// (centre and surrounds at -3 dB, LFE dropped), the same mix the playback engine applies.
void testSurroundSourceDownmixesToStereo() {
    Fixture f("surround", WavEncoding::Surround51);
    document::ExportOutputSettings settings;
    settings.format = ExportFormat::Wav;
    settings.codecProfile = "s24";
    settings.includeAudio = true;
    const std::string file = "Exports/surround.wav";
    const auto finished = f.run(f.addJob(settings, file), "5.1 source");
    if (finished && finished->failure)
        std::cerr << "  5.1 source: " << finished->failure->reason << '\n';
    expect(published(finished), "5.1 source: exports and publishes");
    constexpr double h = 0.70710678118654752440;
    const auto v = [](const std::size_t channel) {
        return static_cast<double>(kSurroundLevels[channel]);
    };
    const double left = v(0) + h * v(2) + h * v(4);
    const double right = v(1) + h * v(2) + h * v(5);
    const auto [channels, samples] = wavFrame(f.folder / file, 24000);
    expect(channels == 2 && samples.size() == 2, "5.1 source: the export is a stereo WAV");
    if (samples.size() == 2)
        expect(std::abs(samples[0] - left) < 1e-4 && std::abs(samples[1] - right) < 1e-4,
               "5.1 source: stereo carries the BS.775 downmix (got " + std::to_string(samples[0]) +
                   ", " + std::to_string(samples[1]) + "; expected " + std::to_string(left) + ", " +
                   std::to_string(right) + ")");
}

struct Preset {
    ExportFormat format;
    std::string profile;
    std::string extension;
    // The audio codec the container carries for a 16-bit source and for a deeper one.
    std::string shallowCodec, deepCodec;
};

void testSourceFormatsAcrossPresets() {
    const std::vector<Preset> presets{
        {ExportFormat::ProResMov, "hq", "mov", "pcm_s16le", "pcm_s24le"},
        {ExportFormat::HevcMov, "main", "mov", "pcm_s16le", "pcm_s24le"},
        {ExportFormat::H264Mp4, "high", "mp4", "aac", "aac"},
        {ExportFormat::Vp9Webm, "8bit", "webm", "opus", "opus"},
        {ExportFormat::Wav, "s24", "wav", "pcm_s24le", "pcm_s24le"},
        {ExportFormat::Wav, "s16", "wav", "pcm_s16le", "pcm_s16le"},
    };
    const std::pair<WavEncoding, std::string> sources[] = {{WavEncoding::Pcm16, "pcm16"},
                                                           {WavEncoding::Pcm24, "pcm24"},
                                                           {WavEncoding::Float32, "f32"}};
    for (const auto& [encoding, label] : sources) {
        Fixture f("formats-" + label, encoding);
        for (const auto& preset : presets) {
            document::ExportOutputSettings settings;
            settings.format = preset.format;
            settings.codecProfile = preset.profile;
            settings.includeAudio = true;
            const auto file = "Exports/" + label + "-" + preset.profile + "." + preset.extension;
            const auto name = label + " source, " + preset.extension + " " + preset.profile;
            const auto finished = f.run(f.addJob(settings, file), name);
            if (finished && finished->failure)
                std::cerr << "  " << name << ": " << finished->failure->reason << '\n';
            expect(published(finished) && std::filesystem::exists(f.folder / file),
                   name + ": exports and publishes");
            // #465: the reopened duration and sample count match the request.
            if (finished && !finished->outputs.empty() && finished->outputs.front().verification)
                for (const auto what : {host::ExportCheck::Timing, host::ExportCheck::Audio}) {
                    const auto& checks = finished->outputs.front().verification->checks;
                    const auto found =
                        std::ranges::find(checks, what, &host::ExportCheckResult::check);
                    expect(
                        found != checks.end() && found->outcome == host::ExportCheckOutcome::Passed,
                        name + ": the " + (what == host::ExportCheck::Timing ? "Timing" : "Audio") +
                            " check passes");
                }
            const auto& expected =
                encoding == WavEncoding::Pcm16 ? preset.shallowCodec : preset.deepCodec;
            const auto actual = audioCodecOf(f.folder / file);
            expect(actual == expected,
                   name + ": the container carries " + expected + " (found " + actual + ")");
        }
    }
}

// #761: every built-in movie and WAV preset, exported with the audio of a 16-bit source. The
// verifying reopen's stream list equals an independent probe of the published file (the media
// worker's intake probe, the platform's own reader where it has one), and the codecs the preset
// states equal the ones written. PCM's bit rate is exact; a lossy stream's is positive, and the
// bytes the streams' rates account for fit in the file. Returns the presets this computer cannot
// export (each named), which the caller reports as a skip, never as a pass.
std::size_t testWrittenStreamsForEveryPreset() {
    Fixture f("streams", WavEncoding::Pcm16, true, {}, 256, 128);
    std::size_t unavailable = 0;
    std::size_t walked = 0;
    const auto catalogue = f.presets->snapshot();
    for (const auto& preset : catalogue->presets) {
        if (preset.origin != host::ExportPresetOrigin::BuiltIn || !preset.capabilities.singleFile)
            continue;
        auto settings = preset.settings;
        settings.includeAudio = true;
        const auto name = preset.id;
        // A deliverable that needs another project color configuration (the review H.264 asks for
        // ACES names) writes the same streams as the raw preset of its format and profile, which
        // this walk exports; it is named and checked against that preset.
        if (const auto project = f.service->projectAvailability(settings);
            preset.projectDependent && !project.available) {
            const auto same = std::ranges::find_if(catalogue->presets, [&](const auto& other) {
                return other.id != preset.id && !other.projectDependent &&
                       other.origin == host::ExportPresetOrigin::BuiltIn &&
                       other.settings.format == settings.format &&
                       other.settings.codecProfile == settings.codecProfile;
            });
            expect(same != catalogue->presets.end(),
                   name + ": a raw preset of the same format and profile is walked");
            if (same != catalogue->presets.end()) {
                auto other = same->settings;
                other.includeAudio = true;
                expect(host::exportDeclaredCodecs(other) == host::exportDeclaredCodecs(settings),
                       name + ": states the codecs of " + same->id);
                std::cerr << "  " << name << ": needs " << project.reason << "; its streams are "
                          << same->id << "'s\n";
            }
            continue;
        }
        if (!preset.platformAvailability.available) {
            std::cerr << "  " << name << ": not available here (" << preset.platformAvailability.reason
                      << ")\n";
            ++unavailable;
            continue;
        }
        ++walked;
        const auto declared = host::exportDeclaredCodecs(settings);
        const auto extension = std::string(host::exportFormatExtension(settings.format));
        const auto file = "Exports/streams-" + preset.id.substr(preset.id.find(':') + 1) + "." +
                          extension;
        const auto finished = f.run(f.addJob(settings, file), name);
        if (finished && finished->failure)
            std::cerr << "  " << name << ": " << finished->failure->reason << '\n';
        expect(published(finished), name + ": exports and publishes");
        if (!published(finished) || finished->outputs.empty() ||
            !finished->outputs.front().verification)
            continue;
        const auto& verification = *finished->outputs.front().verification;
        const auto& written = verification.streams;
        const bool movie = settings.format != ExportFormat::Wav;
        expect(written.size() == (movie ? 2U : 1U),
               name + ": the result lists " + (movie ? "a video and an audio" : "an audio") +
                   " stream (got " + std::to_string(written.size()) + ")");
        // The independent probe of the published file.
        media::video::VideoDecodeSession session(f.folder / file);
        const auto probed = session.probe();
        const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
        expect(probe != nullptr, name + ": the published file probes");
        if (probe == nullptr)
            continue;
        std::vector<media::provider::StreamDescriptor> streams;
        for (const auto& stream : probe->streams)
            if (stream.kind != media::provider::MediaKind::Data)
                streams.push_back(stream);
        expect(streams.size() == written.size(),
               name + ": the probe finds as many streams as the result lists");
        std::uint64_t accounted = 0;
        for (std::size_t index = 0; index < std::min(streams.size(), written.size()); ++index) {
            const auto& w = written[index];
            const auto& p = streams[index];
            const auto label = name + " stream " + std::to_string(index) + " (" +
                               host::exportWrittenStreamText(w) + ")";
            const bool video = w.kind == host::ExportStreamKind::Video;
            expect(video == (p.kind == media::provider::MediaKind::Video),
                   label + ": the kind equals the probe's");
            expect(w.codec == p.codec || (p.codec == "prores_ks" && w.codec == "prores"),
                   label + ": the codec equals the probe's (" + p.codec + ")");
            expect(p.profile.empty() || p.profile == "unknown" || w.profile == p.profile,
                   label + ": the profile equals the probe's (" + p.profile + ")");
            if (video) {
                expect(w.width == p.width && w.height == p.height,
                       label + ": the size equals the probe's (" + std::to_string(p.width) + "x" +
                           std::to_string(p.height) + ")");
                expect(w.frameRate.numerator == p.rate.numerator &&
                           w.frameRate.denominator == p.rate.denominator,
                       label + ": the frame rate equals the probe's (" +
                           std::to_string(p.rate.numerator) + "/" +
                           std::to_string(p.rate.denominator) + ")");
                expect(w.codec == declared.videoCodec && w.profile == declared.videoProfile,
                       label + ": the preset states this codec and profile (" +
                           declared.videoCodec + " " + declared.videoProfile + ")");
                expect(w.bitrate > 0, label + ": the bit rate is positive");
                accounted += w.bitrate; // over one second
            } else {
                expect(w.channels == p.channelLayout.size() && w.sampleRate == p.sampleRate,
                       label + ": the channels and sample rate equal the probe's (" +
                           std::to_string(p.channelLayout.size()) + " ch, " +
                           std::to_string(p.sampleRate) + " Hz)");
                expect(w.codec == declared.audioCodec && w.profile == declared.audioProfile &&
                           w.sampleRate == declared.audioSampleRate,
                       label + ": the preset states this audio (" + declared.audioCodec + " " +
                           declared.audioProfile + " " + std::to_string(declared.audioSampleRate) +
                           " Hz)");
                if (w.codec.starts_with("pcm_")) {
                    const std::uint64_t bits = w.codec == "pcm_s16le" ? 16U : 24U;
                    expect(w.bitrate == bits * w.sampleRate * w.channels,
                           label + ": PCM's bit rate is its sample rate, depth and channels");
                    expect(!declared.audioBitrateKbps, label + ": the preset states no PCM rate");
                } else {
                    expect(w.bitrate > 0 && declared.audioBitrateKbps == 192U,
                           label + ": a lossy stream's rate is positive, and 192 kb/s is stated");
                }
                accounted += w.bitrate;
            }
        }
        // ProRes: the MOV sample entry's four-character code names the profile on its own.
        if (declared.videoCodec == "prores") {
            std::ifstream in(f.folder / file, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            const std::string fourcc = declared.videoProfile == "Proxy"      ? "apco"
                                       : declared.videoProfile == "LT"       ? "apcs"
                                       : declared.videoProfile == "Standard" ? "apcn"
                                       : declared.videoProfile == "HQ"       ? "apch"
                                       : declared.videoProfile == "4444"     ? "ap4h"
                                                                             : "ap4x";
            expect(bytes.find(fourcc) != std::string::npos,
                   name + ": the MOV sample entry is " + fourcc);
        }
        // One second of every stream fits in the file (headers and index on top).
        const auto bytes = std::filesystem::file_size(f.folder / file);
        expect(accounted / 8U <= bytes,
               name + ": the streams' bytes fit in the file (" + std::to_string(accounted / 8U) +
                   " of " + std::to_string(bytes) + ")");
        const auto container = std::ranges::find(verification.checks, host::ExportCheck::Container,
                                                 &host::ExportCheckResult::check);
        const auto* observed = container == verification.checks.end()
                                   ? nullptr
                                   : std::get_if<std::string>(&container->observed);
        expect(observed != nullptr && observed->starts_with(declared.container + "/"),
               name + ": the preset states the container (" + declared.container + ")");
        // The log carries the same list.
        const auto entries = f.service->log(finished->id, 0).entries;
        expect(static_cast<std::size_t>(std::ranges::count(entries, std::string("export.stream"),
                                                           &host::ExportLogEntry::code)) ==
                   written.size(),
               name + ": the export log lists every written stream");
    }
    expect(walked + unavailable >= 10, "every built-in movie and WAV preset is walked");
    return unavailable;
}

void testUndecodableSourceIsRefusedBeforeRendering() {
    Fixture f("adpcm", WavEncoding::ImaAdpcm);
    const auto expectedReason = "Audio asset " + std::to_string(f.audio.value()) +
                                " (adpcm_ima_wav) is not accepted by the export audio path.";
    expect(!output::exportAudioPathDecodesV1("adpcm_ima_wav") &&
               output::exportAudioPathDecodesV1("pcm_s24le") &&
               output::exportAudioPathDecodesV1("pcm_f32le"),
           "the export audio path decodes PCM and float but not IMA ADPCM");
    // #435: the queue samples the composition's sources on a worker, so the refusal is on the
    // outputs and in Start's availability before anything runs.
    for (const auto format : {ExportFormat::ProResMov, ExportFormat::Wav}) {
        document::ExportOutputSettings settings;
        settings.format = format;
        settings.codecProfile = format == ExportFormat::Wav ? "s24" : "hq";
        settings.includeAudio = true;
        const auto file =
            std::string("Exports/adpcm.") + (format == ExportFormat::Wav ? "wav" : "mov");
        const auto job = f.addJob(settings, file);
        const auto refused = [&] {
            const auto view = f.view(job);
            return view && view->audioSourceRefusal && !view->outputs.empty() &&
                   !view->outputs.front().availability.available;
        };
        expect(f.pumpUntil(refused, 20), file + ": the worker sample refuses the audio source");
        const auto view = f.view(job);
        if (!view || view->outputs.empty())
            continue;
        const auto& output = view->outputs.front();
        expect(view->audioSourceRefusal == expectedReason,
               file + ": the job names the stage and the input (got \"" +
                   view->audioSourceRefusal.value_or("") + "\")");
        expect(output.availability.code == host::ExportAvailabilityCode::AudioSourceUnsupported &&
                   output.availability.reason == expectedReason,
               file + ": the output is unavailable for the source");
        expect(std::ranges::any_of(
                   output.preflight,
                   [&](const auto& issue) {
                       return issue.code == host::ExportPreflightCode::AudioSourceUnsupported &&
                              issue.target == host::ExportPreflightTarget::Audio &&
                              issue.severity == host::ExportPreflightSeverity::Blocking &&
                              issue.reason == expectedReason;
                   }),
               file + ": the panel's preflight shows the reason");
        const auto start = f.service->canStart(job);
        expect(!start.available && start.code == host::ExportUnavailableCode::PreflightBlocked &&
                   start.reason == expectedReason,
               file + ": Start is refused with the reason before the job runs");
        const document::ExportJobId ids[] = {job};
        expect(!f.service->start(ids).accepted && !std::filesystem::exists(f.folder / file),
               file + ": starting is refused and nothing is written");
        // Preset availability for the composition (what export.presets reports for the job).
        expect(!host::exportSourceAvailability(settings, view->audioSourceRefusal).available,
               file + ": an audio-carrying preset is unavailable for this composition");
    }
    document::ExportOutputSettings silent;
    silent.format = ExportFormat::ProResMov;
    silent.codecProfile = "hq";
    silent.includeAudio = false;
    expect(host::exportSourceAvailability(silent, expectedReason).available,
           "the same preset without audio stays available");
    // Without audio the same composition still exports.
    const auto finished = f.run(f.addJob(silent, "Exports/silent.mov"), "ADPCM without audio");
    expect(published(finished), "a composition with an undecodable source exports without audio");

    // The run's own source-aware preflight (scripted, CLI and queue-bypassing routes) still
    // refuses before rendering, naming the stage and the input, and never blames the encoder.
    const auto snapshot = currentSnapshot(f.session);
    const auto* composition = snapshot.project().findComposition(f.composition());
    const auto rate = composition->format().frameRate();
    const auto duration = composition->duration();
    host::SequenceExportRequestV1 request{
        .composition = {snapshot, composition->id()},
        .range = {.destination = f.folder / "Exports/direct.mov",
                  .firstFrame = 0,
                  .lastFrame =
                      static_cast<std::uint64_t>(duration.numerator() * rate.numerator() /
                                                 (duration.denominator() * rate.denominator())) -
                      1,
                  .frameRate = rate,
                  .duration = duration},
        .preset = output::OutputPresetV1::ProResMovV1,
        .profile = "hq",
        .audio = true,
        .sampleRate = 48000,
        .pcmCodec = "pcm_s16le",
        .bwfDescription = {},
        .assetBaseDirectory = f.folder,
        .worker = {}};
    request.gpuProvider = f.gpu;
    host::SequenceExportRunnerV1 runner(f.scheduler, f.compiler, f.publications, f.artifacts,
                                        *f.ledger, std::move(request), *f.retirement, f.ledger);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!runner.result() && std::chrono::steady_clock::now() < deadline) {
        runner.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!runner.result())
        runner.cancel();
    while (!runner.result())
        runner.poll();
    const auto& result = *runner.result();
    expect(!result.published() && result.failure &&
               result.failureStage == host::SequenceExportFailureStageV1::AudioSourcePreflight &&
               result.failure->detail == expectedReason && result.encodedFrames == 0,
           "the run's preflight refuses the source before rendering");
}

// #424: a run that fails after encoding frames keeps their route provenance. The artifact limit
// is half of what the unlimited run wrote, so the encoder refuses the stream partway.
void testFailedRunKeepsFrameProvenance() {
    Fixture f("provenance", WavEncoding::Pcm16);
    const auto snapshot = currentSnapshot(f.session);
    const auto* composition = snapshot.project().findComposition(f.composition());
    const auto export_ = [&](const std::filesystem::path& destination,
                             const std::optional<std::uint64_t> limit) {
        const auto rate = composition->format().frameRate();
        const auto duration = composition->duration();
        host::SequenceExportRequestV1 request{
            .composition = {snapshot, composition->id()},
            .range = {.destination = destination,
                      .firstFrame = 0,
                      .lastFrame = static_cast<std::uint64_t>(
                                       duration.numerator() * rate.numerator() /
                                       (duration.denominator() * rate.denominator())) -
                                   1,
                      .frameRate = rate,
                      .duration = duration},
            .preset = output::OutputPresetV1::ProResMovV1,
            .profile = "hq",
            .audio = true,
            .sampleRate = 48000,
            .pcmCodec = "pcm_s16le",
            .bwfDescription = {},
            .assetBaseDirectory = f.folder,
            .worker = {}};
        request.artifactByteLimit = limit;
        request.gpuProvider = f.gpu;
        host::SequenceExportRunnerV1 runner(f.scheduler, f.compiler, f.publications, f.artifacts,
                                            *f.ledger, std::move(request), *f.retirement, f.ledger);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (!runner.result() && std::chrono::steady_clock::now() < deadline) {
            runner.poll();
            if (runner.stage() == host::SequenceExportStageV1::AwaitingApproval) {
                const auto* analysis = runner.analysis();
                const auto digest = runner.frameApprovalDigest();
                if (!analysis || !digest || !runner.approve(analysis->digest, *digest))
                    runner.cancel();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!runner.result())
            runner.cancel();
        while (!runner.result())
            runner.poll();
        return *runner.result();
    };
    const auto full = export_(f.folder / "Exports/full.mov", std::nullopt);
    expect(full.published(), "the unlimited run publishes");
    std::error_code error;
    const auto bytes = std::filesystem::file_size(f.folder / "Exports/full.mov", error);
    expect(!error && bytes > 0, "the unlimited run wrote a file");
    if (error || bytes == 0)
        return;
    const auto limited = export_(f.folder / "Exports/limited.mov", bytes / 2);
    expect(!limited.published() && limited.failure.has_value(),
           "the half-size limit fails the run");
    expect(limited.encodedFrames > 0, "the limited run encoded frames before failing");
    expect(limited.gpuDisplayFrames + limited.cpuDisplayFrames >= limited.encodedFrames,
           "the failure carries every encoded frame's route provenance (" +
               std::to_string(limited.encodedFrames) + " encoded, " +
               std::to_string(limited.gpuDisplayFrames) + " GPU display, " +
               std::to_string(limited.cpuDisplayFrames) + " CPU display)");
}

// #437: a movie or WAV export whose composition does not compile fails with CompileFailed and the
// sentence a still export reports, not the generic encoder-failure summary.
void testCompileFailureNamesItsSubject() {
    Fixture f("compile-failure", WavEncoding::Pcm16, false);
    for (const auto format : {ExportFormat::ProResMov, ExportFormat::Wav}) {
        document::ExportOutputSettings settings;
        settings.format = format;
        settings.codecProfile = format == ExportFormat::Wav ? "s24" : "hq";
        settings.includeAudio = true;
        const auto file =
            std::string("Exports/broken.") + (format == ExportFormat::Wav ? "wav" : "mov");
        const auto finished = f.run(f.addJob(settings, file), "uncompilable " + file);
        expect(finished && finished->state == ExportJobState::Failed && finished->failure,
               file + ": an uncompilable composition fails the job");
        if (!finished || !finished->failure)
            continue;
        const auto& failure = *finished->failure;
        expect(failure.code == host::ExportFailureCode::CompileFailed &&
                   failure.diagnosticCode == "export.compile.failed",
               file + ": the failure is a compile failure");
        const std::string prefix = "The composition could not be compiled for export. "
                                   "Composition \"Main\"";
        expect(failure.reason.starts_with(prefix) &&
                   failure.reason.find("details are in the log") == std::string::npos,
               file + ": the reason names the composition (got \"" + failure.reason + "\")");
        expect(!std::filesystem::exists(f.folder / file), file + ": nothing is published");
    }
}
// An independent BS.1770 reading of a 48 kHz 24-bit WAV (K-weighting, ungated mean square: the
// tone is steady, so the gate changes nothing) and its sample peak, so a loudness report is
// checked against arithmetic that does not share its library.
struct LoudnessReading {
    std::size_t frames = 0;
    double lufs = 0.0;
    double samplePeakDb = 0.0;
};
LoudnessReading measureFile(const std::filesystem::path& path) {
    const auto [channels, samples] = wavSamples(path);
    expect(channels == 2, "the file is stereo");
    if (channels != 2 || samples.empty())
        return {};
    const auto biquad = [](const std::array<double, 3>& b, const std::array<double, 3>& a,
                           std::vector<double> x) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& value : x) {
            const double in = value;
            const double y = b[0] * in + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2;
            x2 = x1, x1 = in, y2 = y1, y1 = y;
            value = y;
        }
        return x;
    };
    double sum = 0.0, peak = 0.0;
    const auto frames = samples.size() / 2;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        std::vector<double> x(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            x[i] = samples[i * 2 + channel];
            peak = std::max(peak, std::abs(x[i]));
        }
        x = biquad({1.53512485958697, -2.69169618940638, 1.19839281085285},
                   {1.0, -1.69065929318241, 0.73248077421585}, std::move(x));
        x = biquad({1.0, -2.0, 1.0}, {1.0, -1.99004745483398, 0.99007225036621}, std::move(x));
        double square = 0.0;
        for (const auto value : x)
            square += value * value;
        sum += square / static_cast<double>(frames);
    }
    return {frames, -0.691 + 10.0 * std::log10(sum), 20.0 * std::log10(peak)};
}

std::optional<host::ExportLoudnessReport>
loudnessReportOf(const std::optional<host::ExportJobView>& view) {
    if (!published(view) || view->outputs.empty() || !view->outputs.front().verification)
        return std::nullopt;
    return view->outputs.front().verification->loudness;
}

// #570: a loudness target on a movie. Each container's audio is normalized like a WAV's: the
// report states the measured and delivered level, and the audio inside the published movie,
// read back through an export of that movie to WAV, measures what the report says. The read-back
// is also the windowed movie audio stream's test (#329): the whole second arrives, from PCM in
// MOV, AAC in MP4 and Opus in WebM, whose millisecond timestamps round every packet.
void testLoudnessThroughVideoContainers() {
    struct Container {
        ExportFormat format;
        std::string profile, extension;
        double tolerance; // of the read-back level; the lossy codecs are not sample exact
    };
    const std::vector<Container> containers{{ExportFormat::ProResMov, "hq", "mov", 0.05},
                                            {ExportFormat::H264Mp4, "high", "mp4", 0.15},
                                            {ExportFormat::Vp9Webm, "8bit", "webm", 0.15}};
    Fixture f("loudness-movies", WavEncoding::Float32);
    for (const auto& container : containers) {
        const auto name = "loudness in " + container.extension;
        document::ExportOutputSettings settings;
        settings.format = container.format;
        settings.codecProfile = container.profile;
        settings.includeAudio = true;
        settings.loudness = document::ExportLoudness{-23.0, -1.0};
        const auto file = "Exports/normalized." + container.extension;
        const auto finished = f.run(f.addJob(settings, file), name);
        if (finished && finished->failure)
            std::cerr << "  " << name << ": " << finished->failure->reason << '\n';
        const auto report = loudnessReportOf(finished);
        expect(report.has_value(), name + ": the movie publishes with a loudness report");
        if (!report)
            continue;
        expect(report->measured && std::abs(report->measuredLufs - -12.4) < 0.6,
               name + ": the tone measures near -12.4 LUFS (got " +
                   std::to_string(report->measuredLufs) + ")");
        expect(report->finalMeasured && std::abs(report->finalLufs - -23.0) < 0.2,
               name + ": the delivered loudness is the target (got " +
                   std::to_string(report->finalLufs) + ")");
        expect(std::abs(report->gainDb - (-23.0 - report->measuredLufs)) < 1e-6,
               name + ": the gain is target minus measured");
        // The movie's own audio, decoded by the import path and written out unchanged.
        Fixture back("loudness-back-" + container.extension, WavEncoding::Float32, true,
                     f.folder / file);
        document::ExportOutputSettings plain;
        plain.format = ExportFormat::Wav;
        plain.codecProfile = "s24";
        plain.includeAudio = true;
        const auto read = back.run(back.addJob(plain, "Exports/back.wav"), name + " read back");
        expect(published(read), name + ": the movie's audio exports to WAV");
        if (!published(read))
            continue;
        const auto measured = measureFile(back.folder / "Exports/back.wav");
        // #329: the sound of the movie's last video frame is its own, not one held sample.
        const auto [backChannels, back48] = wavSamples(back.folder / "Exports/back.wav");
        double low = 1.0, high = -1.0;
        for (std::size_t index = back48.size() >= 4000 ? back48.size() - 4000 : 0;
             index < back48.size(); index += 2) {
            low = std::min(low, back48[index]);
            high = std::max(high, back48[index]);
        }
        expect(backChannels == 2 && back48.size() == 2 * 48000 && high - low > 0.1,
               name + ": the last frame's sound plays to the end (" +
                   std::to_string(back48.size() / 2) + " frames, swing " +
                   std::to_string(high - low) + ")");
        expect(std::abs(measured.lufs - report->finalLufs) < container.tolerance,
               name + ": the audio in the movie measures what the report says (movie " +
                   std::to_string(measured.lufs) + ", report " +
                   std::to_string(report->finalLufs) + ")");
    }
}

// #771: the true-peak limit holds in the delivered file of a lossy codec. AAC in MP4 and Opus in
// WebM are normalized to -14 LUFS with a -1 dBTP limit from a transient-heavy source the limiter
// has to hold down; each published file is decoded independently and measured with the reference
// library (libebur128): its true peak is at or below the limit, and the report and the log give the
// delivered file's own reading. The first trial encode, with the limiter at the limit itself (what
// an export did before #771), measures over it, so the source reproduces the overshoot.
void testTruePeakHoldsInLossyDelivery() {
    struct Lossy {
        ExportFormat format;
        std::string profile, extension;
    };
    const std::vector<Lossy> outputs{{ExportFormat::H264Mp4, "high", "mp4"},
                                     {ExportFormat::Vp9Webm, "8bit", "webm"}};
    Fixture f("true-peak-lossy", WavEncoding::Transients);
    for (const auto& output : outputs) {
        const auto name = "true peak in " + output.extension;
        document::ExportOutputSettings settings;
        settings.format = output.format;
        settings.codecProfile = output.profile;
        settings.includeAudio = true;
        settings.loudness = document::ExportLoudness{-14.0, -1.0};
        const auto file = "Exports/limited." + output.extension;
        const auto finished = f.run(f.addJob(settings, file), name);
        const auto report = loudnessReportOf(finished);
        expect(report.has_value(), name + ": the movie publishes with a loudness report");
        if (!report)
            continue;
        expect(report->finalTruePeakDbtp <= -1.0,
               name + ": the delivered true peak is at or below -1 dBTP (report " +
                   std::to_string(report->finalTruePeakDbtp) + ")");
        // The published file, decoded on its own and measured with the reference library.
        media::video::VideoDecodeSession session(f.folder / file);
        const auto probed = session.probe();
        const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
        expect(probe != nullptr, name + ": the published file probes");
        if (probe == nullptr)
            continue;
        auto decoded = media::video::decodeAudioClip(session, *probe, 4U * 48000U);
        const auto* sound = std::get_if<media::audio::AudioBuffer>(&decoded);
        // The decoder may hand back the codec's last padded frame too; the programme is the first
        // second, as the export's own reopen measured it.
        expect(sound != nullptr && sound->channels == 2 && sound->rate == 48000 &&
                   sound->planes.size() == 2 && sound->planes[0].size() >= 48000,
               name + ": the published file's sound decodes, one second of stereo (" +
                   (sound == nullptr
                        ? std::get<media::provider::Unavailable>(decoded).detail
                        : std::to_string(sound->channels) + " channels at " +
                              std::to_string(sound->rate) + " Hz, " +
                              std::to_string(sound->planes.empty() ? 0 : sound->planes[0].size()) +
                              " frames") +
                   ")");
        if (sound == nullptr || sound->planes.size() != 2 || sound->planes[0].size() < 48000 ||
            sound->planes[1].size() < 48000)
            continue;
        std::vector<float> interleaved(2U * 48000U);
        for (std::size_t i = 0; i < 48000; ++i)
            for (std::size_t c = 0; c < 2; ++c)
                interleaved[i * 2 + c] = sound->planes[c][i];
        media::audio::loudness::Meter meter(48000, 2);
        expect(meter.add(interleaved), name + ": the reference meter takes the sound");
        const auto measured = meter.measurement();
        expect(measured.truePeakDbtp <= -1.0,
               name + ": the published file measures at or below -1 dBTP (" +
                   std::to_string(measured.truePeakDbtp) + ")");
        expect(report->finalMeasured && measured.loudnessMeasured &&
                   std::abs(measured.integratedLufs - report->finalLufs) < 0.05,
               name + ": the report's loudness is the published file's (file " +
                   std::to_string(measured.integratedLufs) + ", report " +
                   std::to_string(report->finalLufs) + ")");
        expect(std::abs(measured.truePeakDbtp - report->finalTruePeakDbtp) < 0.05,
               name + ": the report's true peak is the published file's (file " +
                   std::to_string(measured.truePeakDbtp) + ", report " +
                   std::to_string(report->finalTruePeakDbtp) + ")");
        // The log reports the delivered file's true peak and how many trial encodes it took.
        std::string line;
        for (const auto& entry : f.service->log(finished->id, 0).entries)
            if (entry.code == "export.audio.loudness")
                line = entry.message;
        char peak[32];
        std::snprintf(peak, sizeof peak, "true peak %.2f dBTP", report->finalTruePeakDbtp);
        expect(line.starts_with("Measured on the delivered file: ") &&
                   line.find(peak) != std::string::npos,
               name + ": the log gives the delivered file's true peak (" + line + ")");
        std::cout << "  " << name << ": " << line << '\n';
        const auto trials = line.find(" trial encodes of the sound alone");
        std::size_t start = trials;
        while (start != std::string::npos && start > 0 &&
               std::isdigit(static_cast<unsigned char>(line[start - 1])) != 0)
            --start;
        const auto count = trials == std::string::npos || start == trials
                               ? 0
                               : std::stoul(line.substr(start, trials - start));
        expect(count >= 2, name +
                               ": the first trial at the limit went over it, so the limiter "
                               "was lowered for the encoder (" +
                               line + ")");
    }
}

// #570: a WAV export normalized to a loudness target. The tone source measures about -12.4 LUFS
// (a 0.25-amplitude sine in both channels); the export brings it to the target, the report states
// the measured and the delivered level, and an independent meter reading of the published file
// agrees with the report. A target the true-peak ceiling forbids is limited, and the ceiling holds
// in the file. The file keeps the programme's length.
void testLoudnessNormalization() {
    Fixture f("loudness", WavEncoding::Float32);
    const auto run = [&](const double target, const double ceiling, const std::string& file) {
        document::ExportOutputSettings settings;
        settings.format = ExportFormat::Wav;
        settings.codecProfile = "s24";
        settings.includeAudio = true;
        settings.loudness = document::ExportLoudness{target, ceiling};
        return f.run(f.addJob(settings, "Exports/" + file), file);
    };
    const auto reportOf = loudnessReportOf;

    // Plain normalization to -23 LUFS.
    {
        const auto finished = run(-23.0, -1.0, "quiet.wav");
        const auto report = reportOf(finished);
        expect(report.has_value(), "the normalized export publishes a loudness report");
        if (report) {
            expect(report->measured && report->finalMeasured, "the programme is measurable");
            expect(std::abs(report->measuredLufs - -12.4) < 0.6,
                   "the tone measures near -12.4 LUFS (got " +
                       std::to_string(report->measuredLufs) + ")");
            expect(std::abs(report->finalLufs - -23.0) < 0.2,
                   "the delivered loudness is the target (got " +
                       std::to_string(report->finalLufs) + ")");
            expect(std::abs(report->gainDb - (-23.0 - report->measuredLufs)) < 1e-6,
                   "the gain is target minus measured");
            expect(report->limiterReductionDb < 0.05, "no limiting was needed");
            const auto measured = measureFile(f.folder / "Exports/quiet.wav");
            const auto frames = measured.frames;
            expect(frames == 48000, "the file keeps the programme's 48000 frames (got " +
                                        std::to_string(frames) + ")");
            expect(std::abs(measured.lufs - report->finalLufs) < 0.15,
                   "an independent reading of the file agrees with the report (file " +
                       std::to_string(measured.lufs) + ", report " +
                       std::to_string(report->finalLufs) + ")");
            expect(measured.samplePeakDb <= report->finalTruePeakDbtp + 0.01,
                   "the sample peak does not exceed the reported true peak");
        }
    }
    // A loud target under a low ceiling: the limiter holds the ceiling.
    {
        const auto finished = run(-5.0, -12.0, "limited.wav");
        const auto report = reportOf(finished);
        expect(report.has_value(), "the limited export publishes a loudness report");
        if (report) {
            expect(report->limiterReductionDb > 1.0,
                   "the limiter acted (" + std::to_string(report->limiterReductionDb) + " dB)");
            const auto measured = measureFile(f.folder / "Exports/limited.wav");
            const auto frames = measured.frames;
            expect(frames == 48000, "the limited file keeps 48000 frames");
            expect(measured.samplePeakDb <= -12.0 + 0.1,
                   "the peak ceiling holds in the file (got " +
                       std::to_string(measured.samplePeakDb) + ")");
            expect(report->finalTruePeakDbtp <= -12.0 + 0.1 && measured.lufs <= -5.0 + 0.1,
                   "the limited programme does not exceed the target");
        }
    }
    // Without a target nothing is reported and the audio is as mixed.
    {
        document::ExportOutputSettings settings;
        settings.format = ExportFormat::Wav;
        settings.codecProfile = "s24";
        settings.includeAudio = true;
        const auto finished = f.run(f.addJob(settings, "Exports/plain.wav"), "plain");
        expect(published(finished) && !reportOf(finished).has_value(),
               "an export without a loudness target reports none");
        const auto measured = measureFile(f.folder / "Exports/plain.wav");
        expect(std::abs(measured.lufs - -12.4) < 0.6, "the plain export is unchanged");
    }
}

} // namespace

// ADR 0035: a source stream the platform media framework reads but does not decode is said in the
// export's log, where a user or an agent reads it. The source is a movie with AAC written by the
// macOS encoder: the macOS and Windows frameworks do not declare AAC decode (their samples are not
// the FFmpeg decoder's), so FFmpeg decodes it; Linux has no platform framework and logs nothing.
void testSourceRoutingIsLogged() {
    Fixture f("routing", WavEncoding::Pcm16, true,
              std::filesystem::path(BLOOM_MEDIA_CODEC_FIXTURES) / "apple-h264-aac.mp4");
    document::ExportOutputSettings settings;
    settings.format = ExportFormat::Wav;
    settings.codecProfile = "s24";
    settings.includeAudio = true;
    const auto job = f.addJob(settings, "Exports/routing.wav");
    const auto finished = f.run(job, "AAC source");
    expect(published(finished), "a movie with AAC audio exports its audio");
    std::size_t routed = 0;
    for (const auto& entry : f.service->log(job, 0).entries)
        if (entry.code == "export.source.routing") {
            ++routed;
            std::cout << "  " << entry.code << ": " << entry.message << '\n';
            expect(entry.message.find("apple-h264-aac.mp4") != std::string::npos &&
                       entry.message.find("does not declare aac decode; FFmpeg decodes stream") !=
                           std::string::npos,
                   "the routing entry names the source, the codec and the decoder");
        }
#if defined(__APPLE__) || defined(_WIN32)
    expect(routed == 1, "the export log says once that FFmpeg decodes the AAC stream");
#else
    expect(routed == 0, "without a platform media framework nothing is routed");
#endif
}

// #760: the video route's background. A composition with nothing but audio is transparent; a
// ProRes 4444 movie keeps that alpha (zero) unless the output asks for the composition
// background, when every frame is opaque and uniform, the composition's Background Color. The
// log says so.
void testBackgroundOnTheVideoRoute() {
    Fixture f("background", WavEncoding::Pcm16);
    {
        const auto snapshot = currentSnapshot(f.session);
        const auto* composition = snapshot.project().findComposition(f.composition());
        expect(composition != nullptr, "background: the composition exists");
        if (composition == nullptr)
            return;
        auto settings = commands::compositionSettingsOf(*composition);
        settings.background = core::Color4d{0.8, 0.2, 0.1, 1.0};
        commands::Transaction transaction("Background");
        transaction.emplace<commands::SetCompositionSettings>(f.composition(), settings);
        expect(f.session.execute(std::move(transaction)).changed(),
               "background: the composition's Background Color is set");
    }
    struct Frame final {
        bool ok = false;
        std::uint16_t alphaLow = 0xFFFF, alphaHigh = 0;
        std::uint16_t lumaLow = 0xFFFF, lumaHigh = 0;
    };
    const auto decode = [&](const std::filesystem::path& path, const std::string& name) {
        Frame out;
        media::video::VideoDecodeSession session(path);
        const auto probed = session.probe();
        const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
        expect(probe != nullptr, name + ": the movie probes");
        if (probe == nullptr)
            return out;
        const auto stream = std::ranges::find(probe->streams, media::provider::MediaKind::Video,
                                              &media::provider::StreamDescriptor::kind);
        expect(stream != probe->streams.end(), name + ": the movie has a video stream");
        if (stream == probe->streams.end())
            return out;
        const auto decoded = session.frame(*probe, stream->id, 0, 0, nullptr);
        const auto* frame =
            std::get_if<std::shared_ptr<const media::provider::FrameProduct>>(&decoded);
        expect(frame != nullptr && *frame != nullptr &&
                   (*frame)->format == media::provider::PixelFormat::Yuva444p16 &&
                   (*frame)->planes.size() == 4,
               name + ": frame 0 decodes as 16-bit 4:4:4:4");
        if (frame == nullptr || *frame == nullptr || (*frame)->planes.size() != 4)
            return out;
        const auto scan = [](const media::provider::CpuPlane& plane, std::uint16_t& low,
                             std::uint16_t& high) {
            for (std::uint32_t y = 0; y < plane.height; ++y)
                for (std::uint32_t x = 0; x < plane.width; ++x) {
                    const auto at = static_cast<std::size_t>(y) * plane.stride + x * 2U;
                    std::uint16_t value = 0;
                    std::memcpy(&value, plane.bytes.data() + at, sizeof value);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
        };
        scan((*frame)->planes[3], out.alphaLow, out.alphaHigh);
        scan((*frame)->planes[0], out.lumaLow, out.lumaHigh);
        out.ok = true;
        return out;
    };
    for (const bool background : {false, true}) {
        const auto name = std::string("background ") + (background ? "composition" : "transparent");
        document::ExportOutputSettings settings;
        settings.format = ExportFormat::ProResMov;
        settings.codecProfile = "4444";
        settings.includeAudio = false;
        settings.background = background ? document::ExportBackground::CompositionColor
                                         : document::ExportBackground::Transparent;
        const auto file = std::string("Exports/") + (background ? "opaque" : "clear") + ".mov";
        const auto job = f.addJob(settings, file);
        const auto finished = f.run(job, name);
        expect(published(finished), name + ": the movie publishes");
        if (!published(finished))
            continue;
        bool logged = false;
        for (const auto& entry : f.service->log(job, 0).entries)
            logged = logged || (entry.code == "export.background" &&
                                entry.message.find("Opaque over composition background") == 0);
        expect(logged == background, name + ": the log says opaque over the background only when "
                                            "asked");
        const auto frame = decode(f.folder / file, name);
        if (!frame.ok)
            continue;
        if (background) {
            expect(frame.alphaLow == frame.alphaHigh && frame.alphaLow >= 0xFF00,
                   name + ": every pixel is opaque (alpha " + std::to_string(frame.alphaLow) + ".." +
                       std::to_string(frame.alphaHigh) + ")");
            expect(frame.lumaHigh - frame.lumaLow <= 0x0100 && frame.lumaLow > 0x1000,
                   name + ": the frame is the uniform, non-black background (luma " +
                       std::to_string(frame.lumaLow) + ".." + std::to_string(frame.lumaHigh) + ")");
        } else {
            expect(frame.alphaHigh == 0, name + ": the empty frame keeps zero alpha");
        }
    }
}

// #785: "Use cached frames". A small generated picture (a PNG written here) over the audio layer.
// Every published file is compared byte for byte with the same export rendered with reuse off.
std::uint32_t crc32Of(const std::uint8_t* data, const std::size_t size, std::uint32_t crc = 0) {
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

// An RGB PNG of width x height whose pixels depend on seed, deflated as stored blocks (no zlib).
void writePng(const std::filesystem::path& path, const std::uint32_t width,
              const std::uint32_t height, const std::uint8_t seed) {
    std::vector<std::uint8_t> raw;
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < width; ++x) {
            raw.push_back(static_cast<std::uint8_t>(x * 255 / std::max(1U, width - 1)));
            raw.push_back(static_cast<std::uint8_t>(y * 255 / std::max(1U, height - 1)));
            raw.push_back(static_cast<std::uint8_t>(seed + (x ^ y) * 7));
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const auto length = static_cast<std::uint16_t>(std::min<std::size_t>(65535, raw.size() - at));
        z.push_back(at + length >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(length));
        z.push_back(static_cast<std::uint8_t>(length >> 8));
        z.push_back(static_cast<std::uint8_t>(~length));
        z.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(~length) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                 raw.begin() + static_cast<std::ptrdiff_t>(at + length));
    }
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    const auto adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8)
        z.push_back(static_cast<std::uint8_t>(adler >> shift));
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto chunk = [&png](const char* type, const std::vector<std::uint8_t>& data) {
        const auto size = static_cast<std::uint32_t>(data.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            png.push_back(static_cast<std::uint8_t>(size >> shift));
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        png.insert(png.end(), body.begin(), body.end());
        const auto crc = crc32Of(body.data(), body.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            png.push_back(static_cast<std::uint8_t>(crc >> shift));
    };
    std::vector<std::uint8_t> header;
    for (const auto value : {width, height})
        for (int shift = 24; shift >= 0; shift -= 8)
            header.push_back(static_cast<std::uint8_t>(value >> shift));
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", header);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

std::string fileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The plain stop a changed, moved or deleted source file gets (#790): the export failed, classified
// as an unavailable media source, with a sentence that names the file and says to restore or relink
// it, the same classification in the log, and nothing published.
void expectSourceStop(Fixture& f, const std::optional<host::ExportJobView>& finished,
                      const document::ExportJobId job, const std::string& name,
                      const std::string& file) {
    expect(finished && finished->state == ExportJobState::Failed && finished->failure,
           name + ": the export fails");
    if (!finished || !finished->failure)
        return;
    const auto& failure = *finished->failure;
    expect(failure.diagnosticCode == "export.media.unavailable" &&
               failure.code == host::ExportFailureCode::EvaluationFailed,
           name + ": classified as an unavailable media source (" + failure.diagnosticCode + ")");
    expect(failure.reason.find("\"plate\"") != std::string::npos &&
               failure.reason.find("relink") != std::string::npos &&
               failure.reason.find("Restore") != std::string::npos &&
               failure.reason.find("bug in Bloom") == std::string::npos,
           name + ": the sentence names the file and says to restore or relink it (" +
               failure.reason + ")");
    bool logged = false;
    for (const auto& entry : f.service->log(job, 0).entries)
        logged = logged || (entry.code == "export.media.unavailable" &&
                            entry.message.find("bug in Bloom") == std::string::npos);
    expect(logged, name + ": the export log carries the same classification");
    expect(!std::filesystem::exists(f.folder / file), name + ": nothing published");
}

// #790: a source file that changes or disappears is named, and the export is not called a bug.
void testChangedSourceIsNamed() {
    for (const bool remove : {false, true}) {
        const std::string name = remove ? "source removed" : "source changed";
        Fixture f(remove ? "source-removed" : "source-changed", WavEncoding::Pcm16);
        const auto plate = f.folder / "plate.png";
        writePng(plate, 32, 16, 40);
        {
            commands::Transaction import("Import");
            import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{plate}, f.folder);
            expect(f.session.execute(std::move(import)).changed(), name + ": the picture imports");
            const auto picture = currentSnapshot(f.session).project().assets().back().id;
            commands::Transaction layer("Add picture layer");
            layer.emplace<commands::AddDroppedLayers>(f.composition(),
                                                      std::vector<commands::LayerDropItem>{picture},
                                                      commands::LayerDropPlacement{});
            expect(f.session.execute(std::move(layer)).changed(), name + ": the layer is added");
        }
        document::ExportOutputSettings settings;
        settings.format = ExportFormat::ProResMov;
        settings.codecProfile = "hq";
        settings.includeAudio = false;
        const auto job = f.addJob(settings, "Exports/changed.mov");
        // The picture changes after the job is queued and before its frames are read.
        if (remove)
            std::filesystem::remove(plate);
        else
            writePng(plate, 32, 16, 200);
        const auto finished = f.run(job, name);
        expectSourceStop(f, finished, job, name, "Exports/changed.mov");
    }
}

// #790: a source file that changes or disappears while the export runs stops it the same way. The
// job is paused at a frame boundary (the queue's own Pause), so the file changes after some frames
// were delivered and before any later frame is read, with no timing involved; then it resumes.
void testSourceChangedDuringExport() {
    for (const bool remove : {false, true}) {
        const std::string name = remove ? "source removed mid-export" : "source changed mid-export";
        Fixture f(remove ? "source-removed-mid" : "source-changed-mid", WavEncoding::Pcm16);
        {
            // Long enough that the job is still running when the pause lands.
            commands::CompositionSettingsChange change;
            change.duration = core::RationalTime::fromInteger(20);
            commands::Transaction transaction("Duration");
            transaction.emplace<commands::UpdateCompositionSettings>(f.composition(), change);
            expect(f.session.execute(std::move(transaction)).changed(), name + ": the duration is set");
        }
        const auto plate = f.folder / "plate.png";
        writePng(plate, 32, 16, 40);
        {
            commands::Transaction import("Import");
            import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{plate}, f.folder);
            expect(f.session.execute(std::move(import)).changed(), name + ": the picture imports");
            const auto picture = currentSnapshot(f.session).project().assets().back().id;
            commands::Transaction layer("Add picture layer");
            layer.emplace<commands::AddDroppedLayers>(f.composition(),
                                                      std::vector<commands::LayerDropItem>{picture},
                                                      commands::LayerDropPlacement{});
            expect(f.session.execute(std::move(layer)).changed(), name + ": the layer is added");
        }
        document::ExportOutputSettings settings;
        settings.format = ExportFormat::ProResMov;
        settings.codecProfile = "hq";
        settings.includeAudio = false;
        const auto job = f.addJob(settings, "Exports/changed-mid.mov");
        expect(f.pumpUntil([&] { return f.service->canStart(job).available; }, 20),
               name + ": the job becomes startable");
        const document::ExportJobId ids[] = {job};
        expect(f.service->start(ids).accepted, name + ": start");
        // Frames are delivered; pause at the next frame boundary.
        expect(f.pumpUntil(
                   [&] {
                       const auto now = f.view(job);
                       return !now || host::isTerminal(now->state) ||
                              now->progress.framesCompleted >= 2;
                   },
                   60),
               name + ": frames are delivered");
        expect(f.service->pause(job).accepted, name + ": pause is accepted");
        expect(f.pumpUntil(
                   [&] {
                       const auto now = f.view(job);
                       return !now || now->state == ExportJobState::Paused ||
                              host::isTerminal(now->state);
                   },
                   60),
               name + ": the job pauses");
        const auto paused = f.view(job);
        expect(paused && paused->state == ExportJobState::Paused &&
                   paused->progress.framesCompleted >= 2 &&
                   paused->progress.framesCompleted < paused->progress.framesTotal,
               name + ": the export is paused part way through");
        if (!paused || paused->state != ExportJobState::Paused) {
            continue;
        }
        // Nothing is in flight now: the file changes between two frames.
        if (remove)
            std::filesystem::remove(plate);
        else
            writePng(plate, 32, 16, 200);
        expect(f.service->resume(job).accepted, name + ": resume is accepted");
        expect(f.pumpUntil([&] { return host::isTerminal(f.view(job)->state); }, 120),
               name + ": the job finishes");
        const auto finished = f.view(job);
        expectSourceStop(f, finished, job, name, "Exports/changed-mid.mov");
    }
}

void testCachedFramesAreReused() {
    Fixture f("frame-reuse", WavEncoding::Pcm16);
    const auto plate = f.folder / "plate.png";
    // Smaller than the 64 x 32 composition, so the background shows around it.
    writePng(plate, 32, 16, 40);
    {
        commands::Transaction import("Import");
        import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{plate}, f.folder);
        expect(f.session.execute(std::move(import)).changed(), "reuse: the picture imports");
        const auto picture = currentSnapshot(f.session).project().assets().back().id;
        commands::Transaction layer("Add picture layer");
        layer.emplace<commands::AddDroppedLayers>(f.composition(),
                                                  std::vector<commands::LayerDropItem>{picture},
                                                  commands::LayerDropPlacement{});
        expect(f.session.execute(std::move(layer)).changed(), "reuse: the picture layer is added");
    }
    {
        // A colour the #760 export background shows around the picture.
        commands::CompositionSettingsChange change;
        change.background = core::Color4d{0.8, 0.2, 0.1, 1.0};
        commands::Transaction transaction("Background");
        transaction.emplace<commands::UpdateCompositionSettings>(f.composition(), change);
        expect(f.session.execute(std::move(transaction)).changed(),
               "reuse: the composition's Background Color is set");
    }
    const auto cache = f.gpu->exportFrameCache();
    expect(cache != nullptr, "reuse: the provider has an export frame cache");
    if (cache == nullptr)
        return;
    const auto settingsOf = [](const bool reuse) {
        document::ExportOutputSettings settings;
        settings.format = ExportFormat::ProResMov;
        settings.codecProfile = "hq";
        settings.includeAudio = true;
        settings.useCachedFrames = reuse;
        return settings;
    };
    // Adds one job with these outputs (settings and file), runs it and returns its finished view.
    const auto runJob = [&f](const std::vector<std::pair<document::ExportOutputSettings, std::string>>& outputs,
                             const std::string& name) {
        document::ExportJob job;
        job.composition = f.composition();
        job.render.range = {document::ExportRangeKind::Composition, 0, 0};
        for (const auto& [settings, path] : outputs) {
            document::ExportOutput output;
            output.presetId = "test";
            output.settings = settings;
            output.destination = {"project-relative", path, document::ExportOverwritePolicy::Replace};
            job.outputs.push_back(output);
        }
        commands::Transaction transaction("Add export job");
        transaction.emplace<commands::AddExportJob>(job);
        const auto result = f.session.execute(std::move(transaction));
        if (!result.changed())
            throw std::runtime_error("add job");
        const auto id = *result.command->outputId<document::ExportJobId>(commands::kExportJobOutput);
        return std::make_pair(id, f.run(id, name));
    };
    const auto reused = [](const std::optional<host::ExportJobView>& view, const std::size_t output) {
        if (!view || output >= view->outputs.size() || !view->outputs[output].verification)
            return std::uint64_t{~0ULL};
        return view->outputs[output].verification->provenance.framesReused;
    };
    const auto frames = [](const std::optional<host::ExportJobView>& view, const std::size_t output) {
        if (!view || output >= view->outputs.size() || !view->outputs[output].verification)
            return std::uint64_t{0};
        const auto& p = view->outputs[output].verification->provenance;
        return p.gpuFrames + p.cpuReferenceFrames + p.framesReused;
    };

    // Reuse off: the reference file, and nothing enters the cache.
    const auto [offJob, off] = runJob({{settingsOf(false), "Exports/off.mov"}}, "reuse off");
    expect(published(off), "reuse off: the movie publishes");
    expect(reused(off, 0) == 0 && cache->stats().entries == 0,
           "reuse off: no frame is reused or retained");
    const auto reference = fileBytes(f.folder / "Exports/off.mov");
    const auto total = frames(off, 0);
    expect(total > 1 && !reference.empty(), "reuse off: the reference movie has frames");

    // Two outputs of one job: the first evaluates every frame, the second reuses every one.
    const auto [twoJob, two] =
        runJob({{settingsOf(true), "Exports/first.mov"}, {settingsOf(true), "Exports/second.mov"}},
               "two outputs");
    expect(published(two), "two outputs: the job publishes");
    expect(reused(two, 0) == 0 && frames(two, 0) == total,
           "two outputs: the first output evaluates every frame (" + std::to_string(reused(two, 0)) +
               " reused)");
    expect(reused(two, 1) == total,
           "two outputs: the second output reuses every frame (" + std::to_string(reused(two, 1)) +
               " of " + std::to_string(total) + ")");
    expect(
        fileBytes(f.folder / "Exports/first.mov") == reference &&
            fileBytes(f.folder / "Exports/second.mov") == reference,
        "two outputs: both movies are byte-identical to the reuse-off export (first: " +
            test_support::movieDifference(fileBytes(f.folder / "Exports/first.mov"), reference) +
            "; second: " +
            test_support::movieDifference(fileBytes(f.folder / "Exports/second.mov"), reference) +
            ")");

    // The same export again: every frame is reused, the file is the same.
    const auto [againJob, again] = runJob({{settingsOf(true), "Exports/again.mov"}}, "repeated");
    {
        const auto stats = cache->stats();
        expect(published(again) && reused(again, 0) == total,
               "repeated: a repeated export reuses every frame (" + std::to_string(reused(again, 0)) +
                   "; cache entries " + std::to_string(stats.entries) + " hits " +
                   std::to_string(stats.hits) + " misses " + std::to_string(stats.misses) +
                   " insertions " + std::to_string(stats.insertions) + " refusals " +
                   std::to_string(stats.refusals) + " evictions " + std::to_string(stats.evictions) +
                   ")");
    }
    expect(fileBytes(f.folder / "Exports/again.mov") == reference,
           "repeated: the movie is byte-identical to the reuse-off export (" +
               test_support::movieDifference(fileBytes(f.folder / "Exports/again.mov"), reference) +
               ")");

    // A setting that changes the frame reuses nothing: the #760 export background.
    {
        auto opaque = settingsOf(true);
        opaque.background = document::ExportBackground::CompositionColor;
        auto opaqueOff = opaque;
        opaqueOff.useCachedFrames = false;
        const auto [bgJob, bg] = runJob({{opaque, "Exports/opaque.mov"}}, "background");
        expect(published(bg) && reused(bg, 0) == 0,
               "background: an export over the composition background reuses nothing (" +
                   std::to_string(reused(bg, 0)) + ")");
        const auto [bgOffJob, bgOff] = runJob({{opaqueOff, "Exports/opaque-off.mov"}}, "background off");
        const auto opaqueBytes = fileBytes(f.folder / "Exports/opaque.mov");
        expect(published(bgOff) && opaqueBytes == fileBytes(f.folder / "Exports/opaque-off.mov"),
               "background: the movie is byte-identical to the same export with reuse off (" +
                   test_support::movieDifference(opaqueBytes,
                                                 fileBytes(f.folder / "Exports/opaque-off.mov")) +
                   ")");
        expect(opaqueBytes != reference,
               "background: the movie is its own render over the background, not the cached "
               "frames without it");
    }

    // The picture's input colour changes the frame: nothing is reused; set back, the cached
    // frames are reused again.
    {
        const auto snapshot = currentSnapshot(f.session);
        const auto asset = std::ranges::find_if(snapshot.project().assets(), [](const auto& record) {
            return record.locator.path.ends_with("plate.png");
        });
        expect(asset != snapshot.project().assets().end(), "input colour: the picture asset exists");
        if (asset != snapshot.project().assets().end()) {
            const auto id = asset->id;
            const auto legacy = asset->interpretation.colorSpace;
            const auto exact = asset->interpretation.inputColorSpaceId;
            commands::Transaction raw("Raw input colour");
            raw.emplace<commands::SetAssetInputColor>(id, document::AssetColorSpace::Raw, "");
            expect(f.session.execute(std::move(raw)).changed(), "input colour: set to Raw");
            const auto [rawJob, rawView] = runJob({{settingsOf(true), "Exports/raw.mov"}}, "raw");
            const auto [rawOffJob, rawOff] = runJob({{settingsOf(false), "Exports/raw-off.mov"}}, "raw off");
            expect(published(rawView) && reused(rawView, 0) == 0 &&
                       fileBytes(f.folder / "Exports/raw.mov") ==
                           fileBytes(f.folder / "Exports/raw-off.mov") &&
                       fileBytes(f.folder / "Exports/raw.mov") != reference,
                   "input colour: another input colour reuses nothing and renders fresh (" +
                       std::to_string(reused(rawView, 0)) + ")");
            commands::Transaction back("Input colour back");
            back.emplace<commands::SetAssetInputColor>(id, legacy, exact);
            expect(f.session.execute(std::move(back)).changed(), "input colour: set back");
            const auto [backJob, backView] = runJob({{settingsOf(true), "Exports/back.mov"}}, "back");
            expect(published(backView) && reused(backView, 0) == total &&
                       fileBytes(f.folder / "Exports/back.mov") == reference,
                   "input colour: set back, the cached frames are reused, byte-identical (" +
                       std::to_string(reused(backView, 0)) + ")");
        }
    }

    // The picture changes on disk: a cached frame of the old picture is never delivered. The
    // export fails as a fresh render does (the file is not the content the project recorded).
    writePng(plate, 32, 16, 200);
    {
        const auto [staleJob, stale] = runJob({{settingsOf(true), "Exports/stale.mov"}}, "stale");
        expect(!published(stale) && reused(stale, 0) == 0 &&
                   !std::filesystem::exists(f.folder / "Exports/stale.mov"),
               "stale media: no cached frame of the old picture is published");
        const auto snapshot = currentSnapshot(f.session);
        const auto asset = std::ranges::find_if(snapshot.project().assets(), [](const auto& record) {
            return record.locator.path.ends_with("plate.png");
        });
        expect(asset != snapshot.project().assets().end(), "stale media: the picture asset exists");
        if (asset != snapshot.project().assets().end()) {
            commands::Transaction relink("Relink");
            relink.emplace<commands::RelinkAsset>(asset->id, plate, f.folder);
            expect(f.session.execute(std::move(relink)).changed(), "stale media: the picture relinks");
            const auto [newJob, renewed] = runJob({{settingsOf(true), "Exports/new.mov"}}, "relinked");
            auto newOff = settingsOf(false);
            const auto [newOffJob, renewedOff] = runJob({{newOff, "Exports/new-off.mov"}}, "relinked off");
            expect(published(renewed) && reused(renewed, 0) == 0 &&
                       fileBytes(f.folder / "Exports/new.mov") ==
                           fileBytes(f.folder / "Exports/new-off.mov") &&
                       fileBytes(f.folder / "Exports/new.mov") != reference,
                   "stale media: the new picture renders fresh and reuses nothing");
        }
    }

    // The retained frames are charged to the processing root and go back to it when work needs
    // the memory: the root's reclaimers (the cache's among them) give them back.
    {
        const auto policy = f.gpu->processingPhasePolicy();
        const auto held = cache->stats();
        const auto before = policy->usableCapacity().hostClaimBytes;
        expect(held.entries > 0 && held.bytes > 0 && before >= held.bytes,
               "memory: the retained frames are claims on the processing root (" +
                   std::to_string(held.bytes) + " of " + std::to_string(before) + " bytes)");
        policy->reclaimFor(render::GpuProcessingPhaseDomain::Host,
                           policy->usableCapacity().hostUsableBytes + held.bytes);
        // The jobs' own frame attempts retire on their worker; once they have, the root holds none
        // of the frames the cache gave back.
        (void)f.pumpUntil(
            [&] { return before - policy->usableCapacity().hostClaimBytes >= held.bytes; }, 10);
        const auto after = policy->usableCapacity().hostClaimBytes;
        expect(cache->stats().entries == 0 && before - after >= held.bytes,
               "memory: a host wait on the root releases every retained frame (" +
                   std::to_string(before) + " -> " + std::to_string(after) + " bytes)");
    }

    // An export that reuses frames stays stoppable: a long range (its first second cached) stops.
    {
        const auto [warmJob, warm] = runJob({{settingsOf(true), "Exports/warm.mov"}}, "warm");
        expect(published(warm), "stop: the cache is warm again");
        commands::CompositionSettingsChange longer;
        longer.duration = core::RationalTime::fromInteger(20);
        commands::Transaction transaction("Longer");
        transaction.emplace<commands::UpdateCompositionSettings>(f.composition(), longer);
        expect(f.session.execute(std::move(transaction)).changed(), "stop: the composition is longer");
        document::ExportJob job;
        job.composition = f.composition();
        job.render.range = {document::ExportRangeKind::Composition, 0, 0};
        document::ExportOutput output;
        output.presetId = "test";
        output.settings = settingsOf(true);
        output.destination = {"project-relative", "Exports/stopped.mov",
                              document::ExportOverwritePolicy::Replace};
        job.outputs = {output};
        commands::Transaction add("Add export job");
        add.emplace<commands::AddExportJob>(job);
        const auto added = f.session.execute(std::move(add));
        const auto id = *added.command->outputId<document::ExportJobId>(commands::kExportJobOutput);
        expect(f.pumpUntil([&] { return f.service->canStart(id).available; }, 20),
               "stop: the job becomes startable");
        const document::ExportJobId ids[] = {id};
        expect(f.service->start(ids).accepted, "stop: start");
        expect(f.pumpUntil([&] { return f.view(id)->state == ExportJobState::Rendering; }, 20),
               "stop: the job renders");
        (void)f.service->stop(id);
        expect(f.pumpUntil([&] { return host::isTerminal(f.view(id)->state); }, 60),
               "stop: the job ends after stop");
        expect(f.view(id)->state == ExportJobState::Stopped &&
                   !std::filesystem::exists(f.folder / "Exports/stopped.mov"),
               "stop: the job is stopped and publishes nothing");
    }
}

int main(const int argc, char** argv) {
    // #760: the video route's background (bloom.host.export-background-video).
    if (argc > 1 && std::string_view(argv[1]) == "--background") {
        try {
            testBackgroundOnTheVideoRoute();
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
        if (failures == 0)
            std::cout << "export background video: ok\n";
        return failures == 0 ? 0 : 1;
    }
    // #790: a changed or removed source file is named (bloom.host.export-changed-source).
    if (argc > 1 && std::string_view(argv[1]) == "--changed-source") {
        try {
            testChangedSourceIsNamed();
            testSourceChangedDuringExport();
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
        if (failures == 0)
            std::cout << "export changed source: ok\n";
        return failures == 0 ? 0 : 1;
    }
    // #785: frames reused from the export frame cache (bloom.host.export-frame-reuse).
    if (argc > 1 && std::string_view(argv[1]) == "--frame-reuse") {
        try {
            testCachedFramesAreReused();
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
        if (failures == 0)
            std::cout << "export frame reuse: ok\n";
        return failures == 0 ? 0 : 1;
    }
    // #761: the per-preset stream walk is its own test (bloom.host.export-written-streams).
    if (argc > 1 && std::string_view(argv[1]) == "--written-streams") {
        std::size_t unavailable = 0;
        try {
            unavailable = testWrittenStreamsForEveryPreset();
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
        if (failures != 0)
            return 1;
        if (unavailable != 0) {
            std::cout << "export written streams: " << unavailable
                      << " built-in presets cannot be exported here; skipped\n";
            return 77;
        }
        std::cout << "export written streams: ok\n";
        return 0;
    }
    try {
        testSourceRoutingIsLogged();
        testSourceFormatsAcrossPresets();
        testUndecodableSourceIsRefusedBeforeRendering();
        testSurroundSourceDownmixesToStereo();
        testLevelAutomationRampsWithinBlocks();
        testEffectChainExportsAsPlayback();
        testFailedRunKeepsFrameProvenance();
        testCompileFailureNamesItsSubject();
        testLoudnessNormalization();
        testLoudnessThroughVideoContainers();
        testTruePeakHoldsInLossyDelivery();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "export audio formats: ok\n";
    return failures == 0 ? 0 : 1;
}

// Issue #428: export audio source formats end to end through the export queue and the media
// worker. 16-bit, 24-bit and 32-bit float WAV sources export through every audio-carrying preset
// family, PCM containers keep 24 bits for a deeper source, and a source the export audio path
// cannot decode is refused by the source-aware audio preflight before anything renders, with a
// sentence naming the stage and the input. Issue #424: a failed run keeps its frames' route
// provenance instead of counting every frame as CPU reference. Issue #450: a 5.1 source exports
// to stereo through the ITU-R BS.775 downmix instead of losing every channel past the second.
// Issue #459: an automated level exports as a continuous ramp, not a step every mix block.
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/export_audio_sources.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
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
    auto path = std::filesystem::temp_directory_path() /
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
enum class WavEncoding { Pcm16, Pcm24, Float32, ImaAdpcm, Surround51, ConstantStereo };
constexpr float kConstantLevel = 0.5F;
constexpr std::array<float, 6> kSurroundLevels{0.1F, 0.2F, 0.3F, 0.4F, 0.05F, 0.15F};

void put(std::vector<char>& out, const std::uint64_t value, const int bytes) {
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
}

// One second of a 440 Hz stereo tone at 48 kHz, or of constant 5.1 levels for Surround51.
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
        const bool floating =
            encoding == WavEncoding::Float32 || encoding == WavEncoding::ConstantStereo;
        format = floating ? 3 : 1;
        bits = encoding == WavEncoding::Pcm16 ? 16 : encoding == WavEncoding::Pcm24 ? 24 : 32;
        blockAlign = static_cast<std::uint16_t>(channels * bits / 8);
        for (std::uint32_t frame = 0; frame < rate; ++frame) {
            const double value =
                encoding == WavEncoding::ConstantStereo
                    ? double{kConstantLevel}
                    : 0.25 * std::sin(2.0 * 3.141592653589793 * 440.0 * frame / double(rate));
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
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
                                            const std::filesystem::path& folder) {
        auto format = document::CompositionFormat::create(64, 32);
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
    Fixture(const std::string& name, const WavEncoding source, const bool compilable = true)
        : folder(scratch(name)), session(makeSession(identity, folder)) {
        if (compilable && !runtime::registerBuiltInNodeDefinitions(registry))
            throw std::runtime_error("registry");
        registry.freeze();
        runtime::GpuProcessFrameEvaluatorOptions options;
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
        const auto wav = writeWav(folder / "tone.wav", source);
        commands::Transaction import("Import");
        import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{wav}, folder);
        if (!session.execute(std::move(import)).changed())
            throw std::runtime_error("import the WAV source");
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
    expect(std::abs(samples[12000 * 2] - 0.5 * double{kConstantLevel}) < 1e-3,
           "level ramp: a quarter second in, the level is halfway (got " +
               std::to_string(samples[12000 * 2]) + ")");
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
        expect(std::ranges::any_of(output.preflight,
                                   [&](const auto& issue) {
                                       return issue.code ==
                                                  host::ExportPreflightCode::
                                                      AudioSourceUnsupported &&
                                              issue.target == host::ExportPreflightTarget::Audio &&
                                              issue.severity ==
                                                  host::ExportPreflightSeverity::Blocking &&
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
                  .lastFrame = static_cast<std::uint64_t>(duration.numerator() * rate.numerator() /
                                                          (duration.denominator() *
                                                           rate.denominator())) -
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

} // namespace

int main() {
    try {
        testSourceFormatsAcrossPresets();
        testUndecodableSourceIsRefusedBeforeRendering();
        testSurroundSourceDownmixesToStereo();
        testLevelAutomationRampsWithinBlocks();
        testFailedRunKeepsFrameProvenance();
        testCompileFailureNamesItsSubject();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "export audio formats: ok\n";
    return failures == 0 ? 0 : 1;
}

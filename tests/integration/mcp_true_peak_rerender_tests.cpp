// #771: the stdio server's own movie route (`export` with a ProRes, DNxHR or WAV preset, run directly
// by host::SequenceExportRunnerV1 rather than an export queue job) keeps the true-peak limit in the
// delivered file. The runner refuses to publish a file whose sound, decoded back, peaks over the
// limit; the route then renders the export again with the limiter lower by the overshoot and a
// margin, at most three more times, as the export queue does. Those presets carry PCM sound, which
// no encoder changes after the limiter, so the test seam switches the route's request to Opus in
// WebM with the trial encodes off (an encoder the trials do not model): the first render is refused
// over the limit, and the published file measures at or below it with the reference library.

#include "fixed_memory_budget.hpp"
#include "unique_temp_dir.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/media/audio/loudness.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/session.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <yyjson.h>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

// One second of 48 kHz stereo float: a quiet bed of tone and noise with a loud noise burst every
// 125 ms, dense peaks the limiter holds down hard.
void writeBursts(const std::filesystem::path& path) {
    const auto noise = [](const std::uint32_t at, const std::uint32_t lane) {
        std::uint32_t state = at * 2654435761U + (lane + 1U) * 40503U;
        state ^= state >> 15U;
        state *= 2246822519U;
        state ^= state >> 13U;
        return static_cast<double>(state % 20001U) / 10000.0 - 1.0;
    };
    std::vector<char> wav;
    const auto text = [&](const std::string_view value) { wav.insert(wav.end(), value.begin(), value.end()); };
    const auto number = [&](const std::uint64_t value, const unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            wav.push_back(static_cast<char>((value >> (8U * i)) & 255U));
    };
    constexpr std::uint32_t frames = 48000;
    constexpr double pi = 3.141592653589793;
    text("RIFF");
    number(36 + frames * 8U, 4);
    text("WAVEfmt ");
    number(16, 4);
    number(3, 2); // WAVE_FORMAT_IEEE_FLOAT
    number(2, 2);
    number(48000, 4);
    number(48000 * 8, 4);
    number(8, 2);
    number(32, 2);
    text("data");
    number(frames * 8U, 4);
    for (std::uint32_t frame = 0; frame < frames; ++frame)
        for (std::uint32_t channel = 0; channel < 2; ++channel) {
            const double t = static_cast<double>(frame) / 48000.0;
            const auto value = static_cast<float>(
                0.06 * std::sin(2.0 * pi * (220.0 + 110.0 * channel) * t) +
                0.05 * noise(frame, channel) +
                0.9 * std::exp(-static_cast<double>(frame % 6000U) / 384.0) *
                    noise(frame, channel + 2U));
            std::uint32_t raw = 0;
            std::memcpy(&raw, &value, sizeof raw);
            number(raw, 4);
        }
    std::ofstream out(path, std::ios::binary);
    out.write(wav.data(), static_cast<std::streamsize>(wav.size()));
}

struct Headless final {
    std::unique_ptr<scripting::Session> session = [] {
        auto format = document::CompositionFormat::create(64, 32);
        auto created = scripting::Session::createNew("True peak", "Main",
                                                     core::RationalTime::fromInteger(1), *format);
        if (!created)
            throw std::runtime_error("session");
        return std::move(created).takeSession();
    }();
    runtime::TaskScheduler scheduler;
    std::shared_ptr<host::GpuExportProvider> provider = [] {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
        options.enabled = false;
        return host::GpuExportProvider::create(std::move(options));
    }();
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    int nextId = 1;

    Headless(std::filesystem::path captures,
             std::function<void(host::SequenceExportRequestV1&)> seam) {
        provider->prepare(scheduler);
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(*session),
            // The stdio host: no export queue, so the ProRes preset runs on the direct route.
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = true},
            .scheduler = scheduler,
            .gpuProvider = provider,
            .captureDirectory = std::move(captures),
            .serverVersion = "test",
            .limits = mcp::kMcpMessageLimits,
            .sequenceExportSeam = std::move(seam)});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"core","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    ~Headless() {
        core.reset();
        scheduler.beginShutdown();
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
    }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;

    std::string send(const std::string& message) {
        const mcp::McpRequestControl control{};
        return core->handle(*protocol, message, control).response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
};

// The number after the first `"name":` of a response.
double numberAfter(const std::string& response, const std::string& name) {
    const auto at = response.find("\"" + name + "\":");
    if (at == std::string::npos)
        return std::nan("");
    return std::stod(response.substr(at + name.size() + 3));
}

void deliveredTruePeakRendersAgain() {
    const auto folder =
        bloom::test_support::processTempDirectory() /
        ("bloom-mcp-true-peak-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Remove final {
        std::filesystem::path folder;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    } remove{folder};
    std::vector<double> headroom;
    Headless server(folder / "captures", [&](host::SequenceExportRequestV1& request) {
        request.preset = output::OutputPresetV1::Vp9WebmV1;
        request.profile = "8bit";
        request.pcmCodec = "opus";
        if (request.loudness) {
            request.loudness->trialEncodes = false;
            headroom.push_back(request.loudness->encoderHeadroomDb);
        }
    });
    const auto wav = folder / "bursts.wav";
    writeBursts(wav);
    auto* host = server.session->hostSession();
    expect(host != nullptr, "the session has a project");
    if (host == nullptr)
        return;
    const auto project = [host] {
        const auto decoded = host->decodedSnapshot();
        return decoded.snapshot();
    };
    const auto composition = project().project().compositions().front().id();
    {
        commands::Transaction import("Import");
        import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{wav}, folder);
        expect(server.session->execute(std::move(import)).command.has_value(), "the WAV imports");
        const auto asset = project().project().assets().back().id;
        commands::Transaction layer("Add audio layer");
        layer.emplace<commands::AddDroppedLayers>(composition,
                                                  std::vector<commands::LayerDropItem>{asset},
                                                  commands::LayerDropPlacement{});
        expect(server.session->execute(std::move(layer)).command.has_value(),
               "the audio layer is added");
    }
    const auto movie = folder / "limited.mov";
    const auto response = server.call(
        "export", R"({"composition":)" + std::to_string(composition.value()) +
                      R"(,"preset":"ProResMovV1","destination":")" + movie.generic_string() +
                      R"(","loudnessLufs":-8,"truePeakDbtp":-1,"overwrite":"replace"})");
    std::cout << "  export: " << response.substr(0, 1200) << '\n';
    expect(contains(response, R"("isError":false)"), "the export publishes: " + response);
    expect(headroom.size() >= 2 && headroom.front() == 0.0 && headroom.back() > 0.0,
           "the first render is refused over the limit and the export renders again with the "
           "limiter lower (" +
               std::to_string(headroom.size()) + " renders)");
    expect(headroom.size() <= 4, "at most three more renders");
    expect(contains(response, "so Bloom rendered the export again with the limiter"),
           "the preservation report says the export rendered again: " + response);
    const auto reported = numberAfter(response, "finalTruePeakDbtp");
    expect(reported <= -1.0, "the reported delivered true peak is at or below -1 dBTP (" +
                                 std::to_string(reported) + ")");
    // The published file, decoded on its own and measured with the reference library.
    media::video::VideoDecodeSession session(movie);
    const auto probed = session.probe();
    const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
    expect(probe != nullptr, "the published file probes");
    if (probe == nullptr)
        return;
    auto decoded = media::video::decodeAudioClip(session, *probe, 4U * 48000U);
    const auto* sound = std::get_if<media::audio::AudioBuffer>(&decoded);
    expect(sound != nullptr && sound->planes.size() == 2 && sound->planes[0].size() >= 48000 &&
               sound->planes[1].size() >= 48000,
           "the published file's sound decodes, one second of stereo");
    if (sound == nullptr || sound->planes.size() != 2 || sound->planes[0].size() < 48000 ||
        sound->planes[1].size() < 48000)
        return;
    std::vector<float> interleaved(2U * 48000U);
    for (std::size_t i = 0; i < 48000; ++i)
        for (std::size_t c = 0; c < 2; ++c)
            interleaved[i * 2 + c] = sound->planes[c][i];
    media::audio::loudness::Meter meter(48000, 2);
    expect(meter.add(interleaved), "the reference meter takes the sound");
    const auto measured = meter.measurement();
    expect(measured.truePeakDbtp <= -1.0, "the published file measures at or below -1 dBTP (" +
                                              std::to_string(measured.truePeakDbtp) + ")");
    expect(std::abs(measured.truePeakDbtp - reported) < 0.05,
           "the reported true peak is the published file's (file " +
               std::to_string(measured.truePeakDbtp) + ", report " + std::to_string(reported) +
               ")");
}

} // namespace

int main() {
    try {
        deliveredTruePeakRendersAgain();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "mcp true peak render again: ok\n";
    return failures == 0 ? 0 : 1;
}

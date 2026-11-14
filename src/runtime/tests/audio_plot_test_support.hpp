#ifndef BLOOM_RUNTIME_TESTS_AUDIO_PLOT_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_AUDIO_PLOT_TEST_SUPPORT_HPP

// #347: plans that run Audio Waveform and Audio Spectrum the way the snapshot compiler lowers them (a
// transparent canvas followed by one AudioPlotKernel effect) over a generated WAV file, so the CPU
// reference tests, the GPU coverage fixtures and the native parity harness exercise what an authored
// node evaluates. The file is written once under the scratch directory and probed by its content
// digest, as an imported asset is.

#include <bloom/core/file_uri.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/runtime/audio_analysis_source.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <chrono>
#include <random>
#include <system_error>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace bloom::audio_plot_test {

using document::ParameterId;
using runtime::CompiledColorParameter;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kPlotComposition = 64;
inline constexpr std::uint32_t kSampleRate = 48'000;

inline void putU32(std::ofstream& out, const std::uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
                                    static_cast<char>((value >> 16U) & 0xFFU),
                                    static_cast<char>((value >> 24U) & 0xFFU)};
    out.write(bytes.data(), 4);
}
inline void putU16(std::ofstream& out, const std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
    out.write(bytes.data(), 2);
}

// Left: a sine centred exactly on bin 40 of a 2048-point transform (937.5 Hz) at amplitude 0.5.
// Right: a constant 0.25. The left sine stops at one second, so another second reads another picture. Two seconds of 32-bit float stereo.
inline void writeFixtureWav(const std::filesystem::path& path, const double leftAmplitude = 0.5) {
    constexpr std::uint32_t frames = kSampleRate * 2;
    const std::uint32_t dataBytes = frames * 2U * 4U;
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    putU32(out, 36U + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    putU32(out, 16);
    putU16(out, 3);
    putU16(out, 2);
    putU32(out, kSampleRate);
    putU32(out, kSampleRate * 8U);
    putU16(out, 8);
    putU16(out, 32);
    out.write("data", 4);
    putU32(out, dataBytes);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * std::numbers::pi * 937.5 * static_cast<double>(frame) / kSampleRate;
        const double left = frame < kSampleRate ? std::sin(phase) * leftAmplitude : 0.0;
        putU32(out, std::bit_cast<std::uint32_t>(static_cast<float>(left)));
        putU32(out, std::bit_cast<std::uint32_t>(0.25F));
    }
}

struct AudioFixture final {
    std::filesystem::path directory;
    document::AssetRecord asset;
};

[[nodiscard]] inline AudioFixture makeAudioFixture(const std::filesystem::path& directory,
                                                   const std::string& name, const double leftAmplitude = 0.5) {
    std::filesystem::create_directories(directory);
    AudioFixture fixture;
    fixture.directory = directory;
    const auto path = directory / name;
    writeFixtureWav(path, leftAmplitude);
    const auto hash = platform::hashSourceFile(path);
    if (hash.error != platform::SourceHashError::None)
        throw std::logic_error("the audio fixture could not be hashed");
    fixture.asset.id = document::AssetId::fromRaw(947);
    fixture.asset.kind = document::AssetKind::Audio;
    fixture.asset.locator = {"file", "project-relative", name,
                             bloom::core::fileUriFromAbsolutePath(std::filesystem::absolute(path))
                                 .value_or(std::string{})};
    fixture.asset.contentDigest = hash.digest;
    fixture.asset.rate = kSampleRate;
    fixture.asset.channels = 2;
    fixture.asset.frames = std::uint64_t{kSampleRate} * 2U;
    fixture.asset.duration = core::RationalTime::fromInteger(2);
    fixture.asset.name = name;
    return fixture;
}

// One fixture file per test process, in a directory of its own that goes with the process. Five
// test programs use this fixture and run side by side; with one fixed path, one of them rewrote
// the file while another was reading it ("Audio file could not be read").
[[nodiscard]] inline const AudioFixture& sharedAudioFixture() {
    struct Owned final {
        AudioFixture fixture;
        Owned() {
            std::random_device device;
            const auto name =
                "bloom-audio-plot-" + std::to_string(device()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            fixture =
                makeAudioFixture(std::filesystem::temp_directory_path() / name, "plot_fixture.wav");
        }
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
        ~Owned() {
            std::error_code ignored;
            std::filesystem::remove_all(fixture.directory, ignored);
        }
    };
    static const Owned owned;
    return owned.fixture;
}

struct AudioPlotValues final {
    runtime::AudioPlotKind kind = runtime::AudioPlotKind::Waveform;
    std::int64_t width = 48;
    std::int64_t height = 32;
    bool separate = false;
    std::int64_t fftSize = 2048;
    bool logarithmic = false;
    // Waveform: window seconds, scale. Spectrum: low and high hertz, floor and ceiling decibels.
    std::array<double, 4> values{0.5, 1.0, 0.0, 0.0};
    core::Color4d color{1.0, 0.5, 0.25, 1.0};
    core::Color4d background{0.0, 0.0, 0.1, 0.5};
    // The source second the frame at time zero reads: the start frame is -24 * seconds at 24 fps.
    std::int64_t startFrame = -12;
    bool withSource = true;
};

[[nodiscard]] inline AudioPlotValues spectrumValues() {
    AudioPlotValues values;
    values.kind = runtime::AudioPlotKind::Spectrum;
    values.values = {20.0, 20000.0, -90.0, 0.0};
    return values;
}

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

[[nodiscard]] inline std::shared_ptr<const runtime::AudioAnalysisSource>
audioSource(const AudioFixture& fixture, const std::int64_t startFrame) {
    auto source = std::make_shared<runtime::AudioAnalysisSource>();
    source->asset = fixture.asset;
    source->startFrame = startFrame;
    return source;
}

[[nodiscard]] inline runtime::AudioPlotKernel
audioPlotKernel(const AudioPlotValues& values, const AudioFixture& fixture, const std::uint64_t idBase) {
    runtime::AudioPlotKernel kernel;
    kernel.kind = values.kind;
    kernel.width = values.width;
    kernel.height = values.height;
    kernel.separate = values.separate;
    kernel.fftSize = values.fftSize;
    kernel.logarithmic = values.logarithmic;
    for (std::size_t slot = 0; slot < kernel.values.size(); ++slot)
        kernel.values[slot] = scalar(idBase + 10 + slot, values.values[slot]);
    kernel.color = CompiledColorParameter{ParameterId::fromRaw(idBase + 20), values.color};
    kernel.background = CompiledColorParameter{ParameterId::fromRaw(idBase + 21), values.background};
    if (values.withSource)
        kernel.source = audioSource(fixture, values.startFrame);
    return kernel;
}

// The canvas -> Audio plot -> plain Merge -> Composition Output, in a square composition.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
audioPlotPlan(const AudioPlotValues& values = {}, const std::uint64_t idBase = 347000,
              const AudioFixture& fixture = sharedAudioFixture()) {
    using namespace runtime;
    using document::NodeId;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kPlotComposition, kPlotComposition),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 1),
                                          {ParameterId::fromRaw(idBase + 2), core::Color4d{0.0, 0.0, 0.0, 0.0}},
                                          scalar(idBase + 3, static_cast<double>(values.width)),
                                          scalar(idBase + 4, static_cast<double>(values.height))});
    const auto plot = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledImageEffect{NodeId::fromRaw(idBase + 5), OperationIndex::fromRaw(0),
                                                audioPlotKernel(values, fixture, idBase)});
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, plot}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::audio_plot_test

#endif // BLOOM_RUNTIME_TESTS_AUDIO_PLOT_TEST_SUPPORT_HPP

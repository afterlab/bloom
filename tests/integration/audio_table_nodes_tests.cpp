// #631 to #635 the table nodes through the production path: a document (an audio layer and the
// nodes), the real compiler and the CPU evaluator, over a real file whose answers are known.
//
// The file is four seconds of stereo: clicks (5 ms of a 2 kHz sine at 0.8, both channels) every
// half second from 0.25 s, which is 120 beats a minute; a 440 Hz sine at 0.25 on both channels
// for the first two seconds; and a 100 Hz sine at 0.25 on the left only for the last two.

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/group_definition_operations.hpp>
#include <bloom/commands/keyframe_bake_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/file_uri.hpp>
#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/platform/private_temporary_directory.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/audio_sound.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/node_path_query.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace bloom;

namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
core::RationalTime at(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value)
        throw std::logic_error("invalid fixture time");
    return *value;
}
bool near(const double value, const double want, const double tolerance) {
    return std::fabs(value - want) <= tolerance;
}

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint32_t kSeconds = 4;
std::filesystem::path scratch;

void put(std::ofstream& out, const std::uint32_t value, const int bytes) {
    for (int index = 0; index < bytes; ++index)
        out.put(static_cast<char>((value >> (8 * index)) & 0xFFU));
}

document::AssetRecord writeSound() {
    const auto path = scratch / "sound.wav";
    const std::uint32_t frames = kRate * kSeconds;
    std::vector<float> left(frames, 0.0F);
    std::vector<float> right(frames, 0.0F);
    const auto tone = [&](const double hertz, const std::uint32_t frame) {
        return std::sin(2.0 * std::numbers::pi * hertz * static_cast<double>(frame) / kRate);
    };
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        if (frame < 2 * kRate) {
            left[frame] += static_cast<float>(0.25 * tone(440.0, frame));
            right[frame] += static_cast<float>(0.25 * tone(440.0, frame));
        } else {
            left[frame] += static_cast<float>(0.25 * tone(100.0, frame));
        }
    }
    for (std::uint32_t start = kRate / 4; start < frames; start += kRate / 2)
        for (std::uint32_t index = 0; index < 240; ++index) {
            const auto click = static_cast<float>(0.8 * tone(2000.0, index));
            left[start + index] += click;
            right[start + index] += click;
        }
    {
        std::ofstream out(path, std::ios::binary);
        const std::uint32_t dataBytes = frames * 2U * 4U;
        out.write("RIFF", 4);
        put(out, 36U + dataBytes, 4);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        put(out, 16, 4);
        put(out, 3, 2);
        put(out, 2, 2);
        put(out, kRate, 4);
        put(out, kRate * 8U, 4);
        put(out, 8, 2);
        put(out, 32, 2);
        out.write("data", 4);
        put(out, dataBytes, 4);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            put(out, std::bit_cast<std::uint32_t>(left[frame]), 4);
            put(out, std::bit_cast<std::uint32_t>(right[frame]), 4);
        }
    }
    const auto hash = platform::hashSourceFile(path);
    if (hash.error != platform::SourceHashError::None)
        throw std::logic_error("the fixture could not be hashed");
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(631);
    asset.kind = document::AssetKind::Audio;
    asset.locator = {"file", "project-relative", "sound.wav",
                     core::fileUriFromAbsolutePath(std::filesystem::absolute(path))
                         .value_or(std::string{})};
    asset.contentDigest = hash.digest;
    asset.rate = kRate;
    asset.channels = 2;
    asset.frames = frames;
    asset.duration = at(kSeconds);
    asset.name = "sound.wav";
    return asset;
}

struct Fixture {
    document::AssetRecord asset;
    document::NewProject initial;
    document::Document document;
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    explicit Fixture(const document::AssetRecord& sound)
        : asset(sound), initial(makeProject(sound)), document(std::move(initial.project)) {
        run<commands::AddAudioLayer>(composition, asset.id);
    }
    static document::NewProject makeProject(const document::AssetRecord& asset) {
        auto project = document::makeNewProject(
            "Audio table", "Main", asset.duration,
            *document::CompositionFormat::create(4, 4, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        if (!project.project.addAsset(asset))
            throw std::runtime_error("fixture assets");
        return project;
    }
    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Audio table", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.changed() && result.status != commands::CommandStatus::NoChange) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
        return result;
    }
    [[nodiscard]] const document::CanonicalGraph& graph() const {
        return document.snapshot().project().findComposition(composition)->graph();
    }
    [[nodiscard]] document::NodeId nodeOfType(const std::string_view type) const {
        for (const auto& node : graph().nodes())
            if (node.typeId == type)
                return node.id;
        throw std::runtime_error("fixture node");
    }
    // A node of `type` that reads the layer's sound.
    document::NodeId add(const std::string_view type) {
        run<commands::AddNode>(composition, std::string(type), document::Vec2d{0.0, 0.0});
        document::NodeId added;
        for (const auto& node : graph().nodes())
            if (node.typeId == type && node.id.value() > added.value())
                added = node.id;
        if (document::isAudioAnalysisNodeType(type))
            run<commands::ConnectPorts>(
                composition,
                document::OutputPortRef{nodeOfType(document::kLayerOutputNodeType),
                                        std::string(document::kLayerOutputAudioOutputPort)},
                document::NodeInputRef{added, std::string(document::kAudioAnalysisInputPort)});
        return added;
    }
    void set(const document::NodeId node, const std::string_view role, const double value) {
        run<commands::SetNodeParameterValue>(composition, node, std::string(role),
                                             document::ParameterValue{value});
    }
    void choose(const document::NodeId node, const std::string_view role, const std::int64_t value) {
        run<commands::SetNodeParameterValue>(composition, node, std::string(role),
                                             document::ParameterValue{value});
    }
};

struct Outputs final {
    std::vector<runtime::CompiledValue> values;
    std::vector<std::string> names;
    [[nodiscard]] double scalar(const std::string_view name) const {
        for (std::size_t index = 0; index < names.size() && index < values.size(); ++index)
            if (names[index] == name)
                if (const auto* value = std::get_if<double>(&values[index]))
                    return *value;
        expect(false, "the output " + std::string(name) + " is a Scalar");
        return std::nan("");
    }
    [[nodiscard]] std::vector<double> list(const std::string_view name) const {
        for (std::size_t index = 0; index < names.size() && index < values.size(); ++index)
            if (names[index] == name)
                if (const auto* value = std::get_if<runtime::CompiledNumberList>(&values[index]))
                    return *value->values;
        expect(false, "the output " + std::string(name) + " is a number list");
        return {};
    }
};

// The node's outputs at `seconds`, as a frame evaluated by the CPU evaluator reads them.
Outputs read(const Fixture& fixture, const document::NodeId node, const double seconds) {
    Outputs outputs;
    runtime::SnapshotCompileRequest compile{fixture.document.snapshot(), fixture.composition};
    compile.valueReadouts.push_back(node);
    const auto compiled =
        runtime::SnapshotCompiler(document::builtInNodeDefinitions()).compile(compile, {});
    if (!compiled.plan) {
        expect(false, "the composition compiles: " +
                          (compiled.diagnostics.empty() ? std::string("no diagnostic")
                                                        : compiled.diagnostics.front().summary));
        return outputs;
    }
    const auto* record = fixture.graph().findNode(node);
    for (const auto port : document::audioTableOutputPorts(record->typeId))
        outputs.names.emplace_back(port);
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(scratch);
    const auto evaluation = evaluator.evaluate(
        compiled.plan,
        {.time = at(std::llround(seconds * 1000.0), 1000),
         .output = compiled.plan->output(),
         .resolution = runtime::CompositionFormatResolution{},
         .quality = runtime::EvaluationQuality::Reference,
         .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
         .pixelStorageByteLimit = std::size_t{64} * 1024U * 1024U},
        {});
    if (evaluation.frame() == nullptr) {
        expect(false, "the frame evaluates");
        return outputs;
    }
    for (const auto& operation : compiled.plan->valueOperations())
        if (operation.sourceNodeId == node) {
            const auto all = evaluation.frame()->valueOutputs();
            for (std::size_t slot = 0; slot < operation.outputCount; ++slot)
                outputs.values.push_back(all[operation.firstOutput.value() + slot]);
        }
    expect(outputs.values.size() == outputs.names.size(), "every output of the node is evaluated");
    return outputs;
}

namespace choices = document::image_pixel_parameters;

// #632 Audio Bands: the 100 Hz sine is in the low band, on the left, in the last two seconds.
void bands(const document::AssetRecord& sound) {
    Fixture fixture(sound);
    const auto node = fixture.add(document::kAudioBandsNodeType);
    fixture.set(node, "lowFrequency", 20.0);
    fixture.set(node, "highFrequency", 200.0);
    fixture.choose(node, "normalize", 1); // Peak.
    expect(read(fixture, node, 1.0).scalar("level") < 0.02,
           "the low band is silent while only the 440 Hz sine sounds");
    expect(near(read(fixture, node, 3.0).scalar("level"), 1.0, 0.02),
           "the low band is at its peak while the 100 Hz sine sounds");
    // The time offset reads the curve later: at one second, two seconds on.
    fixture.set(node, "offset", 2.0);
    expect(near(read(fixture, node, 1.0).scalar("level"), 1.0, 0.02),
           "a time offset reads the sound ahead");
    fixture.set(node, "offset", 0.0);
    // The channel: the sine is on the left only.
    fixture.choose(node, "channel", 2); // Right.
    expect(read(fixture, node, 3.0).scalar("level") < 0.02, "the right channel has no low band");
    fixture.choose(node, "channel", 1); // Left.
    expect(near(read(fixture, node, 3.0).scalar("level"), 1.0, 0.02), "the left channel has");
    // A manual range: the sine's power on the left is 0.0625 (-12.04 dB), the mix's half that
    // (-15.05 dB), which is 62.4 % of the way from -40 dB to 0 dB.
    fixture.choose(node, "channel", 0);
    fixture.choose(node, "normalize", 3);
    fixture.set(node, "floor", -40.0);
    fixture.set(node, "ceiling", 0.0);
    const double manual = read(fixture, node, 3.0).scalar("level");
    expect(near(manual, 0.624, 0.02), "a manual range maps decibels: " + std::to_string(manual));
    // Attack: a tenth of a second after the sine starts, with a tenth of a second of attack, the
    // level has covered about 1 - 1/e of its rise.
    fixture.set(node, "attack", 100.0);
    const double rising = read(fixture, node, 2.1).scalar("level");
    expect(rising > manual * 0.5 && rising < manual * 0.75,
           "attack slows the rise: " + std::to_string(rising));
    fixture.set(node, "attack", 0.0);
    // Release: a second after... the sine does not stop, so the fall is read at the 440 Hz band.
    fixture.set(node, "lowFrequency", 300.0);
    fixture.set(node, "highFrequency", 600.0);
    const double before = read(fixture, node, 2.2).scalar("level");
    fixture.set(node, "release", 500.0);
    const double held = read(fixture, node, 2.2).scalar("level");
    expect(before < 0.05 && held > 0.3, "release slows the fall: " + std::to_string(before) +
                                            " to " + std::to_string(held));
    // An empty range says so.
    fixture.set(node, "lowFrequency", 800.0);
    expect(read(fixture, node, 1.0).scalar("level") == 0.0, "an empty range reads 0");

    // #630 with #632: an equalizer before the node changes what the band holds.
    Fixture filtered(sound);
    const auto equalizer = filtered.add(document::kAudioEqualizerNodeType);
    {
        const auto layer = filtered.nodeOfType(document::kLayerOutputNodeType);
        const document::InputPortRef layerAudio = document::NodeInputRef{
            layer, std::string(document::kLayerOutputAudioInputPort)};
        const auto fed = std::ranges::find_if(filtered.graph().edges(), [&](const auto& edge) {
            return edge.destination == layerAudio;
        });
        const auto upstream = fed->source;
        filtered.run<commands::DisconnectInput>(filtered.composition, layerAudio);
        filtered.run<commands::ConnectPorts>(filtered.composition, upstream,
                                             document::NodeInputRef{equalizer, "audio"});
        filtered.run<commands::ConnectPorts>(
            filtered.composition, document::OutputPortRef{equalizer, "audio"}, layerAudio);
    }
    const auto low = filtered.add(document::kAudioBandsNodeType);
    filtered.set(low, "lowFrequency", 20.0);
    filtered.set(low, "highFrequency", 200.0);
    filtered.choose(low, "normalize", 3);
    filtered.set(low, "floor", -40.0);
    filtered.set(low, "ceiling", 0.0);
    const double open = read(filtered, low, 3.0).scalar("level");
    filtered.set(equalizer, "highPass", 2000.0);
    const double cut = read(filtered, low, 3.0).scalar("level");
    expect(near(open, 0.624, 0.03) && cut < open - 0.3,
           "a high-pass before the node lowers the low band: " + std::to_string(open) + " to " +
               std::to_string(cut));
}

// #633 Audio Sample: the spectrum and the waveform as lists and at a position.
void sample(const document::AssetRecord& sound) {
    Fixture fixture(sound);
    const auto node = fixture.add(document::kAudioSampleNodeType);
    fixture.choose(node, "channel", 1); // Left.
    const auto low = read(fixture, node, 3.0);
    const auto spectrum = low.list("spectrum");
    expect(spectrum.size() == document::kAudioSampleSpectrumItems, "the spectrum has 64 items");
    if (spectrum.size() == document::kAudioSampleSpectrumItems) {
        // 100 Hz is in band 14 of 64 (90.6 Hz to 100.9 Hz); the transform bin it falls in
        // reaches into the bands beside it.
        const auto peak = static_cast<std::size_t>(
            std::max_element(spectrum.begin(), spectrum.end()) - spectrum.begin());
        expect(peak >= 13 && peak <= 15, "the spectrum peaks at 100 Hz: band " +
                                             std::to_string(peak));
        expect(spectrum[40] == 0.0, "and is empty far from it");
        // -12 dB on the left spread over a few bands: well inside the default -72 dB to 0 dB.
        expect(spectrum[peak] > 0.6 && spectrum[peak] < 0.9,
               "a level maps into the range: " + std::to_string(spectrum[peak]));
        fixture.set(node, "position", static_cast<double>(peak) / 63.0);
        expect(near(read(fixture, node, 3.0).scalar("value"), spectrum[peak], 1e-9),
               "the value is the spectrum at the position");
    }
    const auto waveform = low.list("waveform");
    expect(waveform.size() == document::kAudioSampleWaveformItems, "the waveform has 256 items");
    if (!waveform.empty()) {
        // The mono sum of a sine at 0.25 on one channel swings to 0.125.
        const auto [least, most] = std::minmax_element(waveform.begin(), waveform.end());
        expect(near(*most, 0.125, 0.01) && near(*least, -0.125, 0.01),
               "the waveform swings to the sine's amplitude: " + std::to_string(*least) + " to " +
                   std::to_string(*most));
        fixture.choose(node, "read", choices::kAudioSampleWaveform);
        fixture.set(node, "position", 0.0);
        expect(near(read(fixture, node, 3.0).scalar("value"), waveform.front(), 1e-9),
               "the value is the waveform at the position");
    }
    // Outside the sound both lists are silence, of the same size.
    fixture.set(node, "offset", 10.0);
    const auto outside = read(fixture, node, 3.0);
    expect(outside.list("spectrum") == std::vector<double>(64, 0.0) &&
               outside.list("waveform") == std::vector<double>(256, 0.0),
           "outside the sound the lists are silence");

    // The lists reach a scripted read: a Number List input takes them.
    Fixture scripted(sound);
    const auto reader = scripted.add(document::kAudioSampleNodeType);
    const auto queried = scripting::queryNodeNumberList(
        scripted.document.snapshot(), scripted.composition, reader, "spectrum",
        at(3), {}, scratch);
    expect(queried.list && queried.list->values.size() == 64 && queried.diagnostic.empty(),
           "a scripted read gets the spectrum: " + queried.diagnostic);
}

// #634 Audio Features.
void features(const document::AssetRecord& sound) {
    Fixture fixture(sound);
    const auto node = fixture.add(document::kAudioFeaturesNodeType);
    // The sound starts at once, which is a hit; then the clicks at 0.25 and 0.75.
    const auto one = read(fixture, node, 1.0);
    expect(one.scalar("hitCount") == 3.0, "three hits before one second: " +
                                              std::to_string(one.scalar("hitCount")));
    expect(near(one.scalar("sinceHit"), 0.25, 0.03), "a quarter second since the last: " +
                                                         std::to_string(one.scalar("sinceHit")));
    expect(one.scalar("hit") == 0.0, "the hit pulse has passed");
    const auto justAfter = read(fixture, node, 0.78);
    expect(justAfter.scalar("hit") > 0.4 && justAfter.scalar("hit") <= 1.0,
           "the hit pulse is high just after a hit: " + std::to_string(justAfter.scalar("hit")));
    // Before the sound there is no hit yet.
    fixture.set(node, "offset", -0.5);
    const auto before = read(fixture, node, 0.1);
    expect(before.scalar("hitCount") == 0.0 && before.scalar("hit") == 0.0 &&
               before.scalar("sinceHit") == 0.0,
           "before the first hit the count is 0");
    fixture.set(node, "offset", 0.0);
    // The start, the eight clicks, and the change of sine at two seconds.
    expect(read(fixture, node, 3.9).scalar("hitCount") == 10.0,
           "ten hits in the four seconds: " +
               std::to_string(read(fixture, node, 3.9).scalar("hitCount")));
    // A longer shortest gap keeps the start and the clicks at 1.25, 2.25 and 3.25.
    fixture.set(node, "gap", 800.0);
    expect(read(fixture, node, 3.9).scalar("hitCount") == 4.0, "a longer gap drops hits");
    fixture.set(node, "gap", 80.0);
    // At three seconds only the 100 Hz sine on the left sounds.
    const auto three = read(fixture, node, 3.0);
    expect(near(three.scalar("pan"), -1.0, 0.01), "the low sine is all left");
    expect(near(three.scalar("width"), 0.5, 0.01), "one channel is half side");
    expect(near(three.scalar("brightness"), 0.233, 0.05),
           "100 Hz is low: " + std::to_string(three.scalar("brightness")));
    expect(three.scalar("noisiness") < 0.05, "a sine is not noise");
    // At one second the 440 Hz sine is on both channels.
    expect(near(one.scalar("pan"), 0.0, 0.01) && near(one.scalar("width"), 0.0, 0.01),
           "the first sine is centred and mono");
    expect(one.scalar("brightness") > three.scalar("brightness") + 0.1, "and brighter");
    const double early = one.scalar("total");
    const double late = read(fixture, node, 3.5).scalar("total");
    expect(early > 0.0 && late > early, "the running total grows: " + std::to_string(early) +
                                            " to " + std::to_string(late));
}

// #635 Audio Beat and Audio Pitch.
void beatAndPitch(const document::AssetRecord& sound) {
    Fixture fixture(sound);
    const auto beat = fixture.add(document::kAudioBeatNodeType);
    // An eighth of a second after the click at 1.25 s is a quarter of the way to the next.
    const auto read1 = read(fixture, beat, 1.375);
    expect(near(read1.scalar("tempo"), 120.0, 1.0),
           "the clicks are 120 a minute: " + std::to_string(read1.scalar("tempo")));
    expect(near(read1.scalar("phase"), 0.25, 0.06),
           "a quarter of the way through the beat: " + std::to_string(read1.scalar("phase")));
    expect(near(read1.scalar("pulse"), 1.0 - read1.scalar("phase"), 1e-9),
           "the pulse falls as the phase rises");
    expect(read1.scalar("beat") == 3.0, "the third beat: " + std::to_string(read1.scalar("beat")));
    const auto pitch = fixture.add(document::kAudioPitchNodeType);
    const auto first = read(fixture, pitch, 1.0);
    const auto classes = first.list("classes");
    double sum = 0.0;
    for (const double share : classes)
        sum += share;
    expect(classes.size() == 12 && near(sum, 1.0, 1e-3), "twelve shares that sum to 1");
    expect(first.scalar("strongest") == 9.0 && first.scalar("strength") > 0.9,
           "440 Hz is A: class " + std::to_string(first.scalar("strongest")) + " at " +
               std::to_string(first.scalar("strength")));
    // 100 Hz is nearest G2 (98 Hz): class 7.
    const auto second = read(fixture, pitch, 3.0);
    expect(second.scalar("strongest") == 7.0 && second.scalar("strength") > 0.9,
           "100 Hz is G: class " + std::to_string(second.scalar("strongest")));
}

// One sound serves every node: it is rendered and its table built once.
void oneSound(const document::AssetRecord& sound) {
    runtime::clearAudioAnalysis();
    runtime::clearAudioSounds();
    Fixture fixture(sound);
    const auto low = fixture.add(document::kAudioBandsNodeType);
    const auto beat = fixture.add(document::kAudioBeatNodeType);
    const double forward = read(fixture, low, 3.0).scalar("level");
    (void)read(fixture, beat, 1.0);
    expect(runtime::audioSoundStatistics().rendered == 1,
           "two nodes on one sound render it once: " +
               std::to_string(runtime::audioSoundStatistics().rendered));
    // The value does not depend on which frame was asked first.
    runtime::clearAudioAnalysis();
    runtime::clearAudioSounds();
    (void)read(fixture, low, 0.5);
    expect(read(fixture, low, 3.0).scalar("level") == forward,
           "a frame reads the same value whichever frame was asked first");
}

// A node with nothing wired reads silence without a failure.
void unwired(const document::AssetRecord& sound) {
    Fixture fixture(sound);
    fixture.run<commands::AddNode>(fixture.composition, std::string(document::kAudioPitchNodeType),
                                   document::Vec2d{0.0, 0.0});
    const auto node = fixture.nodeOfType(document::kAudioPitchNodeType);
    const auto silent = read(fixture, node, 1.0);
    expect(silent.list("classes") == std::vector<double>(12, 0.0) &&
               silent.scalar("strongest") == -1.0 && silent.scalar("strength") == 0.0,
           "no audio reads no pitch class");
}

// #633 Audio Sample through Function Plot: a Function group whose argument drives the position
// draws the whole spectrum as a path, one anchor an item.
void functionPlot(const document::AssetRecord& sound) {
    Fixture fixture(sound);
    const auto main = fixture.composition;
    const auto addIn = [&](const document::CompositionId in, const std::string_view type) {
        const auto made =
            fixture.run<commands::AddNode>(in, std::string(type), document::Vec2d{0.0, 0.0});
        const auto id = made.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!id)
            throw std::runtime_error("fixture node was not added");
        return *id;
    };
    const auto parameterOf = [&](const document::CompositionId in, const document::NodeId node,
                                 const std::string_view role) {
        const auto* composition = fixture.document.snapshot().project().findComposition(in);
        const auto* record = composition != nullptr ? composition->graph().findNode(node) : nullptr;
        if (record != nullptr)
            for (const auto& binding : record->parameters)
                if (binding.role == role)
                    return binding.parameterId;
        throw std::runtime_error("fixture parameter is missing: " + std::string(role));
    };
    const auto set = [&](const document::CompositionId in, const document::NodeId node,
                         const std::string_view role, document::ParameterValue value) {
        fixture.run<commands::SetParameterSource>(in, parameterOf(in, node, role),
                                                  document::ConstantValueSource{std::move(value)});
    };
    // The reference: an Audio Sample in the composition, reading the layer's sound.
    const auto reference = fixture.add(document::kAudioSampleNodeType);
    const auto spectrum = read(fixture, reference, 1.0).list("spectrum");
    expect(spectrum.size() == document::kAudioSampleSpectrumItems &&
               *std::ranges::max_element(spectrum) > 0.1,
           "the reference spectrum has its 64 items and is not silent");
    if (spectrum.size() != document::kAudioSampleSpectrumItems)
        return;

    // The function: position in, the spectrum's value there out.
    const auto made = fixture.run<commands::CreateGroupDefinition>(main, "Spectrum");
    const auto group = made.outputId<document::CompositionId>(commands::kCreateGroupDefinitionOutput);
    const auto groupInput =
        made.outputId<document::NodeId>(commands::kCreateGroupDefinitionInputNodeOutput);
    const auto groupOutput =
        made.outputId<document::NodeId>(commands::kCreateGroupDefinitionOutputNodeOutput);
    expect(group && groupInput && groupOutput, "a group definition is made");
    if (!group || !groupInput || !groupOutput)
        return;
    const auto source = addIn(*group, document::kAudioSourceNodeType);
    set(*group, source, "asset", std::to_string(sound.id.value()));
    const auto sampled = addIn(*group, document::kAudioSampleNodeType);
    fixture.run<commands::ConnectPorts>(
        *group, document::OutputPortRef{source, std::string(document::kAudioSourceOutputPort)},
        document::NodeInputRef{sampled, std::string(document::kAudioAnalysisInputPort)});
    fixture.run<commands::SetParameterSource>(
        *group, parameterOf(*group, sampled, "position"),
        document::DriverBindingSource{*groupInput, std::string(document::kFunctionInputSlot)});
    fixture.run<commands::SetParameterSource>(
        *group, parameterOf(*group, *groupOutput, document::kFunctionOutputSlot),
        document::DriverBindingSource{sampled, "value"});

    const auto plot = addIn(main, document::kFunctionPlotNodeType);
    set(main, plot, "group", static_cast<std::int64_t>(group->value()));
    set(main, plot, document::kFunctionPlotStartRole, 0.0);
    set(main, plot, document::kFunctionPlotEndRole, 1.0);
    set(main, plot, document::kFunctionPlotSamplesRole,
        static_cast<std::int64_t>(document::kAudioSampleSpectrumItems));
    set(main, plot, document::kFunctionPlotOriginRole, document::Vec2d{0.0, 0.0});
    set(main, plot, document::kFunctionPlotScaleRole, document::Vec2d{1.0, 1.0});
    set(main, plot, document::kFunctionPlotSmoothRole, false);
    const auto drawn =
        scripting::queryNodePath(fixture.document.snapshot(), main, plot, {}, at(1), {}, scratch);
    expect(drawn.path.has_value() && drawn.path->contours.size() == 1 &&
               drawn.path->contours[0].anchors.size() == spectrum.size(),
           "Function Plot draws the spectrum as one contour of 64 anchors: " + drawn.diagnostic);
    if (!drawn.path || drawn.path->contours.size() != 1 ||
        drawn.path->contours[0].anchors.size() != spectrum.size())
        return;
    bool follows = true;
    double worst = 0.0;
    for (std::size_t index = 0; index < spectrum.size(); ++index) {
        const auto& point = drawn.path->contours[0].anchors[index].point;
        const double x = static_cast<double>(index) / static_cast<double>(spectrum.size() - 1);
        // Up is negative y in a path, so the plot's height is -y.
        worst = std::max(worst, std::fabs(-point.y - spectrum[index]));
        follows = follows && near(point.x, x, 1e-9) && near(-point.y, spectrum[index], 1e-3);
    }
    expect(follows, "every anchor is at its item's position and level: worst difference " +
                        std::to_string(worst));
}

// A token whose task was cancelled, made the way a frame's is: by a scheduler task.
runtime::CancellationToken cancelledToken() {
    using namespace runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("bake cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(636)}),
        [&](TaskContext& context) {
            {
                const std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted())
        throw std::logic_error("the cancel token task was refused");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        const std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    return token;
}

// #636 Bake to keyframes: the node read at every frame, stored as keys on a Number node in one
// undo step; the keys are what the live node gave and do not follow it afterwards.
void bake(const document::AssetRecord& sound) {
    Fixture fixture(sound);
    const auto node = fixture.add(document::kAudioBandsNodeType);
    fixture.set(node, "lowFrequency", 20.0);
    fixture.set(node, "highFrequency", 200.0);
    fixture.choose(node, "normalize", 1); // Peak.
    const auto snapshot = fixture.document.snapshot();
    auto read = scripting::queryNodeScalarKeys(snapshot, fixture.composition, node, "level", {},
                                               scratch);
    // Four seconds at 24 frames a second.
    expect(read.keys.size() == 96 && read.diagnostic.empty() && !read.cancelled,
           "a bake reads one value a frame: " + std::to_string(read.keys.size()) + " " +
               read.diagnostic);
    if (read.keys.size() != 96)
        return;
    expect(read.keys.front().time == at(0) && read.keys[72].time == at(3) &&
               read.keys.back().time == at(95, 24),
           "each key is at its frame's time");
    expect(read.keys[24].value < 0.02 && near(read.keys[72].value, 1.0, 0.02),
           "the keys follow the sound: quiet at one second, at the peak at three");
    expect(read.keys[72].value == ::read(fixture, node, 3.0).scalar("level"),
           "a baked value is the value the live node gives at that frame");
    expect(scripting::nodeBakeActivity().framesTotal == 0, "a finished bake reports no progress");
    // The default port is the node's first number output.
    const auto first =
        scripting::queryNodeScalarKeys(snapshot, fixture.composition, node, {}, {}, scratch);
    expect(first.keys.size() == 96 && first.keys[72].value == read.keys[72].value,
           "no port named reads the first number output");
    // What cannot be baked says why and gives nothing.
    const auto missing =
        scripting::queryNodeScalarKeys(snapshot, fixture.composition, node, "nope", {}, scratch);
    expect(missing.keys.empty() && missing.diagnostic.find("no number output named") !=
                                       std::string::npos,
           "a port the node does not have is refused: " + missing.diagnostic);
    const auto pitch = fixture.add(document::kAudioPitchNodeType);
    const auto list = scripting::queryNodeScalarKeys(fixture.document.snapshot(),
                                                     fixture.composition, pitch, "classes", {},
                                                     scratch);
    expect(list.keys.empty() && !list.diagnostic.empty(), "a number list output is not baked");
    const auto stopped = scripting::queryNodeScalarKeys(snapshot, fixture.composition, node,
                                                        "level", cancelledToken(), scratch);
    expect(stopped.cancelled && stopped.keys.empty(), "a cancelled bake gives nothing");

    // Stored: one new Number node, keyed at every frame, in one undo step.
    const auto nodesBefore = fixture.graph().nodes().size();
    const auto undoBefore = fixture.commands.size();
    const auto values = read.keys;
    const auto done = fixture.run<commands::BakeValueToKeyframes>(
        fixture.composition, std::move(read.keys), document::Vec2d{300.0, 0.0});
    const auto number = done.outputId<document::NodeId>(commands::kBakeToKeyframesNodeOutput);
    expect(number.has_value() && fixture.graph().nodes().size() == nodesBefore + 1 &&
               fixture.commands.size() == undoBefore + 1,
           "a bake adds one Number node in one undo step");
    if (!number)
        return;
    const auto curveOf = [&]() -> const document::ScalarAnimationCurve* {
        const auto* composition =
            fixture.document.snapshot().project().findComposition(fixture.composition);
        const auto* record = composition->graph().findNode(*number);
        if (record == nullptr || record->typeId != document::kScalarValueNodeType)
            return nullptr;
        for (const auto& binding : record->parameters) {
            if (binding.role != document::kValueParameterRole)
                continue;
            const auto* stored = composition->parameters().find(binding.parameterId);
            const auto* source = stored != nullptr
                                     ? std::get_if<document::AnimationCurveSource>(&stored->source)
                                     : nullptr;
            return source != nullptr ? composition->animationCurves().findScalar(source->curveId)
                                     : nullptr;
        }
        return nullptr;
    };
    const auto* curve = curveOf();
    expect(curve != nullptr && curve->keyframes.size() == 96, "the Number node holds the keys");
    if (curve != nullptr && curve->keyframes.size() == 96) {
        bool same = true;
        for (std::size_t index = 0; index < 96; ++index)
            same = same && curve->keyframes[index].time == values[index].time &&
                   curve->keyframes[index].value == values[index].value;
        expect(same, "every key holds its frame's time and value");
    }
    // The keys do not follow the sound: the live node changes, the baked keys stay.
    fixture.set(node, "lowFrequency", 5000.0);
    fixture.set(node, "highFrequency", 8000.0);
    const auto* after = curveOf();
    expect(after != nullptr && after->keyframes.size() == 96 &&
               after->keyframes[72].value == values[72].value,
           "baked keys do not change when the live node does");
    expect(fixture.commands.undo().changed() && fixture.commands.undo().changed() &&
               fixture.commands.undo().changed() &&
               fixture.graph().nodes().size() == nodesBefore && curveOf() == nullptr,
           "one undo removes the baked node and its keys");
    // Refusals of the command itself.
    commands::Transaction empty("Bake", fixture.document.snapshot().revision());
    empty.emplace<commands::BakeValueToKeyframes>(fixture.composition,
                                                  std::vector<commands::BakedKey>{},
                                                  document::Vec2d{});
    expect(!fixture.commands.execute(std::move(empty)).changed(), "a bake of nothing is refused");
    commands::Transaction disordered("Bake", fixture.document.snapshot().revision());
    disordered.emplace<commands::BakeValueToKeyframes>(
        fixture.composition,
        std::vector<commands::BakedKey>{{at(1), 0.5}, {at(1), 0.25}}, document::Vec2d{});
    expect(!fixture.commands.execute(std::move(disordered)).changed(),
           "keys that are not in order of time are refused");
}
} // namespace

// #635 asks what Audio Beat and Audio Pitch get wrong on real music. This is the aid that report
// was made with, not a test (no test runs it, and the repository holds no recorded music):
//   <this program> --measure <file.wav>
// reads a 48 kHz stereo 32-bit float WAV and prints, for each second, the tempo and beat count
// Audio Beat reports and the leading note Audio Pitch reports, then the mean share of each note.
int measure(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<char, 12> riff{};
    in.read(riff.data(), static_cast<std::streamsize>(riff.size()));
    std::uint64_t dataBytes = 0;
    while (in) {
        std::array<char, 8> chunk{};
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (in.gcount() != static_cast<std::streamsize>(chunk.size()))
            break;
        std::uint32_t size = 0;
        std::memcpy(&size, chunk.data() + 4, 4);
        if (std::string_view(chunk.data(), 4) == "data") {
            dataBytes = size;
            break;
        }
        in.seekg(size + (size & 1U), std::ios::cur);
    }
    const auto frames = dataBytes / 8U;
    if (frames < kRate) {
        std::cerr << "not a 48 kHz stereo float WAV of at least a second: " << path << '\n';
        return 1;
    }
    const auto hash = platform::hashSourceFile(path);
    if (hash.error != platform::SourceHashError::None)
        return 1;
    const auto seconds = static_cast<std::uint32_t>(frames / kRate);
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(635);
    asset.kind = document::AssetKind::Audio;
    asset.locator = {
        "file", "project-relative", path.filename().string(),
        core::fileUriFromAbsolutePath(std::filesystem::absolute(path)).value_or(std::string{})};
    asset.contentDigest = hash.digest;
    asset.rate = kRate;
    asset.channels = 2;
    asset.frames = frames;
    asset.duration = at(seconds);
    asset.name = path.filename().string();
    Fixture fixture(asset);
    const auto beat = fixture.add(document::kAudioBeatNodeType);
    const auto pitch = fixture.add(document::kAudioPitchNodeType);
    std::vector<double> mean(12, 0.0);
    std::cout << "second tempo beat strongest strength\n";
    for (std::uint32_t second = 1; second < seconds; ++second) {
        const auto beats = read(fixture, beat, second);
        const auto notes = read(fixture, pitch, second);
        const auto classes = notes.list("classes");
        for (std::size_t note = 0; note < mean.size() && note < classes.size(); ++note)
            mean[note] += classes[note] / (seconds - 1);
        std::cout << second << ' ' << beats.scalar("tempo") << ' ' << beats.scalar("beat") << ' '
                  << notes.scalar("strongest") << ' ' << notes.scalar("strength") << '\n';
    }
    std::cout << "mean share of each note, C first:";
    for (const double share : mean)
        std::cout << ' ' << share;
    std::cout << '\n';
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const auto directory = platform::createPrivateTemporaryDirectory("bloom-audio-table");
    if (!directory) {
        std::cerr << "no scratch directory\n";
        return 1;
    }
    scratch = *directory;
    runtime::setAudioSoundDirectory(scratch / "sounds");
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--measure") {
            const int result = measure(argv[2]);
            runtime::clearAudioAnalysis();
            runtime::clearAudioSounds();
            runtime::setAudioSoundDirectory(std::nullopt);
            std::error_code ignored;
            std::filesystem::remove_all(scratch, ignored);
            return result;
        }
        const auto sound = writeSound();
        bands(sound);
        sample(sound);
        features(sound);
        beatAndPitch(sound);
        oneSound(sound);
        unwired(sound);
        bake(sound);
        functionPlot(sound);
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    runtime::clearAudioAnalysis();
    runtime::clearAudioSounds();
    runtime::setAudioSoundDirectory(std::nullopt);
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}

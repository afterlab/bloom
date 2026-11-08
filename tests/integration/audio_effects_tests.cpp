// #569 audio effect nodes through the production path: the document graph (audio source, effects,
// layer), the compiled plan, the evaluated mix, and the mixer that plays and exports it. Playback
// builds its clips from one mix; the export re-evaluates the mix at every block and replaces its
// clips; both must give the same samples for the same parameters, whatever the block size.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
constexpr auto kAudioAsset = document::AssetId::fromRaw(9000);
constexpr std::uint32_t kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

std::shared_ptr<const media::audio::AudioBuffer> tone(const double amplitude = 0.4) {
    // Two seconds of a 700 Hz tone with an amplitude step in the middle, so the dynamics move.
    std::vector<float> samples(2 * kRate);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const double gain = index < kRate ? amplitude : amplitude * 0.25;
        samples[index] = static_cast<float>(
            gain * std::sin(2.0 * kPi * 700.0 * static_cast<double>(index) / kRate));
    }
    return std::make_shared<const media::audio::AudioBuffer>(
        media::audio::AudioBuffer{kRate, 1, samples.size(), {std::move(samples)}});
}

struct Fixture {
    document::NewProject initial = makeProject();
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    static document::NewProject makeProject() {
        auto project = document::makeNewProject(
            "Audio effects", "Main", at(2),
            *document::CompositionFormat::create(4, 4, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        document::AssetRecord audio;
        audio.id = kAudioAsset;
        audio.kind = document::AssetKind::Audio;
        audio.locator = {"file", "project-relative", "tone.wav", "file:tone.wav"};
        audio.rate = kRate;
        audio.channels = 1;
        audio.frames = 2 * kRate;
        audio.duration = at(2);
        if (!project.project.addAsset(audio))
            throw std::runtime_error("fixture assets");
        return project;
    }
    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Audio effects", document.snapshot().revision());
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
    [[nodiscard]] const document::NodeRecord& nodeOfType(const std::string_view type) const {
        for (const auto& node : graph().nodes())
            if (node.typeId == type)
                return node;
        throw std::runtime_error("fixture node");
    }
    // Adds an effect and wires it between the chain's current head (nearest the layer) and the
    // layer: layer.audio <- effect <- whatever fed the layer before.
    document::NodeId addEffect(const std::string_view type) {
        const auto before = graph().nodes().size();
        run<commands::AddNode>(composition, std::string(type), document::Vec2d{0.0, 0.0});
        document::NodeId effect;
        for (const auto& node : graph().nodes())
            if (node.typeId == type)
                effect = node.id;
        if (graph().nodes().size() != before + 1)
            throw std::runtime_error("effect not added");
        const auto& layer = nodeOfType(document::kLayerOutputNodeType);
        const document::InputPortRef layerAudio = document::NodeInputRef{
            layer.id, std::string(document::kLayerOutputAudioInputPort)};
        const auto fed = std::ranges::find_if(graph().edges(), [&](const auto& edge) {
            return edge.destination == layerAudio;
        });
        const auto upstream = fed->source;
        run<commands::DisconnectInput>(composition, layerAudio);
        run<commands::ConnectPorts>(composition, upstream,
                                    document::NodeInputRef{effect, "audio"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{effect, "audio"},
                                    layerAudio);
        return effect;
    }
    void setParameter(const document::NodeId node, const std::string_view role, const double v) {
        run<commands::SetNodeParameterValue>(composition, node, std::string(role),
                                             document::ParameterValue{v});
    }
    [[nodiscard]] document::ParameterId parameterOf(const document::NodeId node,
                                                    const std::string_view role) const {
        const auto* record = graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("fixture parameter");
    }
    [[nodiscard]] runtime::SnapshotCompileResult compiled() const {
        return runtime::SnapshotCompiler(document::builtInNodeDefinitions())
            .compile({document.snapshot(), composition}, {});
    }
    [[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> plan() const {
        const auto result = compiled();
        if (!result.plan)
            throw std::runtime_error("fixture compile");
        return result.plan;
    }
    [[nodiscard]] runtime::AudioMixDescription mix(const core::RationalTime time) const {
        auto mix = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(plan(), time);
        if (!mix)
            throw std::runtime_error("fixture audio mix");
        return *mix;
    }
};

media::audio::effects::EffectKind kindOf(const runtime::AudioEffectKind kind) {
    return static_cast<media::audio::effects::EffectKind>(kind);
}

// The clips as the export builds them: the effects at the block start, moving to the block end.
std::vector<media::audio::playback::AudioClip>
clipsFor(const runtime::AudioMixDescription& mix,
         const std::shared_ptr<const media::audio::AudioBuffer>& buffer,
         const runtime::AudioMixDescription* end = nullptr,
         const std::optional<media::audio::playback::AudioEffectRamp> ramp = {}) {
    std::vector<media::audio::playback::AudioClip> clips;
    for (std::size_t index = 0; index < mix.clips.size(); ++index) {
        const auto& description = mix.clips[index];
        clips.push_back({.buffer = buffer,
                         .startTime = description.startTime,
                         .level = static_cast<float>(description.level),
                         .muted = description.muted,
                         .solo = description.solo,
                         .endTime = description.endTime,
                         .mapTime = {}});
        if (!description.timeMappings.empty())
            clips.back().mapTime = [description](const core::RationalTime time) {
                return runtime::mapAudioClipTime(description, time);
            };
        clips.back().effectKey = description.sourceNodeId.value() * 1'000'003ULL + index;
        for (std::size_t effect = 0; effect < description.effects.size(); ++effect) {
            const auto& one = description.effects[effect];
            const bool paired = end != nullptr && index < end->clips.size() &&
                                effect < end->clips[index].effects.size();
            clips.back().effects.push_back(media::audio::effects::makeEffectSettings(
                kindOf(one.kind), one.values,
                paired ? std::span<const double>(end->clips[index].effects[effect].values)
                       : std::span<const double>(one.values)));
        }
        clips.back().effectRamp = ramp;
    }
    return clips;
}

std::vector<float> playback(const Fixture& fixture, const std::int64_t start,
                            const std::size_t count) {
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* sink = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend), {kRate, 1, 16384});
    engine.replaceClips(clipsFor(fixture.mix(at(start, kRate)), tone()));
    if (engine.play(at(start, kRate)) || engine.renderForTesting(count))
        throw std::runtime_error("playback render");
    return {sink->captured().begin(), sink->captured().end()};
}

// The export: the mix is evaluated at both edges of every block, the clips are replaced, and the
// effect parameters move linearly across the block.
std::vector<float> exported(const Fixture& fixture, const std::int64_t start,
                            const std::size_t count, const std::size_t block) {
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* sink = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend), {kRate, 1, 16384});
    if (engine.play(at(start, kRate)))
        throw std::runtime_error("export start");
    const auto buffer = tone();
    std::vector<float> samples;
    for (std::size_t done = 0; done < count; done += block) {
        const auto frames = std::min(block, count - done);
        const auto from = at(start + static_cast<std::int64_t>(done), kRate);
        const auto to = at(start + static_cast<std::int64_t>(done + frames), kRate);
        const auto startMix = fixture.mix(from);
        const auto endMix = fixture.mix(to);
        engine.replaceClips(clipsFor(startMix, buffer, &endMix,
                                     media::audio::playback::AudioEffectRamp{from, to}));
        sink->clearCaptured();
        if (engine.renderForTesting(frames))
            throw std::runtime_error("export render");
        samples.insert(samples.end(), sink->captured().begin(), sink->captured().end());
    }
    return samples;
}

double rms(const std::vector<float>& samples, const std::size_t begin, const std::size_t end) {
    double sum = 0.0;
    for (std::size_t index = begin; index < end; ++index)
        sum += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
    return std::sqrt(sum / static_cast<double>(end - begin));
}

void chainCompilesInOrder() {
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, kAudioAsset);
    const auto eq = fixture.addEffect(document::kAudioEqualizerNodeType);
    const auto compressor = fixture.addEffect(document::kAudioCompressorNodeType);
    const auto plan = fixture.plan();
    expect(plan->audioMix().layers.size() == 1, "the audio layer compiles");
    const auto& effects = plan->audioMix().layers.front().effects;
    expect(effects.size() == 2 && effects[0].nodeId == eq && effects[1].nodeId == compressor &&
               effects[0].kind == runtime::AudioEffectKind::Equalizer &&
               effects[1].kind == runtime::AudioEffectKind::Compressor,
           "the effects compile nearest the source first");
    expect(effects.size() == 2 && effects[0].parameters.size() == 9 &&
               effects[1].parameters.size() == 6,
           "each effect carries every parameter of its node");
    const auto described = fixture.mix(at(0)).clips.front().effects;
    expect(described.size() == 2 && described[0].values.size() == 9 &&
               described[0].values[0] == 20.0 && described[1].values[0] == -18.0,
           "the evaluated mix carries the defaults");
    // A changed parameter is a different plan, so every cache keyed on the plan misses.
    fixture.setParameter(compressor, "threshold", -30.0);
    expect(!(*fixture.plan() == *plan), "an effect parameter is part of the plan's identity");
    expect(fixture.mix(at(0)).clips.front().effects[1].values[0] == -30.0,
           "the evaluated mix follows the edit");
}

void unwiredAndMutedEffects() {
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, kAudioAsset);
    const auto plain = playback(fixture, 0, 4800);
    const auto hiss = fixture.addEffect(document::kAudioHissNodeType);
    fixture.setParameter(hiss, "level", -20.0);
    const auto hissy = playback(fixture, 0, 4800);
    expect(hissy != plain, "a hiss node changes the sound");
    // Muting an effect bypasses it: the layer sounds as it did without the node.
    fixture.run<commands::SetNodeMuted>(fixture.composition, hiss, true);
    expect(playback(fixture, 0, 4800) == plain, "a muted effect is bypassed");
    fixture.run<commands::SetNodeMuted>(fixture.composition, hiss, false);
    // An effect whose audio input has nothing wired plays nothing.
    const auto& layer = fixture.nodeOfType(document::kLayerOutputNodeType);
    (void)layer;
    fixture.run<commands::DisconnectInput>(
        fixture.composition, document::InputPortRef{document::NodeInputRef{hiss, "audio"}});
    const auto plan = fixture.compiled();
    expect(plan.plan && plan.plan->audioMix().layers.empty(),
           "an effect with no audio input leaves the layer silent");
}

void wireOnlyChecks() {
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, kAudioAsset);
    const auto eq = fixture.addEffect(document::kAudioEqualizerNodeType);
    const auto* record = fixture.graph().findNode(eq);
    expect(record != nullptr && record->parameters.size() == 9, "the equalizer has nine parameters");
    // The sockets: Audio in and out, one Scalar per animatable parameter.
    const auto* definition = document::builtInNodeDefinitions().find(
        std::string(document::kAudioEqualizerNodeType), document::kAudioEffectNodeSchemaVersion);
    expect(definition != nullptr && definition->inputs.size() == 10 &&
               definition->outputs.size() == 1 &&
               definition->outputs.front().valueKind == document::SocketValueKind::Audio,
           "the equalizer has an audio input and output and a socket per parameter");
}

void staticChainPlaybackEqualsExport() {
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, kAudioAsset);
    const auto eq = fixture.addEffect(document::kAudioEqualizerNodeType);
    fixture.setParameter(eq, "highPass", 150.0);
    fixture.setParameter(eq, "midGain", 5.0);
    const auto compressor = fixture.addEffect(document::kAudioCompressorNodeType);
    fixture.setParameter(compressor, "threshold", -20.0);
    fixture.setParameter(compressor, "ratio", 4.0);
    const auto hiss = fixture.addEffect(document::kAudioHissNodeType);
    fixture.setParameter(hiss, "level", -45.0);
    const auto played = playback(fixture, 0, 24000);
    expect(played.size() == 24000 && rms(played, 0, 24000) > 0.01, "the chain plays something");
    for (const std::size_t block : {std::size_t{37}, std::size_t{512}, std::size_t{2048}})
        expect(exported(fixture, 0, 24000, block) == played,
               "export (block " + std::to_string(block) + ") equals playback, sample for sample");
    // Started later: the dynamics warm up from the audio just before, so the same samples come out.
    const auto whole = playback(fixture, 0, 72000);
    const auto later = playback(fixture, 60000, 6000);
    double worst = 0.0;
    for (std::size_t index = 0; index < later.size(); ++index)
        worst = std::max(worst, static_cast<double>(std::abs(later[index] - whole[60000 + index])));
    expect(worst < 2e-3, "playback started mid-clip equals a run from the start (worst " +
                             std::to_string(worst) + ")");
}

void animatedParameterMovesWithoutSteps() {
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, kAudioAsset);
    const auto eq = fixture.addEffect(document::kAudioEqualizerNodeType);
    // A mid band that rises from 0 to +12 dB at 700 Hz over the first second.
    const auto parameter = fixture.parameterOf(eq, "midGain");
    fixture.setParameter(eq, "midFrequency", 700.0);
    fixture.run<commands::CreateAnimationForParameter>(fixture.composition, parameter,
                                                       core::RationalTime{});
    fixture.run<commands::SetKeyframeAtTimeForParameter>(fixture.composition, parameter,
                                                         core::RationalTime{}, 0.0);
    fixture.run<commands::SetKeyframeAtTimeForParameter>(fixture.composition, parameter, at(1),
                                                         12.0);
    const auto plan = fixture.plan();
    expect(!std::holds_alternative<double>(
               plan->audioMix().layers.front().effects.front().parameters[4].source),
           "an animated effect parameter compiles to a curve");
    expect(fixture.mix(at(1, 2)).clips.front().effects.front().values[4] == 6.0,
           "the evaluated mix samples the curve at the mix time");
    for (const std::size_t block : {std::size_t{256}, std::size_t{512}, std::size_t{4800}}) {
        const auto samples = exported(fixture, 0, 40000, block);
        double previous = 0.0;
        double biggest = 0.0;
        bool rising = true;
        for (std::size_t window = 0; window + 480 <= 40000; window += 480) {
            const double level = rms(samples, window, window + 480);
            if (window > 0) {
                rising = rising && level >= previous * 0.998;
                biggest = std::max(biggest, 20.0 * std::log10(level / previous));
            }
            previous = level;
        }
        expect(rising, "the animated gain rises with block " + std::to_string(block));
        expect(biggest < 0.35, "the animated gain has no jump with block " +
                                   std::to_string(block) + " (largest step " +
                                   std::to_string(biggest) + " dB per 10 ms)");
        expect(20.0 * std::log10(rms(samples, 38000, 40000) / rms(samples, 0, 480)) > 9.0,
               "the animated gain reaches most of its +12 dB");
    }
}

} // namespace

int main() {
    try {
        chainCompilesInOrder();
        unwiredAndMutedEffects();
        wireOnlyChecks();
        staticChainPlaybackEqualsExport();
        animatedParameterMovesWithoutSteps();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

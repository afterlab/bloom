// #630 audio analysis reads any audio output, through the production path: a document (audio
// layer, effects, an Audio Amplitude node), the real compiler, the CPU evaluator that prepares the
// analysis on its worker, and the sound store that renders the sound through the export's mixer
// code. The fixture is a real file: a 937.5 Hz sine at 0.5 on the left for one second and a
// constant 0.25 on the right for two.

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/platform/private_temporary_directory.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/audio_mix_render.hpp>
#include <bloom/runtime/audio_sound.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include "audio_plot_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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

std::filesystem::path scratch;

struct Fixture {
    audio_plot_test::AudioFixture audio = audio_plot_test::makeAudioFixture(scratch, "tone.wav");
    document::NewProject initial = makeProject(audio.asset);
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    static document::NewProject makeProject(const document::AssetRecord& asset) {
        auto project = document::makeNewProject(
            "Audio sound", "Main", at(2),
            *document::CompositionFormat::create(4, 4, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        if (!project.project.addAsset(asset))
            throw std::runtime_error("fixture assets");
        return project;
    }
    template <typename Operation, typename... Arguments>
    commands::CommandResult attempt(Arguments&&... arguments) {
        commands::Transaction transaction("Audio sound", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return commands.execute(std::move(transaction));
    }
    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        auto result = attempt<Operation>(std::forward<Arguments>(arguments)...);
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
    document::NodeId add(const std::string_view type) {
        const auto before = graph().nodes().size();
        run<commands::AddNode>(composition, std::string(type), document::Vec2d{0.0, 0.0});
        if (graph().nodes().size() != before + 1)
            throw std::runtime_error("node not added");
        document::NodeId added;
        for (const auto& node : graph().nodes())
            if (node.typeId == type && node.id.value() > added.value())
                added = node.id;
        return added;
    }
    // Adds an equalizer between the source and the layer.
    document::NodeId addEqualizer() {
        const auto effect = add(document::kAudioEqualizerNodeType);
        const auto layer = nodeOfType(document::kLayerOutputNodeType);
        const document::InputPortRef layerAudio = document::NodeInputRef{
            layer, std::string(document::kLayerOutputAudioInputPort)};
        const auto fed = std::ranges::find_if(
            graph().edges(), [&](const auto& edge) { return edge.destination == layerAudio; });
        const auto upstream = fed->source;
        run<commands::DisconnectInput>(composition, layerAudio);
        run<commands::ConnectPorts>(composition, upstream, document::NodeInputRef{effect, "audio"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{effect, "audio"},
                                    layerAudio);
        return effect;
    }
    // An Audio Amplitude node that reads `output` of `from`.
    document::NodeId addAmplitude(const document::NodeId from, const std::string_view output) {
        const auto node = add(document::kAudioAmplitudeNodeType);
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{from, std::string(output)},
            document::NodeInputRef{node, std::string(document::kAudioAnalysisInputPort)});
        return node;
    }
    void set(const document::NodeId node, const std::string_view role, const double value) {
        run<commands::SetNodeParameterValue>(composition, node, std::string(role),
                                             document::ParameterValue{value});
    }
};

struct Reading final {
    std::optional<double> value;
    std::shared_ptr<const runtime::AudioAnalysisSource> source;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
};

// The node's amplitude at `time`, as a frame evaluated by the CPU evaluator reads it.
Reading read(const Fixture& fixture, const document::NodeId node, const core::RationalTime time) {
    Reading reading;
    runtime::SnapshotCompileRequest compile{fixture.document.snapshot(), fixture.composition};
    compile.valueReadouts.push_back(node);
    const auto compiled =
        runtime::SnapshotCompiler(document::builtInNodeDefinitions()).compile(compile, {});
    if (!compiled.plan) {
        expect(false, "the composition compiles: " +
                          (compiled.diagnostics.empty() ? std::string("no diagnostic")
                                                        : compiled.diagnostics.front().summary));
        return reading;
    }
    reading.plan = compiled.plan;
    const runtime::CompiledValueOperation* operation = nullptr;
    for (const auto& candidate : compiled.plan->valueOperations())
        if (candidate.sourceNodeId == node)
            operation = &candidate;
    if (operation == nullptr) {
        expect(false, "the amplitude node is in the plan");
        return reading;
    }
    if (const auto* kernel = std::get_if<runtime::CompiledValueAudioAmplitude>(&operation->kernel))
        reading.source = kernel->source;
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(scratch);
    const auto evaluation = evaluator.evaluate(
        compiled.plan,
        {.time = time,
         .output = compiled.plan->output(),
         .resolution = runtime::CompositionFormatResolution{},
         .quality = runtime::EvaluationQuality::Reference,
         .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
         .pixelStorageByteLimit = std::size_t{64} * 1024U * 1024U},
        {});
    if (evaluation.frame() == nullptr) {
        expect(false, "the frame evaluates");
        return reading;
    }
    const auto outputs = evaluation.frame()->valueOutputs();
    if (operation->firstOutput.value() < outputs.size())
        if (const auto* value = std::get_if<double>(&outputs[operation->firstOutput.value()]))
            reading.value = *value;
    return reading;
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
        TaskRequest("audio sound cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(630)}),
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

bool near(const std::optional<double>& value, const double want, const double tolerance = 3e-3) {
    return value && std::fabs(*value - want) <= tolerance;
}
std::string text(const std::optional<double>& value) {
    return value ? std::to_string(*value) : std::string("nothing");
}

void reset() {
    runtime::clearAudioAnalysis();
    runtime::clearAudioSounds();
}

// The RMS over both channels: the sine and the constant in the first second, the constant alone
// in the second.
const double kFirst = std::sqrt((0.125 + 0.0625) / 2.0);
const double kSecond = std::sqrt(0.0625 / 2.0);

void bareSourceReadsItsFile() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto node = fixture.addAmplitude(fixture.nodeOfType(document::kAudioSourceNodeType),
                                           document::kAudioSourceOutputPort);
    const auto reading = read(fixture, node, at(1, 2));
    expect(reading.source && reading.source->bare && reading.source->sound,
           "a source wired straight in is read from its file");
    expect(near(reading.value, kFirst), "the bare source reads " + text(reading.value));
    expect(runtime::audioSoundStatistics().rendered == 0, "no sound is rendered for a bare source");
}

void layerSoundIsRendered() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto node = fixture.addAmplitude(fixture.nodeOfType(document::kLayerOutputNodeType),
                                           document::kLayerOutputAudioOutputPort);
    const auto first = read(fixture, node, at(1, 2));
    expect(first.source && !first.source->bare, "a layer's sound is rendered");
    expect(near(first.value, kFirst), "the layer's sound reads " + text(first.value));
    auto statistics = runtime::audioSoundStatistics();
    expect(statistics.rendered == 1 && statistics.entries == 1 &&
               statistics.scratchBytes == std::uint64_t{2} * runtime::kAudioSoundRate * 2U * 4U,
           "the two seconds were rendered once into the scratch file");
    // Another frame of the same plan's sound, and the same document compiled again, read the
    // sound that is there.
    const auto second = read(fixture, node, at(3, 2));
    expect(near(second.value, kSecond), "the second half reads " + text(second.value));
    statistics = runtime::audioSoundStatistics();
    expect(statistics.rendered == 1 && statistics.reused == 1,
           "the same sound compiled again is found by its identity, not rendered again");
    expect(first.source && second.source &&
               first.source->sound->identity->digest() == second.source->sound->identity->digest() &&
               first.source->sound->identity->digest().size() == 64,
           "the same sound has the same identity");
    const auto activity = runtime::audioSoundActivity();
    expect(activity.phase == runtime::AudioSoundPhase::Idle && activity.queued == 0 &&
               activity.lastFailure.empty(),
           "the store is idle after the frames");
}

void effectsChangeTheResult() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto layer = fixture.nodeOfType(document::kLayerOutputNodeType);
    const auto node = fixture.addAmplitude(layer, document::kLayerOutputAudioOutputPort);
    const auto plain = read(fixture, node, at(3, 2));
    expect(near(plain.value, kSecond), "without an effect the constant reads " + text(plain.value));

    // An equalizer before the node: its high-pass removes the constant, so the second half, which
    // is only the constant, reads as silence.
    const auto equalizer = fixture.addEqualizer();
    fixture.set(equalizer, "highPass", 2000.0);
    const auto filtered = read(fixture, node, at(3, 2));
    expect(filtered.value && *filtered.value < 0.01,
           "an equalizer before the node changes the result: " + text(filtered.value));
    expect(plain.source && filtered.source &&
               plain.source->sound->identity->digest() !=
                   filtered.source->sound->identity->digest(),
           "an effect is another identity");

    // The node may also read the effect's own output.
    const auto direct = fixture.addAmplitude(equalizer, document::kAudioEffectOutputPort);
    const auto tapped = read(fixture, direct, at(3, 2));
    expect(tapped.source && !tapped.source->bare && tapped.value && *tapped.value < 0.01,
           "an effect's output is read through the effect: " + text(tapped.value));

    // A parameter that changes is another sound again: boosting the sine's band raises the first
    // half.
    fixture.set(equalizer, "highPass", 20.0);
    fixture.set(equalizer, "midFrequency", 937.5);
    const auto before = read(fixture, node, at(1, 2));
    fixture.set(equalizer, "midGain", 12.0);
    const auto boosted = read(fixture, node, at(1, 2));
    expect(before.value && boosted.value && *boosted.value > *before.value * 1.5,
           "a boost at the sine's frequency raises the level: " + text(before.value) + " to " +
               text(boosted.value));
    expect(before.source && boosted.source &&
               before.source->sound->identity->digest() !=
                   boosted.source->sound->identity->digest(),
           "a parameter change is another identity");
}

// The rendered sound is the mix the mixer plays: rendering it in blocks, with the mix evaluated at
// every block boundary, gives the samples of one uninterrupted pass over the same clips.
void renderedSoundIsTheMix() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto equalizer = fixture.addEqualizer();
    fixture.set(equalizer, "midFrequency", 937.5);
    fixture.set(equalizer, "midGain", 6.0);
    const auto node = fixture.addAmplitude(fixture.nodeOfType(document::kLayerOutputNodeType),
                                           document::kLayerOutputAudioOutputPort);
    const auto reading = read(fixture, node, at(1, 2));
    const auto sound = reading.source ? runtime::findAudioSound(*reading.source->sound) : nullptr;
    expect(sound != nullptr && sound->frames == std::uint64_t{2} * runtime::kAudioSoundRate,
           "the rendered sound is found without rendering");
    if (!sound)
        return;
    constexpr std::size_t kFrames = 4800;
    std::vector<float> rendered(kFrames * 2);
    const auto got = sound->buffer->stream->readInterleaved(0, rendered);
    expect(got && *got.value() == kFrames, "the scratch file reads back");

    media::audio::AudioDecodeLimits limits;
    limits.sampleBudget = std::uint64_t{media::audio::AudioStream::kWindowFrames} * 2U;
    auto decoded = media::audio::openWindowedAudio(scratch / "tone.wav", limits,
                                                   media::audio::AudioStream::kWindowFrames, {});
    if (!decoded) {
        expect(false, "the fixture decodes");
        return;
    }
    const auto buffer =
        std::make_shared<const media::audio::AudioBuffer>(std::move(*decoded.value()));
    const auto mix = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(
        reading.plan, at(0), {}, &reading.source->sound->mix);
    if (!mix) {
        expect(false, "the analysis mix evaluates");
        return;
    }
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* const sink = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend),
                                               {runtime::kAudioSoundRate, 2, 16384});
    engine.replaceClips(runtime::audioMixClips(
        *mix, nullptr, at(0), std::nullopt,
        [&](const runtime::AudioClipDescription&) { return buffer; }));
    expect(!engine.play(at(0)) && !engine.renderForTesting(kFrames), "the reference pass renders");
    const auto reference = sink->captured();
    double worst = 0.0;
    for (std::size_t index = 0; index < std::min(reference.size(), rendered.size()); ++index)
        worst = std::max(worst, static_cast<double>(std::fabs(reference[index] - rendered[index])));
    expect(reference.size() == rendered.size() && worst < 1e-5,
           "the rendered sound is the mixer's: worst difference " + std::to_string(worst));
}

// No result depends on which frame was asked first.
void seekOrder() {
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto equalizer = fixture.addEqualizer();
    fixture.set(equalizer, "lowGain", 6.0);
    const auto node = fixture.addAmplitude(fixture.nodeOfType(document::kLayerOutputNodeType),
                                           document::kLayerOutputAudioOutputPort);
    reset();
    const auto forwardEarly = read(fixture, node, at(1, 4));
    const auto forwardLate = read(fixture, node, at(7, 4));
    reset();
    const auto backwardLate = read(fixture, node, at(7, 4));
    const auto backwardEarly = read(fixture, node, at(1, 4));
    expect(forwardEarly.value && forwardLate.value && forwardEarly.value == backwardEarly.value &&
               forwardLate.value == backwardLate.value,
           "a frame reads the same value whichever frame was asked first");
}

void unavailableSound() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto node = fixture.addAmplitude(fixture.nodeOfType(document::kLayerOutputNodeType),
                                           document::kLayerOutputAudioOutputPort);
    const auto ready = read(fixture, node, at(1, 2));
    expect(near(ready.value, kFirst), "the sound reads before the file goes away");
    reset();
    std::filesystem::rename(scratch / "tone.wav", scratch / "moved.wav");
    const auto status =
        runtime::prepareAudioSound(ready.plan, ready.source->sound, scratch, {});
    expect(status.outcome == runtime::AudioSoundOutcome::Unavailable &&
               status.detail == "The audio file is missing" && !status.sound,
           "a missing file is reported in words: " + status.detail);
    const auto statistics = runtime::audioSoundStatistics();
    expect(statistics.entries == 0 && statistics.failed == 1, "nothing is kept for a failed render");
    expect(runtime::audioSoundActivity().lastFailure == "The audio file is missing",
           "the store reports its last failure");
    expect(!runtime::findAudioSound(*ready.source->sound), "a failed sound is not found");
    const auto silent = read(fixture, node, at(1, 2));
    expect(silent.value && *silent.value == 0.0, "an unavailable sound reads as silence");
    std::filesystem::rename(scratch / "moved.wav", scratch / "tone.wav");
    const auto again = read(fixture, node, at(1, 2));
    expect(near(again.value, kFirst), "the sound reads again once the file is back");
    expect(runtime::audioSoundActivity().lastFailure.empty(), "a success clears the last failure");
}

// The waveform and the spectrum of a rendered sound are those of the source it plays unchanged.
void plotsReadTheRenderedSound() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto direct = fixture.addAmplitude(fixture.nodeOfType(document::kAudioSourceNodeType),
                                             document::kAudioSourceOutputPort);
    const auto layered = fixture.addAmplitude(fixture.nodeOfType(document::kLayerOutputNodeType),
                                              document::kLayerOutputAudioOutputPort);
    const auto bare = read(fixture, direct, at(1, 2));
    const auto rendered = read(fixture, layered, at(1, 2));
    if (!bare.source || !rendered.source) {
        expect(false, "both sources compile");
        return;
    }
    for (const auto kind :
         {runtime::AudioAnalysisKind::Waveform, runtime::AudioAnalysisKind::Spectrum}) {
        runtime::AudioAnalysisQuery query;
        query.kind = kind;
        query.time = at(1, 2);
        query.windowSeconds = 0.1;
        query.columns = 64;
        query.separateChannels = true;
        query.source = bare.source;
        const auto bareStatus = runtime::prepareAudioAnalysis(query, scratch, {}, bare.plan);
        const auto want = runtime::findAudioAnalysis(query);
        query.source = rendered.source;
        const auto status = runtime::prepareAudioAnalysis(query, scratch, {}, rendered.plan);
        const auto got = runtime::findAudioAnalysis(query);
        expect(bareStatus.outcome == runtime::AudioAnalysisOutcome::Ready &&
                   status.outcome == runtime::AudioAnalysisOutcome::Ready && want && got,
               "both plots prepare: " + status.detail);
        if (!want || !got)
            continue;
        double worst = 0.0;
        for (std::size_t index = 0; index < std::min(want->values.size(), got->values.size());
             ++index)
            worst = std::max(
                worst, static_cast<double>(std::fabs(want->values[index] - got->values[index])));
        expect(want->values.size() == got->values.size() && want->lanes == got->lanes &&
                   worst < 2e-3,
               std::string(kind == runtime::AudioAnalysisKind::Waveform ? "waveform" : "spectrum") +
                   " of the rendered sound is the source's: worst difference " +
                   std::to_string(worst));
    }
}

// A frame that is already cancelled starts nothing and keeps nothing.
void cancelledFrame() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto node = fixture.addAmplitude(fixture.nodeOfType(document::kLayerOutputNodeType),
                                           document::kLayerOutputAudioOutputPort);
    const auto ready = read(fixture, node, at(1, 2));
    reset();
    const auto status =
        runtime::prepareAudioSound(ready.plan, ready.source->sound, scratch, cancelledToken());
    expect(status.outcome == runtime::AudioSoundOutcome::Cancelled && !status.sound,
           "a cancelled frame is told so");
    const auto statistics = runtime::audioSoundStatistics();
    expect(statistics.entries == 0 && statistics.rendered == 0 && statistics.failed == 0,
           "a cancelled frame renders nothing and is not a failure");
    const auto after = runtime::prepareAudioSound(ready.plan, ready.source->sound, scratch, {});
    expect(after.outcome == runtime::AudioSoundOutcome::Ready && after.sound,
           "the next frame renders the sound");
}

// A sound cannot be driven by its own analysis: the wire that would close the loop is refused.
void ownAnalysisIsRefused() {
    reset();
    Fixture fixture;
    fixture.run<commands::AddAudioLayer>(fixture.composition, fixture.audio.asset.id);
    const auto equalizer = fixture.addEqualizer();
    const auto node = fixture.addAmplitude(fixture.nodeOfType(document::kLayerOutputNodeType),
                                           document::kLayerOutputAudioOutputPort);
    const auto result = fixture.attempt<commands::ConnectPorts>(
        fixture.composition,
        document::OutputPortRef{node, std::string(document::kAudioAmplitudeOutputPort)},
        document::NodeInputRef{equalizer, "midGain"});
    expect(!result.changed(), "an effect cannot be driven by the analysis of its own sound");
}
} // namespace

int main() {
    const auto directory = platform::createPrivateTemporaryDirectory("bloom-audio-sound");
    if (!directory) {
        std::cerr << "no scratch directory\n";
        return 1;
    }
    scratch = *directory;
    runtime::setAudioSoundDirectory(scratch / "sounds");
    try {
        bareSourceReadsItsFile();
        layerSoundIsRendered();
        effectsChangeTheResult();
        renderedSoundIsTheMix();
        seekOrder();
        unavailableSound();
        plotsReadTheRenderedSound();
        cancelledFrame();
        ownAnalysisIsRefused();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    reset();
    runtime::setAudioSoundDirectory(std::nullopt);
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}

// ADR 0024 layer-owned time: the mapping core, the compiler's clock placement, and the CPU
// evaluator's per-clock operands, on a real compiled document. Content is a Solid whose width is
// animated 1 -> 7 over [0, 2) s, so the covered width of a row reads back the content time.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
core::RationalTime at(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value)
        throw std::logic_error("invalid fixture time");
    return *value;
}
document::FrameRate rate(const std::uint32_t fps) { return *document::FrameRate::create(fps, 1); }

void mappingCore() {
    using document::LayerTimeMode;
    const runtime::TimeExtent extent{at(1), rate(4)}; // frames at 0, 1/4, 1/2, 3/4
    const auto wrap = [&](const core::RationalTime time, const LayerTimeMode before,
                          const LayerTimeMode after) {
        return runtime::wrapToExtent(time, extent, before, after);
    };
    expect(!wrap(at(-1, 4), LayerTimeMode::None, LayerTimeMode::None)->present,
           "None before the extent is absent");
    expect(!wrap(at(1), LayerTimeMode::None, LayerTimeMode::None)->present,
           "None at the extent end is absent");
    expect(wrap(at(1, 2), LayerTimeMode::None, LayerTimeMode::None)->time == at(1, 2),
           "times inside the extent pass through");
    expect(wrap(at(-1, 4), LayerTimeMode::Hold, LayerTimeMode::None)->time == at(0),
           "Hold before the extent holds the first frame");
    expect(wrap(at(3), LayerTimeMode::None, LayerTimeMode::Hold)->time == at(3, 4),
           "Hold after the extent holds the last frame start");
    expect(wrap(at(5, 4), LayerTimeMode::None, LayerTimeMode::Loop)->time == at(1, 4),
           "Loop wraps at the extent duration");
    expect(wrap(at(-1, 4), LayerTimeMode::Loop, LayerTimeMode::None)->time == at(3, 4),
           "Loop before the extent wraps to a non-negative time");
    expect(wrap(at(1), LayerTimeMode::None, LayerTimeMode::PingPong)->time == at(1, 2),
           "PingPong reflects around the last frame start");
    expect(wrap(at(-1, 4), LayerTimeMode::PingPong, LayerTimeMode::None)->time == at(1, 4),
           "PingPong reflects before the extent too");
    expect(runtime::mapLayerTime(at(3), at(1), *document::LayerSpeed::create(1, 2)) == at(1),
           "layer time is (t - start) * speed exactly");
    expect(runtime::secondsToTime(0.25) == at(1, 4), "binary seconds convert exactly");
    // mapCompositionTime keeps its historical rule through the shared core.
    for (const std::int64_t loop : {0, 1, 2})
        expect(runtime::mapCompositionTime(at(-1), 0, 1, at(1), rate(4), loop) == at(0),
               "composition sources hold their first frame before zero");
    expect(runtime::mapCompositionTime(at(5, 4), 0, 1, at(1), rate(4), 1) == at(1, 4),
           "composition source Loop is unchanged");
    expect(runtime::mapCompositionTime(at(7, 8), 0, 1, at(1), rate(4), 0) == at(3, 4),
           "composition source Hold clamps inside the last frame as before");
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Layer time", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(16, 4, core::PixelAspectRatio::square(), rate(24)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::LayerId layer;
    document::NodeId solid;
    runtime::CpuCompositionEvaluator evaluator;

    Fixture() {
        commands::Transaction add("Add solid", document.snapshot().revision());
        add.emplace<commands::AddSolidLayer>(initial.initialCompositionId, "Solid",
                                             core::Color4d{1, 1, 1, 1},
                                             document::Vec2d{8, 2});
        const auto result = commands.execute(std::move(add));
        if (!result.changed())
            throw std::runtime_error("solid creation failed");
        layer = *result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        solid = *result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto width = parameter("width");
        commands::Transaction animate("Animate width", document.snapshot().revision());
        animate.emplace<commands::SetParameterSource>(initial.initialCompositionId, width,
                                                      document::ConstantValueSource{1.0});
        animate.emplace<commands::CreateAnimationForParameter>(initial.initialCompositionId,
                                                               width, at(0));
        animate.emplace<commands::SetKeyframeAtTimeForParameter>(initial.initialCompositionId,
                                                                 width, at(2), 13.0);
        if (!commands.execute(std::move(animate)).succeeded())
            throw std::runtime_error("width animation failed");
    }
    document::ParameterId parameter(const std::string_view role) const {
        const auto snapshot = document.snapshot();
        const auto* node = snapshot.project()
                               .findComposition(initial.initialCompositionId)
                               ->graph()
                               .findNode(solid);
        for (const auto& binding : node->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("missing solid parameter");
    }
    void setTiming(const document::LayerTiming& timing, const core::RationalTime in = {},
                   const core::RationalTime out = {}) {
        const auto base = document.snapshot();
        auto draft = document.draft(base);
        auto* boundary =
            draft.project().findComposition(initial.initialCompositionId)->graph().findLayer(layer);
        boundary->timing = timing;
        boundary->inPoint = in;
        boundary->outPoint = out;
        if (!document.commit(base.revision(), std::move(draft)).committed())
            throw std::runtime_error("timing commit refused");
    }
    std::shared_ptr<const runtime::CompiledCompositionPlan> compile() const {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), initial.initialCompositionId}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << '\n';
            throw std::runtime_error("layer time compilation failed");
        }
        return compiled.plan;
    }
    // Opaque pixel count of row 2: the solid's covered width at the content time.
    int covered(const core::RationalTime time) {
        const auto plan = compile();
        const auto result =
            evaluator.evaluate(plan,
                               {time, plan->output(), runtime::CompositionFormatResolution{},
                                runtime::EvaluationQuality::Reference,
                                runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 20U},
                               {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << '\n';
            ++failures;
            return -1;
        }
        const auto pixels = result.frame()->processImage().pixels();
        int count = 0;
        for (std::size_t x = 0; x < 16; ++x)
            if (pixels[2 * 16 + x].alpha() > 0.5F)
                ++count;
        return count;
    }
};

void evaluation() {
    Fixture fixture;
    expect(fixture.compile()->clocks().empty(), "identity timing compiles without clocks");
    const auto identityHalf = fixture.covered(at(1, 2));
    const auto identityOne = fixture.covered(at(1));
    expect(identityHalf > 0 && identityHalf < identityOne, "fixture animation is visible");

    document::LayerTiming moved;
    moved.startTime = at(1, 2);
    fixture.setTiming(moved);
    expect(!fixture.compile()->clocks().empty(), "a moved layer compiles onto layer clocks");
    expect(fixture.covered(at(1)) == identityHalf,
           "moving the start time moves the content and its keys");

    document::LayerTiming fast;
    fast.speed = *document::LayerSpeed::create(2, 1);
    fixture.setTiming(fast);
    expect(fixture.covered(at(1, 2)) == identityOne, "speed 2 plays the content twice as fast");

    document::LayerTiming bounded;
    bounded.contentExtent = document::FixedLayerExtent{at(1, 2), rate(24)};
    bounded.afterMode = document::LayerTimeMode::None;
    fixture.setTiming(bounded);
    expect(fixture.covered(at(1)) == 0, "None after the content extent is transparent");
    bounded.afterMode = document::LayerTimeMode::Loop;
    fixture.setTiming(bounded);
    const auto quarter = fixture.covered(at(1, 4));
    expect(fixture.covered(at(3, 4)) == quarter, "Loop repeats the content extent");
    bounded.afterMode = document::LayerTimeMode::Hold;
    fixture.setTiming(bounded);
    expect(fixture.covered(at(2)) == fixture.covered(at(11, 24)),
           "Hold shows the last content frame");

    // A layer that starts before composition time zero is valid.
    document::LayerTiming early;
    early.startTime = at(-1);
    fixture.setTiming(early, at(-1), at(2));
    expect(fixture.covered(at(0)) == identityOne, "a range may start before composition zero");
}
// Audio follows exactly the picture mapping: a 1 s tone asset placed at start 2 s plays content
// time t - 2 inside its range and is silent after its content extent (after mode None).
void audio() {
    auto initial = document::makeNewProject(
        "Audio time", "Main", core::RationalTime::fromInteger(8),
        *document::CompositionFormat::create(4, 4, core::PixelAspectRatio::square(), rate(24)));
    {
        document::AssetRecord tone;
        tone.id = document::AssetId::fromRaw(9000);
        tone.kind = document::AssetKind::Audio;
        tone.locator = {"file", "project-relative", "tone.wav", "file:tone.wav"};
        tone.rate = 48000;
        tone.channels = 1;
        tone.frames = 48000;
        tone.duration = core::RationalTime::fromInteger(1);
        if (!initial.project.addAsset(tone))
            throw std::runtime_error("audio asset");
    }
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    commands::Transaction add("Add audio", document.snapshot().revision());
    add.emplace<commands::AddAudioLayer>(initial.initialCompositionId,
                                         document::AssetId::fromRaw(9000));
    if (!commands.execute(std::move(add)).changed())
        throw std::runtime_error("audio layer");
    const auto layerId =
        document.snapshot().project().findComposition(initial.initialCompositionId)
            ->graph()
            .layerOutputs()
            .back()
            .layerId;
    {
        const auto* layer = document.snapshot()
                                .project()
                                .findComposition(initial.initialCompositionId)
                                ->graph()
                                .findLayer(layerId);
        expect(layer->endPoint(core::RationalTime::fromInteger(8)) == at(1),
               "a new audio layer ends at its asset length");
    }
    commands::Transaction move("Move", document.snapshot().revision());
    move.emplace<commands::SetLayerTiming>(initial.initialCompositionId, layerId, at(2), at(2),
                                           at(5));
    if (!commands.execute(std::move(move)).changed())
        throw std::runtime_error("audio move");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), initial.initialCompositionId}, {});
    if (!compiled.plan)
        throw std::runtime_error("audio compile");
    const auto mix = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(compiled.plan, at(0));
    expect(mix && mix->clips.size() == 1 && !mix->clips.front().timeMappings.empty(),
           "a retimed audio layer carries its layer mapping");
    if (!mix || mix->clips.empty())
        return;
    const auto& clip = mix->clips.front();
    expect(!runtime::mapAudioClipTime(clip, at(19, 10)), "silent before the layer range");
    expect(runtime::mapAudioClipTime(clip, at(5, 2)) == at(1, 2),
           "inside the range the content time is t - start");
    expect(!runtime::mapAudioClipTime(clip, at(7, 2)),
           "silent after the content extent (after mode None), though the range continues");
}
// An identity Time Remap (two Linear keys over the range) shows exactly the same frames: its
// binary64 seconds are snapped onto frame starts within a nanosecond.
void identityRemap() {
    Fixture fixture;
    std::vector<int> plain;
    for (std::int64_t frame = 0; frame < 48; ++frame)
        plain.push_back(fixture.covered(at(frame, 24)));
    commands::Transaction enable("Remap", fixture.document.snapshot().revision());
    enable.emplace<commands::EnableLayerTimeRemap>(fixture.initial.initialCompositionId,
                                                   fixture.layer);
    expect(fixture.commands.execute(std::move(enable)).changed(), "Time Remap enables");
    bool same = true;
    for (std::int64_t frame = 0; frame < 48; ++frame)
        same = same && fixture.covered(at(frame, 24)) == plain[static_cast<std::size_t>(frame)];
    expect(same, "an identity Time Remap selects exactly the same frames");
    commands::Transaction disable("Remap", fixture.document.snapshot().revision());
    disable.emplace<commands::DisableLayerTimeRemap>(fixture.initial.initialCompositionId,
                                                     fixture.layer);
    expect(fixture.commands.execute(std::move(disable)).changed(), "Time Remap disables");
    expect(fixture.covered(at(30, 24)) == plain[30], "disabling restores plain playback");
}
template <typename Operation, typename... Arguments>
bool execute(document::Document& document, commands::CommandStack& commands,
             Arguments&&... arguments) {
    commands::Transaction transaction("Disabled remap", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    return commands.execute(std::move(transaction)).changed();
}

// #431: disabling a Time Remapped layer excludes it from evaluation instead of failing the
// compile, whether the layer carries pictures, audio, or a video's picture and audio, and enabling
// it again brings it back. The remap is animated, so its curve is compiled only while the layer
// contributes; the audio mix used to lower the disabled layer's remap anyway.
void disabledRemappedLayers() {
    auto initial = document::makeNewProject(
        "Disabled remap", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(16, 4, core::PixelAspectRatio::square(), rate(24)));
    {
        document::AssetRecord tone;
        tone.id = document::AssetId::fromRaw(9000);
        tone.kind = document::AssetKind::Audio;
        tone.locator = {"file", "project-relative", "tone.wav", "file:tone.wav"};
        tone.rate = 48000;
        tone.channels = 1;
        tone.frames = 96000;
        tone.duration = at(2);
        document::AssetRecord video;
        video.id = document::AssetId::fromRaw(9001);
        video.kind = document::AssetKind::Video;
        video.locator = {"file", "project-relative", "clip.mov", "file:clip.mov"};
        video.width = video.height = 4;
        video.rate = 48000;
        video.channels = 1;
        video.frames = 48;
        video.duration = at(2);
        document::AssetVideoStream picture;
        picture.codec = "raw";
        picture.timebase = picture.framePeriod = at(1, 24);
        picture.duration = at(2);
        video.videoStreams.push_back(picture);
        document::AssetVideoStream sound;
        sound.id = 1;
        sound.kind = 2;
        sound.codec = "pcm";
        sound.timebase = sound.framePeriod = at(1, 48000);
        sound.duration = at(2);
        sound.sampleRate = 48000;
        sound.channelLayout = {"M"};
        video.videoStreams.push_back(sound);
        if (!initial.project.addAsset(tone) || !initial.project.addAsset(video))
            throw std::runtime_error("disabled remap assets");
    }
    const auto composition = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    const auto run = [&](const bool changed) {
        if (!changed)
            throw std::runtime_error("disabled remap command refused");
    };
    run(execute<commands::AddSolidLayer>(document, commands, composition, "Solid",
                                         core::Color4d{1, 1, 1, 1}, document::Vec2d{8, 2}));
    run(execute<commands::AddAudioLayer>(document, commands, composition,
                                         document::AssetId::fromRaw(9000)));
    run(execute<commands::AddImageLayer>(document, commands, composition,
                                         document::AssetId::fromRaw(9001)));
    // A nested-composition layer routes its composition's audio as well as its picture.
    run(execute<commands::AddComposition>(
        document, commands, "Inner",
        *document::CompositionFormat::create(16, 4, core::PixelAspectRatio::square(), rate(24)),
        core::RationalTime::fromInteger(4)));
    const auto inner = document.snapshot().project().compositions().back().id();
    run(execute<commands::AddCompositionSource>(document, commands, composition, inner,
                                                document::Vec2d{}, true));
    std::vector<document::LayerId> layers;
    for (const auto& boundary :
         document.snapshot().project().findComposition(composition)->graph().layerOutputs())
        layers.push_back(boundary.layerId);
    expect(layers.size() == 4, "the fixture has a solid, an audio, a video and a nested layer");
    // Every layer but the audio-only one carries a Time Remap.
    for (const std::size_t index : {0U, 2U, 3U})
        run(execute<commands::EnableLayerTimeRemap>(document, commands, composition,
                                                    layers[index]));
    const auto compile = [&] {
        return runtime::SnapshotCompiler(document::builtInNodeDefinitions())
            .compile({document.snapshot(), composition}, {});
    };
    // Whether the layer at `index` is in the mix: the nested layer is the only nested entry.
    const auto mixes = [&](const runtime::SnapshotCompileResult& result, const std::size_t index) {
        if (!result.plan)
            return false;
        const auto& mix = result.plan->audioMix();
        if (index == 3)
            return mix.nestedLayers.size() == 1;
        return std::ranges::any_of(
            mix.layers, [&](const auto& audio) { return audio.layerId == layers[index]; });
    };
    const auto enabled = compile();
    expect(enabled.status == runtime::SnapshotCompileStatus::Compiled,
           "the Time Remapped layers compile");
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto layer = layers[index];
        const bool audible = index > 0;
        expect(mixes(enabled, index) == audible, "an enabled layer with audio is in the mix");
        run(execute<commands::SetLayerEnabled>(document, commands, composition, layer, false));
        const auto disabled = compile();
        expect(disabled.status == runtime::SnapshotCompileStatus::Compiled,
               "a disabled layer compiles, Time Remapped or not");
        expect(!mixes(disabled, index), "a disabled layer is excluded from the audio mix");
        run(execute<commands::SetLayerEnabled>(document, commands, composition, layer, true));
        const auto again = compile();
        expect(again.status == runtime::SnapshotCompileStatus::Compiled && again.plan &&
                   again.plan->operations().size() == enabled.plan->operations().size() &&
                   mixes(again, index) == audible,
               "enabling the layer again restores its picture and audio");
    }

    // The picture: a disabled remapped Solid renders nothing and enabling it renders it again.
    Fixture fixture;
    const auto solidComposition = fixture.initial.initialCompositionId;
    expect(execute<commands::EnableLayerTimeRemap>(fixture.document, fixture.commands,
                                                   solidComposition, fixture.layer),
           "the Solid's Time Remap enables");
    const auto shown = fixture.covered(at(1, 2));
    expect(shown > 0, "the remapped Solid is visible");
    expect(execute<commands::SetLayerEnabled>(fixture.document, fixture.commands, solidComposition,
                                              fixture.layer, false),
           "the Solid disables");
    expect(fixture.covered(at(1, 2)) == 0, "a disabled remapped Solid renders nothing");
    expect(execute<commands::SetLayerEnabled>(fixture.document, fixture.commands, solidComposition,
                                              fixture.layer, true),
           "the Solid enables again");
    expect(fixture.covered(at(1, 2)) == shown, "the re-enabled remapped Solid renders again");
}
// #431: a real compile failure names its subject in the description an export failure carries.
// A stale Time Remap override (a parameter-only subject) names the composition, the layer whose
// Layer Output binds it and the parameter; an unknown node type names the node.
void compileFailureNamesItsSubject() {
    Fixture fixture;
    const auto composition = fixture.initial.initialCompositionId;
    expect(execute<commands::EnableLayerTimeRemap>(fixture.document, fixture.commands, composition,
                                                   fixture.layer),
           "the Solid's Time Remap enables");
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto snapshot = fixture.document.snapshot();
    const auto& graph = snapshot.project().findComposition(composition)->graph();
    const auto& bindings = graph.findNode(graph.findLayer(fixture.layer)->nodeId)->parameters;
    const auto remap = std::ranges::find(bindings, document::kTimeRemapParameterRole,
                                         &document::ParameterBinding::role);
    expect(remap != bindings.end(), "the Solid has a Time Remap parameter");
    if (remap == bindings.end())
        return;
    runtime::SnapshotCompileRequest stale{snapshot, composition};
    stale.parameterOverrides.push_back({document::Revision{}, remap->parameterId, 1.0});
    const auto overridden = compiler.compile(stale, {});
    const auto overrideText =
        runtime::describeCompileFailure(overridden, snapshot.project(), composition);
    expect(overridden.status == runtime::SnapshotCompileStatus::Failed &&
               overrideText.starts_with(
                   R"(Composition "Main", layer "Solid", parameter "timeRemap": )"),
           ("a parameter failure names its composition, layer and parameter: " + overrideText)
               .c_str());

    const auto base = fixture.document.snapshot();
    auto draft = fixture.document.draft(base);
    draft.project().findComposition(composition)->graph().findNode(fixture.solid)->typeId =
        "example.unknown";
    expect(fixture.document.commit(base.revision(), std::move(draft)).committed(),
           "the document holds an unknown node type");
    snapshot = fixture.document.snapshot();
    const auto unknown = compiler.compile({snapshot, composition}, {});
    const auto unknownText =
        runtime::describeCompileFailure(unknown, snapshot.project(), composition);
    expect(unknown.status != runtime::SnapshotCompileStatus::Compiled &&
               unknownText.starts_with(R"(Composition "Main", node )" +
                                       std::to_string(fixture.solid.value()) +
                                       " (example.unknown): "),
           ("a node failure names its composition and node: " + unknownText).c_str());
    expect(runtime::describeCompileFailure(
               runtime::SnapshotCompileResult{runtime::SnapshotCompileStatus::Compiled, {}, {}},
               snapshot.project(), composition)
               .empty(),
           "a compiled result has no failure to describe");
}
} // namespace

int main() {
    try {
        mappingCore();
        evaluation();
        audio();
        identityRemap();
        disabledRemappedLayers();
        compileFailureNamesItsSubject();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}

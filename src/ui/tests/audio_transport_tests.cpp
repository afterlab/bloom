// End-to-end audio transport check: an imported WAV placed as an audio layer must reach the
// playback engine through the same wiring apps/bloom/main.cpp uses (asset decode -> compiled
// audio mix -> AudioPlaybackSession -> PlaybackController -> AudioEngine) and produce non-silent
// samples when the transport plays.
#include "await_qt.hpp"
#include "window_fixture.hpp"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_stack_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/audio_device_control.hpp>
#include <bloom/ui/audio_playback_session.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace bloom;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void writeTone(const QString& path) {
    QByteArray wave;
    const auto appendLe16 = [&wave](const std::uint16_t value) {
        wave.append(static_cast<char>(value & 0xffU));
        wave.append(static_cast<char>((value >> 8U) & 0xffU));
    };
    const auto appendLe32 = [&wave](const std::uint32_t value) {
        wave.append(static_cast<char>(value & 0xffU));
        wave.append(static_cast<char>((value >> 8U) & 0xffU));
        wave.append(static_cast<char>((value >> 16U) & 0xffU));
        wave.append(static_cast<char>((value >> 24U) & 0xffU));
    };
    const std::uint32_t sampleCount = 48'000;
    const std::uint32_t dataBytes = sampleCount * static_cast<std::uint32_t>(sizeof(std::int16_t));
    wave.append("RIFF", 4);
    appendLe32(36U + dataBytes);
    wave.append("WAVEfmt ", 8);
    appendLe32(16);
    appendLe16(1);
    appendLe16(1);
    appendLe32(48'000);
    appendLe32(48'000U * static_cast<std::uint32_t>(sizeof(std::int16_t)));
    appendLe16(static_cast<std::uint16_t>(sizeof(std::int16_t)));
    appendLe16(16);
    wave.append("data", 4);
    appendLe32(dataBytes);
    for (std::uint32_t index = 0; index < sampleCount; ++index) {
        const auto sample = std::sin(2.0 * std::numbers::pi * 440.0 * index / 48'000.0);
        appendLe16(static_cast<std::uint16_t>(static_cast<std::int16_t>(sample * 12'000.0)));
    }
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(wave) == wave.size(), "tone fixture");
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomAudioTests");
    app.setApplicationName("AudioTransport");
    ui::kit::installAfterlabTheme(app);
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    writeTone(directory.filePath("Tone.wav"));

    ui::test::WindowFixture fixture(
        [&](auto& target) {
            commands::Transaction setup("Audio composition", target.session.snapshot().revision());
            setup.emplace<commands::SetCompositionDuration>(target.session.compositionId(),
                                                            core::RationalTime::fromInteger(2));
            require(target.session.executeTransaction(std::move(setup)).succeeded(), "setup");
            target.assets->importFiles({directory.filePath("Tone.wav")});
            require(bloom::test_support::awaitQtCondition("WAV import finishes",
                                                          [&] { return !target.assets->busy(); }),
                    "WAV import finishes");
            require(target.session.snapshot().project().assets().size() == 1, "WAV import");
            const auto asset = target.session.snapshot().project().assets().front();
            commands::Transaction add("Place audio", target.session.snapshot().revision());
            add.emplace<commands::AddAudioLayer>(target.session.compositionId(), asset.id);
            require(target.session.executeTransaction(std::move(add)).succeeded(), "audio layer");
        },
        128);
    const auto assetId = fixture.session.snapshot().project().assets().front().id;
    require(bloom::test_support::awaitQtCondition(
                "decoded audio buffer is delivered",
                [&] { return fixture.assets->audioBuffer(assetId) != nullptr; }),
            "stage 1: decoded audio buffer");
    require(fixture.assets->audioBuffer(assetId) != nullptr, "stage 1: decoded audio buffer");
    require(fixture.assets->decodedAudioBytes() == 0,
            "oversized-for-cache buffer stays in playback delivery, not retention cache");
    const std::weak_ptr<const media::audio::AudioBuffer> delivered =
        fixture.assets->audioBuffer(assetId);
    std::cout << "stage 1 ok: buffer frames " << fixture.assets->audioBuffer(assetId)->frames
              << '\n';

    // Same wiring as apps/bloom/main.cpp.
    auto backendOwner = std::make_unique<media::audio::playback::NullBackend>();
    auto* backend = backendOwner.get();
    auto engineOwner =
        std::make_unique<media::audio::playback::AudioEngine>(std::move(backendOwner));
    auto* engine = engineOwner.get();
    ui::AudioDeviceControl audioDeviceControl(fixture.scheduler, fixture.bridge,
                                              std::move(engineOwner));
    ui::PlaybackController playback(
        fixture.session, *fixture.preview, [] { return std::chrono::steady_clock::now(); }, 16ms);
    playback.setAudioDeviceControl(&audioDeviceControl);
    ui::AudioPlaybackSession audioPlaybackSession(
        fixture.session, fixture.scheduler, fixture.bridge, fixture.compiler,
        std::make_shared<ui::CompiledPlanCache>(), fixture.evaluator);
    audioPlaybackSession.setAudioBufferSource(*fixture.assets);
    const auto awaitMix = [&] {
        const bool published = bloom::test_support::awaitQtCondition(
            "audio session publishes a mix for the live revision", [&] {
                return audioPlaybackSession.mix() &&
                       audioPlaybackSession.revision() == fixture.session.snapshot().revision();
            });
        return published && audioPlaybackSession.mix().has_value();
    };
    std::size_t appliedClips = 0;
    const auto applyAudioMix = [&] {
        const auto& mix = audioPlaybackSession.mix();
        if (!mix.has_value()) {
            appliedClips = 0;
            playback.setAudioMix({}, {});
            return;
        }
        auto clips = audioPlaybackSession.engineClips();
        appliedClips = clips.size();
        playback.setAudioMix(*mix, std::move(clips));
    };
    QObject::connect(&audioPlaybackSession, &ui::AudioPlaybackSession::mixChanged, &playback,
                     applyAudioMix);
    require(audioPlaybackSession.refresh(), "stage 2: mix derived from the live revision");
    require(awaitMix(), "stage 2: worker-published live mix");
    const auto& mix = audioPlaybackSession.mix();
    if (!mix.has_value())
        throw std::runtime_error("stage 3: published mix");
    std::cout << "stage 3 ok: mix clips " << mix->clips.size() << " applied clips " << appliedClips
              << '\n';
    require(!mix->clips.empty(), "stage 3: mix has clips");
    require(appliedClips == 1, "stage 4: clip applied to the engine");
    require(audioPlaybackSession.audioBuffer(assetId) == delivered.lock() &&
                fixture.assets->decodedAudioBytes() == 0,
            "active mix pins admitted audio outside the controller retention cache");

    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(0));
    playback.play();
    require(playback.state() == ui::PlaybackState::Playing, "stage 5: transport playing");
    require(bloom::test_support::awaitQtCondition("audio clock becomes active",
                                                  [&] { return playback.audioClockActive(); }),
            "stage 5: audio clock active");
    require(engine->isPlaying() && playback.audioClockActive(),
            "stage 5: worker-started engine playing");
    require(!engine->renderForTesting(4096).has_value(), "stage 6: render");
    float peak = 0.0F;
    for (const float sample : backend->captured())
        peak = std::max(peak, std::abs(sample));
    std::cout << "stage 6: captured " << backend->captured().size() << " samples, peak " << peak
              << '\n';
    require(peak > 0.1F, "stage 6: audible output");
    playback.pause();

    // Regression: a layout-only edit changes the revision without changing any pixel. The mix
    // must survive it -- the live revision, not a shown frame, is what audio follows.
    {
        std::map<document::NodeId, document::Vec2d> positions;
        for (const auto& node : fixture.session.composition()->graph().nodes())
            positions.emplace(node.id, document::Vec2d{40.0, 40.0});
        const auto before = audioPlaybackSession.work();
        commands::Transaction move("Move nodes", fixture.session.snapshot().revision());
        move.emplace<commands::MoveNodes>(fixture.session.compositionId(), std::move(positions));
        require(fixture.session.executeTransaction(std::move(move)).succeeded(), "move nodes");
        require(awaitMix(), "stage 7: the mix follows the layout-only revision");
        const auto& moved = audioPlaybackSession.mix();
        require(moved.has_value() && !moved->clips.empty(),
                "stage 7: mix survives a layout-only edit");
        require(appliedClips == 1, "stage 7: engine still holds the clip");
        // #358: a render-neutral edit neither remixes nor silences.
        const auto after = audioPlaybackSession.work();
        require(after.mixRequests == before.mixRequests && after.silences == before.silences &&
                    after.neutralEdits == before.neutralEdits + 1,
                "stage 7: a layout-only edit requests no remix and never silences");

        // An unconnected node is render-neutral too: no remix, no dropout.
        commands::Transaction loose("Loose node", fixture.session.snapshot().revision());
        loose.emplace<commands::AddNode>(fixture.session.compositionId(),
                                         std::string(document::kSolidSourceNodeType),
                                         document::Vec2d{0.0, 0.0});
        require(fixture.session.executeTransaction(std::move(loose)).changed(), "loose node");
        require(awaitMix(), "stage 7b: the mix follows the neutral revision");
        const auto unrelated = audioPlaybackSession.work();
        require(unrelated.mixRequests == before.mixRequests &&
                    unrelated.silences == before.silences && appliedClips == 1,
                "stage 7b: an unconnected node requests no remix and keeps the clip playing");
    }

    // #471: mute and solo are followed while the transport plays, and the mix playback holds is
    // the one export evaluates. Neither switch can be keyed (they are layer switches, unlike the
    // level), so what changes over time is the edit arriving during playback: each one reaches
    // the playing transport's clips without a stop, and an engine playing those clips is silent,
    // one tone or two tones accordingly.
    {
        const auto composition = fixture.session.compositionId();
        const auto layerIds = [&] {
            std::vector<document::LayerId> ids;
            for (const auto& entry : fixture.session.composition()->graph().layerStack().entries())
                ids.push_back(entry.layerId);
            return ids;
        };
        // The clips the export's mix would play: the compiled live revision, evaluated at zero.
        const auto exportAudible = [&] {
            const auto compiled = runtime::SnapshotCompiler(document::builtInNodeDefinitions())
                                      .compile({fixture.session.snapshot(), composition}, {});
            require(compiled.plan != nullptr, "stage 7c: the revision compiles");
            const auto mix =
                runtime::CpuCompositionEvaluator{}.evaluateAudioMix(compiled.plan, {});
            require(mix.has_value(), "stage 7c: the export mix evaluates");
            std::vector<std::pair<std::uint64_t, double>> audible;
            for (const auto& clip : mix->clips)
                if (!clip.muted)
                    audible.emplace_back(clip.sourceNodeId.value(), clip.level);
            std::ranges::sort(audible);
            return audible;
        };
        const auto playbackAudible = [&] {
            std::vector<std::pair<std::uint64_t, double>> audible;
            for (const auto& clip : audioPlaybackSession.mix()->clips)
                if (!clip.muted)
                    audible.emplace_back(clip.sourceNodeId.value(), clip.level);
            std::ranges::sort(audible);
            return audible;
        };
        // The peak an engine plays from the clips the transport was given.
        const auto heardPeak = [&] {
            auto owner = std::make_unique<media::audio::playback::NullBackend>();
            auto* sink = owner.get();
            media::audio::playback::AudioEngine heard(std::move(owner));
            heard.replaceClips(audioPlaybackSession.engineClips());
            require(!heard.play(core::RationalTime{}) && !heard.renderForTesting(2400).has_value(),
                    "stage 7c: render");
            float peak = 0.0F;
            for (const float sample : sink->captured())
                peak = std::max(peak, std::abs(sample));
            return peak;
        };
        const auto edit = [&](auto&& build, const char* label) {
            commands::Transaction transaction(label, fixture.session.snapshot().revision());
            build(transaction);
            require(fixture.session.executeTransaction(std::move(transaction)).succeeded(), label);
            // The session keeps the previous mix audible until the worker publishes the new one,
            // so wait for the published mix to describe this revision's audible clips.
            const auto expected = exportAudible();
            QElapsedTimer timer;
            timer.start();
            // ... and for the engine clips of every audible one (a new layer's audio is
            // delivered after its mix).
            const auto delivered = [&] {
                const auto clips = audioPlaybackSession.engineClips();
                return static_cast<std::size_t>(std::ranges::count_if(
                           clips, [](const auto& clip) { return !clip.muted; })) == expected.size();
            };
            const auto published = [&] {
                return audioPlaybackSession.mix().has_value() && playbackAudible() == expected &&
                       delivered();
            };
            require(bloom::test_support::awaitQtCondition("stage 7c: playback publishes the mix export evaluates",
                                     published),
                    "stage 7c: playback publishes the mix export evaluates");
            require(playback.state() == ui::PlaybackState::Playing,
                    "stage 7c: the edit does not stop playback");
        };
        (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(0));
        playback.play();
        require(playback.state() == ui::PlaybackState::Playing, "stage 7c: transport playing");
        const auto first = layerIds().front();
        const float one = heardPeak();
        require(one > 0.3F && playbackAudible().size() == 1, "stage 7c: one tone plays");

        edit([&](auto& t) { t.template emplace<commands::SetLayerEnabled>(composition, first, false); },
             "stage 7c: mute the layer");
        require(playbackAudible().empty() && heardPeak() == 0.0F,
                "stage 7c: a layer muted during playback is silent");
        edit([&](auto& t) { t.template emplace<commands::SetLayerEnabled>(composition, first, true); },
             "stage 7c: unmute the layer");
        require(std::abs(heardPeak() - one) < 1e-4F, "stage 7c: the unmuted layer plays again");

        edit([&](auto& t) { t.template emplace<commands::AddAudioLayer>(composition, assetId); },
             "stage 7c: a second audio layer");
        const auto ids = layerIds();
        require(ids.size() == 2, "stage 7c: two audio layers");
        const auto second = ids.front() == first ? ids.back() : ids.front();
        const float two = heardPeak();
        require(playbackAudible().size() == 2 && two > one * 1.8F,
                "stage 7c: two layers of the same tone play together");
        edit([&](auto& t) { t.template emplace<commands::SetLayerSolo>(composition, second, true); },
             "stage 7c: solo the second layer");
        require(playbackAudible().size() == 1 && std::abs(heardPeak() - one) < 1e-4F,
                "stage 7c: a solo during playback silences the other layer");
        edit([&](auto& t) { t.template emplace<commands::SetLayerSolo>(composition, second, false); },
             "stage 7c: clear the solo");
        require(playbackAudible().size() == 2 && std::abs(heardPeak() - two) < 1e-4F,
                "stage 7c: clearing the solo plays both again");
        // Back to the single layer the stages below expect.
        commands::Transaction remove("Remove the second layer",
                                     fixture.session.snapshot().revision());
        remove.emplace<commands::DeleteLayers>(composition, std::vector<document::LayerId>{second});
        require(fixture.session.executeTransaction(std::move(remove)).succeeded(),
                "stage 7c: the second layer is removed");
        require(bloom::test_support::awaitQtCondition("stage 7c: one layer remains",
                                 [&] {
                                     return audioPlaybackSession.mix().has_value() &&
                                            playbackAudible().size() == 1;
                                 }),
                "stage 7c: one layer remains");
        playback.pause();
        std::cout << "stage 7c ok: peaks " << one << " and " << two << '\n';
    }

    document::NodeId layerNode;
    document::NodeId sourceNode;
    document::NodeId outputNode;
    for (const auto& node : fixture.session.composition()->graph().nodes()) {
        if (node.typeId == document::kAudioSourceNodeType)
            sourceNode = node.id;
        else if (node.typeId == document::kLayerOutputNodeType)
            layerNode = node.id;
    }
    if (const auto& endpoint = fixture.session.composition()->graph().compositionOutput())
        outputNode = endpoint->nodeId;
    require(layerNode.isValid() && sourceNode.isValid() && outputNode.isValid(), "graph nodes");

    // Task FOLLOW-1: hand-wiring an ordinary Layer's own audio output back onto Merge's audio pill
    // -- ConnectPorts' audio-role sentinel, the same path the Merge card's new pill drives --
    // reuses that Layer's existing (image) stack slot rather than opening a second one, and the
    // compiled mix still comes out to exactly one clip. First detach the original AddAudioLayer
    // slot so the count stays at one clip throughout; run before the "direct feeds" stages below
    // rewire the composition's audio input away from the stack entirely.
    {
        const auto merge = fixture.session.composition()->graph().layerStack().nodeId();
        const auto originalBoundaries = fixture.session.composition()->graph().layerOutputs();
        const auto originalBoundary =
            std::ranges::find_if(originalBoundaries, [&](const auto& candidate) {
                return candidate.nodeId == layerNode;
            });
        require(originalBoundary != originalBoundaries.end(), "stage 10: original layer boundary");
        const auto originalEntries = fixture.session.composition()->graph().layerStack().entries();
        const auto originalEntry = std::ranges::find(originalEntries, originalBoundary->layerId,
                                                     &document::LayerStackEntry::layerId);
        require(originalEntry != originalEntries.end(), "stage 10: the audio layer has a slot");
        const auto originalSlotId = originalEntry->slotId;

        commands::Transaction detachAudio("Detach audio", fixture.session.snapshot().revision());
        detachAudio.emplace<commands::DisconnectInput>(
            fixture.session.compositionId(),
            document::LayerStackInputRef{merge, originalSlotId,
                                         std::string(document::kLayerStackAudioInputRole)});
        require(fixture.session.executeTransaction(std::move(detachAudio)).succeeded(),
                "stage 10: detach the audio-only slot's only edge");
        require(fixture.session.composition()->graph().layerStack().find(originalSlotId) == nullptr,
                "stage 10: a slot left with no edge at all is removed");

        commands::Transaction addLayer("Content layer", fixture.session.snapshot().revision());
        addLayer.emplace<commands::AddNode>(fixture.session.compositionId(),
                                            std::string(document::kLayerOutputNodeType),
                                            document::Vec2d{500.0, 500.0});
        const auto addLayerResult = fixture.session.executeTransaction(std::move(addLayer));
        require(addLayerResult.succeeded(), "stage 10: add an ordinary content Layer");
        const auto contentLayerOutput =
            addLayerResult.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!contentLayerOutput.has_value())
            throw std::runtime_error("stage 10: content layer id");
        const document::NodeId contentLayer = *contentLayerOutput;

        commands::Transaction feedAudio("Feed layer audio", fixture.session.snapshot().revision());
        feedAudio.emplace<commands::ConnectPorts>(
            fixture.session.compositionId(),
            document::OutputPortRef{sourceNode, std::string(document::kAudioSourceOutputPort)},
            document::NodeInputRef{contentLayer,
                                   std::string(document::kLayerOutputAudioInputPort)});
        require(fixture.session.executeTransaction(std::move(feedAudio)).succeeded(),
                "stage 10: wire the decoded source into the Layer's own audio input");

        commands::Transaction addContent("Content by hand", fixture.session.snapshot().revision());
        addContent.emplace<commands::ConnectPorts>(
            fixture.session.compositionId(),
            document::OutputPortRef{contentLayer, std::string(document::kLayerOutputOutputPort)},
            document::LayerStackInputRef{merge, document::LayerSlotId{},
                                         std::string(document::kLayerStackContentInputRole)});
        require(fixture.session.executeTransaction(std::move(addContent)).succeeded(),
                "stage 10: hand-wire the Layer's own (empty) image onto Merge");
        const auto boundaries = fixture.session.composition()->graph().layerOutputs();
        const auto boundary = std::ranges::find_if(
            boundaries, [&](const auto& candidate) { return candidate.nodeId == contentLayer; });
        require(boundary != boundaries.end(), "stage 10: content layer boundary");
        const auto entriesAfterContent =
            fixture.session.composition()->graph().layerStack().entries();
        const auto entryAfterContent = std::ranges::find(entriesAfterContent, boundary->layerId,
                                                         &document::LayerStackEntry::layerId);
        require(entryAfterContent != entriesAfterContent.end(), "stage 10: content slot exists");
        const auto newSlotId = entryAfterContent->slotId;

        commands::Transaction addAudio("Audio by hand", fixture.session.snapshot().revision());
        addAudio.emplace<commands::ConnectPorts>(
            fixture.session.compositionId(),
            document::OutputPortRef{contentLayer,
                                    std::string(document::kLayerOutputAudioOutputPort)},
            document::LayerStackInputRef{merge, document::LayerSlotId{},
                                         std::string(document::kLayerStackAudioInputRole)});
        require(fixture.session.executeTransaction(std::move(addAudio)).succeeded(),
                "stage 10: hand-wire the Layer's own audio onto Merge");
        const auto entriesAfterAudio =
            fixture.session.composition()->graph().layerStack().entries();
        require(
            std::ranges::count(entriesAfterAudio, boundary->layerId,
                               &document::LayerStackEntry::layerId) == 1,
            "stage 10: the audio edge reuses the content slot rather than opening a second one");
        const auto entryAfterAudio = std::ranges::find(entriesAfterAudio, boundary->layerId,
                                                       &document::LayerStackEntry::layerId);
        require(entryAfterAudio != entriesAfterAudio.end() && entryAfterAudio->slotId == newSlotId,
                "stage 10: same slot before and after the audio hand-wire");

        require(audioPlaybackSession.refresh(), "stage 10: mix recompiles");
        require(awaitMix(), "stage 10: worker published hand-wired mix");
        const auto& handWired = audioPlaybackSession.mix();
        if (!handWired.has_value())
            throw std::runtime_error("stage 10: hand-wired mix");
        require(handWired->clips.size() == 1,
                "stage 10: hand-wiring through the reused slot still compiles to one clip");
        std::cout << "stage 10 ok: hand-wired reuse clips " << handWired->clips.size() << '\n';
    }

    // Direct feeds: a Layer, and then an Audio source, wired straight into the composition's
    // audio input are one-clip mixes rather than an unsupported preview.
    const auto wire = [&](const document::NodeId from, const std::string_view port,
                          const char* label) {
        commands::Transaction connect(label, fixture.session.snapshot().revision());
        connect.emplace<commands::ConnectPorts>(
            fixture.session.compositionId(), document::OutputPortRef{from, std::string(port)},
            document::NodeInputRef{outputNode,
                                   std::string(document::kCompositionOutputAudioInputPort)});
        require(fixture.session.executeTransaction(std::move(connect)).succeeded(), label);
        require(awaitMix(), label);
        const auto& wired = audioPlaybackSession.mix();
        require(wired.has_value() && wired->clips.size() == 1, label);
    };
    wire(layerNode, document::kLayerOutputAudioOutputPort, "stage 8: layer wired to output");
    wire(sourceNode, document::kAudioSourceOutputPort, "stage 9: source wired to output");
    // #471: playback follows a keyed level over time, as export does. The level is keyed 0 at 0 s
    // to 1 at 0.9 s; the mix evaluated at 0.2 s carries a ramp from that level to the level a
    // twelfth of a second later, and an engine playing it is louder at the end of that span than at
    // its start. Past the last key the level is constant and no ramp is carried.
    {
        std::optional<document::ParameterId> level;
        for (const auto& binding :
             fixture.session.composition()->graph().findNode(sourceNode)->parameters)
            if (binding.role == "level")
                level = binding.parameterId;
        require(level.has_value(), "stage 11: the audio source has a level parameter");
        commands::Transaction automate("Automate level", fixture.session.snapshot().revision());
        automate.emplace<commands::CreateAnimationForParameter>(
            fixture.session.compositionId(), *level, core::RationalTime{});
        automate.emplace<commands::SetKeyframeAtTimeForParameter>(
            fixture.session.compositionId(), *level, core::RationalTime{}, 0.0);
        automate.emplace<commands::SetKeyframeAtTimeForParameter>(
            fixture.session.compositionId(), *level, *core::RationalTime::create(9, 10), 1.0);
        require(fixture.session.executeTransaction(std::move(automate)).succeeded(),
                "stage 11: the level is keyed");
        const auto mixAt = [&](const core::RationalTime time, const bool ramped) {
            (void)fixture.session.setCurrentTime(time);
            const auto ready = [&] {
                const auto& current = audioPlaybackSession.mix();
                if (!current || current->clips.size() != 1 ||
                    audioPlaybackSession.revision() != fixture.session.snapshot().revision())
                    return false;
                const auto& clip = current->clips.front();
                return ramped ? clip.levelRamp && clip.levelRamp->start == time
                              : !clip.levelRamp && clip.level == 1.0;
            };
            require(bloom::test_support::awaitQtCondition("mix follows the playhead", ready),
                    "stage 11: the mix follows the playhead");
        };
        const auto start = *core::RationalTime::create(1, 5);
        mixAt(start, true);
        const auto& clip = audioPlaybackSession.mix()->clips.front();
        require(std::abs(clip.level - 0.2 / 0.9) < 1e-6 &&
                    std::abs(clip.levelRamp->endLevel - (0.2 + 1.0 / 12.0) / 0.9) < 1e-3,
                "stage 11: the clip holds the level at the playhead and ramps to the next");
        auto heardOwner = std::make_unique<media::audio::playback::NullBackend>();
        auto* heardBackend = heardOwner.get();
        media::audio::playback::AudioEngine heard(std::move(heardOwner));
        heard.replaceClips(audioPlaybackSession.engineClips());
        require(!heard.play(start), "stage 11: the engine plays from the playhead");
        constexpr std::size_t kSpan = 4000; // a twelfth of a second at 48 kHz, rounded down
        constexpr std::size_t kWindow = 480;
        require(!heard.renderForTesting(kSpan).has_value(), "stage 11: render");
        const auto captured = heardBackend->captured();
        require(captured.size() == kSpan * 2, "stage 11: stereo samples");
        float first = 0.0F;
        float last = 0.0F;
        for (std::size_t frame = 0; frame < kWindow; ++frame) {
            first = std::max(first, std::abs(captured[frame * 2]));
            last = std::max(last, std::abs(captured[(kSpan - 1 - frame) * 2]));
        }
        std::cout << "stage 11: peak " << first << " -> " << last << '\n';
        require(first > 0.01F && last > first * 1.2F,
                "stage 11: the keyed level rises while it plays");
        mixAt(*core::RationalTime::create(19, 20), false);
    }
    audioPlaybackSession.beginShutdown();
    playback.pause();
    audioDeviceControl.beginShutdown();
    require(bloom::test_support::awaitQtCondition(
                "audio device retirement completes",
                [&] { return audioDeviceControl.retirementComplete(); }),
            "audio device retired before bridge close");
    audioDeviceControl.drainAfterEventLoop();
    require(audioPlaybackSession.audioBuffer(assetId) == nullptr && delivered.expired(),
            "stopping the session and device releases the playback audio lease");
    std::cout << "audio transport ok\n";
    return bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// #718: the routes an artist uses read a source the local mirror keeps exactly once. A three-frame
// image sequence on "slow" storage (the preference stands in for a network share) is shown by
// scrubbing the Viewer, played with the Viewer's own transport, cached by RAM Preview and exported
// as a movie through the export the Export panel runs. Each member is read from its storage once,
// by the first frame that needs it; nothing after that reads it again, and a second set of
// evaluators with empty caches (as after a restart) reads nothing at all.
// Device-free: the CPU reference route. The GPU preparation's reads are covered by
// bloom.runtime.gpu_media_scene_preparation.

#include "await_qt.hpp"
#include "fixed_memory_budget.hpp"

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/media/cache/source_mirror.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/platform/source_store.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;
void expect(const bool ok, const std::string& message) {
    if (ok)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 36;
constexpr std::uint64_t kMembers = 3;

struct Rig final {
    runtime::TaskScheduler scheduler;
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    std::unique_ptr<ui::TaskUiBridge> bridge;
};

struct Counts final {
    std::uint64_t sourceReads = 0;
    std::uint64_t diskHits = 0;
};

// One artist session over the sequence with evaluators and caches of its own. `afterScrub` is
// called once every member has been shown; the reads at that point are all the pass may make.
void pass(const std::string& label, const std::filesystem::path& first,
          const std::filesystem::path& workDir, const std::function<Counts()>& counts,
          const std::optional<std::uint64_t> expectedReadsAfterScrub) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    auto created = scripting::Session::createNew("Routes", "Main",
                                                 core::RationalTime::fromInteger(1), *format);
    expect(static_cast<bool>(created), label + ": the session is created");
    if (!created)
        return;
    const auto scripting = std::move(created).takeSession();
    const auto composition = scripting->snapshot().project().compositions().front().id();
    const auto imported = scripting->executeJsonOperation(
        "bloom.asset.import",
        {{"paths",
          scripting::Value{scripting::ValueArray{scripting::Value{first.generic_string()}}}}});
    expect(imported.succeeded() && scripting->snapshot().project().assets().size() == 1,
           label + ": the numbered files import as one sequence");
    if (scripting->snapshot().project().assets().size() != 1)
        return;
    const auto asset = scripting->snapshot().project().assets().front().id;
    const auto layered = scripting->executeJsonOperation(
        "bloom.layer.add-from-assets",
        {{"composition", scripting::Value{scripting::StableId{composition.value()}}},
         {"time", scripting::Value{std::int64_t{0}}},
         {"assets", scripting::Value{scripting::ValueArray{
                        scripting::Value{scripting::StableId{asset.value()}}}}}});
    expect(layered.succeeded(), label + ": the sequence becomes a layer");
    if (!layered.succeeded())
        return;
    ui::CompositionSession session(*scripting->document(), *scripting->commandStack(), composition);
    const auto rate = scripting->snapshot().project().compositions().front().format().frameRate();
    const auto timeOf = [&](const std::uint64_t frame) {
        return *core::RationalTime::create(static_cast<std::int64_t>(frame) *
                                               static_cast<std::int64_t>(rate.denominator()),
                                           static_cast<std::int64_t>(rate.numerator()));
    };

    Rig rig;
    expect(runtime::registerBuiltInNodeDefinitions(rig.definitions),
           label + ": node definitions register");
    rig.definitions.freeze();
    rig.compiler = std::make_unique<runtime::SnapshotCompiler>(rig.definitions);
    rig.qualified.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    rig.bridge = std::make_unique<ui::TaskUiBridge>(rig.scheduler, nullptr, 1ms);
    auto pipeline = ui::makeCompositionPreviewPipeline(*rig.compiler, rig.evaluator,
                                                       rig.displayPreparer, rig.qualified);
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController controller(session, rig.scheduler, *rig.bridge, pipeline,
                                                settings);
    ui::RamPreviewController ram(session, controller, rig.scheduler, *rig.bridge, pipeline);
    ui::ViewerEditor viewer(session, controller, &ram);
    viewer.resize(480, 320);
    viewer.show();

    // Scrubbing: every member forward, then back again.
    const auto show = [&](const std::uint64_t frame, const bool first) {
        const auto before = viewer.displayedFrameForTest();
        expect(session.setCurrentTime(timeOf(frame)) || first,
               label + ": the playhead moves to frame " + std::to_string(frame));
        controller.requestRefresh();
        expect(bloom::test_support::awaitQtCondition(
                   "the Viewer shows the frame",
                   [&] {
                       const auto displayed = viewer.displayedFrameForTest();
                       return controller.state().activity == ui::PreviewActivity::Ready &&
                              displayed != nullptr && (first || displayed != before) &&
                              viewer.displayedPreviewState().freshness ==
                                  ui::FrameFreshness::Current;
                   }),
               label + ": the Viewer shows frame " + std::to_string(frame));
    };
    for (std::uint64_t frame = 0; frame < kMembers; ++frame)
        show(frame, frame == 0);
    const auto afterScrub = counts();
    if (expectedReadsAfterScrub)
        expect(afterScrub.sourceReads == *expectedReadsAfterScrub,
               label + ": scrubbing over the sequence read its storage " +
                   std::to_string(afterScrub.sourceReads) + " times, expected " +
                   std::to_string(*expectedReadsAfterScrub));
    const auto unchanged = [&](const std::string& route) {
        expect(counts().sourceReads == afterScrub.sourceReads,
               label + ": " + route + " read the sequence's storage again (" +
                   std::to_string(counts().sourceReads) + " reads, was " +
                   std::to_string(afterScrub.sourceReads) + ")");
    };
    show(1, false);
    show(0, false);
    unchanged("scrubbing back");

    // Playback with the Viewer's own transport, from the first frame until the playhead has
    // passed the last member.
    auto* const play = viewer.findChild<QAbstractButton*>(QStringLiteral("playPauseButton"));
    expect(play != nullptr, label + ": the Viewer has its play button");
    if (play != nullptr) {
        play->click();
        expect(bloom::test_support::awaitQtCondition(
                   "playback passes the sequence",
                   [&] { return session.currentTime() >= timeOf(kMembers); }),
               label + ": playback moves the playhead past the sequence");
        if (play->isChecked())
            play->click();
        expect(bloom::test_support::awaitQtCondition(
                   "the Viewer settles after playback",
                   [&] { return controller.state().activity == ui::PreviewActivity::Ready; }),
               label + ": the Viewer settles after playback");
    }
    unchanged("playback");

    // RAM Preview of the whole composition, started from the Viewer's button.
    auto* const cache =
        viewer.findChild<QAbstractButton*>(QStringLiteral("timelineRamPreviewButton"));
    expect(cache != nullptr && cache->isEnabled(), label + ": the Viewer has RAM Preview");
    if (cache != nullptr) {
        cache->click();
        expect(bloom::test_support::awaitQtCondition(
                   "RAM Preview starts",
                   [&] { return ram.isCaching() || ram.lastCompletion().has_value(); }) &&
                   bloom::test_support::awaitQtCondition("RAM Preview finishes",
                                                         [&] { return !ram.isCaching(); }) &&
                   ram.lastCompletion() == ui::RamPreviewController::Completion::Complete &&
                   ram.cachedFrameCount() == ram.totalFrameCount() && ram.totalFrameCount() != 0,
               label + ": RAM Preview caches every frame (" +
                   std::to_string(ram.cachedFrameCount()) + " of " +
                   std::to_string(ram.totalFrameCount()) + ")");
    }
    unchanged("RAM Preview");
    ram.beginShutdown();

    // Movie export of the three frames through the export the Export panel runs. Its evaluators
    // are made for the export and have no cache of this session's.
    if (output::mediaWorkerAvailableV1()) {
        runtime::TaskScheduler exportScheduler;
        const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
        options.enabled = false;
        const auto provider = host::GpuExportProvider::create(options);
        provider->prepare(exportScheduler);
        const auto movie = workDir / (label + ".mov");
        const auto encoded = scripting::Render::run(
            *scripting, exportScheduler, exportCompiler,
            {.composition = composition,
             .frame = std::nullopt,
             .range = std::pair<std::uint64_t, std::uint64_t>{0, kMembers - 1},
             .preset = output::OutputPresetV1::ProResMovV1,
             .destination = movie},
            workDir / ("scratch-" + label), provider);
        expect(encoded.succeeded && std::filesystem::exists(movie),
               label + ": the movie exports: " + encoded.diagnostic);
        expect(provider->shutdownAndWait(10s), label + ": the export provider retires");
        unchanged("the movie export");
    } else {
        std::cout << "NotRun: " << label << " movie export: no media worker here\n";
    }

    controller.beginShutdown();
    rig.bridge->beginShutdown();
    expect(bloom::test_support::awaitQtCondition("the scheduler is quiet",
                                                 [&] { return rig.scheduler.isQuiescent(); }),
           label + ": the scheduler is quiet at the end");
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomRouteSourceReads");
    QCoreApplication::setApplicationName("RouteSourceReads");
    QSettings().setValue("viewer/resolution", "Full");
    const std::filesystem::path workDir = temporary.path().toStdString();

    // The sequence: three small frames, each its own color.
    const auto footage = workDir / "footage";
    std::filesystem::create_directories(footage);
    const std::array<QColor, kMembers> colors{QColor(220, 40, 30), QColor(30, 200, 60),
                                              QColor(40, 60, 230)};
    std::filesystem::path first;
    for (std::uint64_t member = 0; member < kMembers; ++member) {
        QImage image(static_cast<int>(kWidth), static_cast<int>(kHeight), QImage::Format_RGBA8888);
        image.fill(colors.at(member));
        const auto path = footage / ("plate.000" + std::to_string(member + 1) + ".png");
        expect(image.save(QString::fromStdString(path.string())), "a sequence member is written");
        if (member == 0)
            first = path;
    }

    media::cache::SourceMirrorConfig config;
    config.rootDirectory = workDir / "mirror";
    // The scratch folder is fast local storage: the preference stands in for a network share.
    config.policy.preference = media::cache::LocalCopyPreference::Always;
    const auto mirror = std::make_shared<media::cache::SourceMirror>(config);
    mirror->flush();
    platform::setProcessSourceStore(mirror);
    const auto counts = [&] {
        const auto statistics = mirror->statistics();
        return Counts{statistics.networkReads + statistics.otherSourceReads, statistics.diskHits};
    };

    pass("first", first, workDir, counts, kMembers);
    mirror->flush();
    const auto afterFirst = counts();
    expect(mirror->statistics().writes == kMembers, "each member is kept locally once");
    pass("second", first, workDir, counts, kMembers);
    platform::setProcessSourceStore(nullptr);
    const auto afterSecond = counts();
    expect(afterSecond.sourceReads == afterFirst.sourceReads,
           "a second session with empty caches reads nothing from the sequence's storage");
    expect(afterSecond.diskHits > afterFirst.diskHits,
           "the second session is served from the local copies");

    failures += bloom::test_support::awaitFailureCount();
    if (failures != 0) {
        std::cerr << "media route source reads: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: media route source reads\n";
    return 0;
}

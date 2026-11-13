// #380: the Assets selection header shows a composition as a rendered frame. The frame is prepared
// on a worker through the composition preview pipeline, kept per composition and revision (so
// refreshing the header costs nothing), shown stale while an edit is prepared, then replaced.

#include "await_qt.hpp"
#include <QApplication>
#include <QPixmap>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/composition_thumbnail_controller.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/thumbnail.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace bloom;
using namespace std::chrono_literals;

namespace {
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction pipeline;
    Pipeline() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

struct Fixture final {
    document::NewProject initial =
        document::makeNewProject("Thumbnails", "Main", core::RationalTime::fromInteger(2),
                                 *document::CompositionFormat::create(320, 180));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    ui::CompositionSession session{document, commands, initial.initialCompositionId};
    runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                                  .blockingIoWorkerCount = 1,
                                                                  .cpuQueueCapacity = 16,
                                                                  .blockingIoQueueCapacity = 4,
                                                                  .terminalHistoryCapacity = 32,
                                                                  .diagnosticsPerTask = 8,
                                                                  .groupRegistryCapacity = 8}};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    Pipeline pipeline;
    ui::CompositionThumbnailController thumbnails{session, scheduler, bridge, pipeline.pipeline,
                                                  std::size_t{256} << 20U};
    ~Fixture() {
        thumbnails.cancel();
        bridge.beginShutdown();
        // A destructor cannot assert; a timeout is counted by the helper and fails the exit.
        (void)bloom::test_support::awaitQtCondition("scheduler quiescent after shutdown",
                                                    [&] { return scheduler.isQuiescent(); });
    }
};

QColor centre(const ui::kit::KThumbnail& thumbnail) {
    const auto image = thumbnail.pixmap().toImage();
    return image.isNull() ? QColor{} : image.pixelColor(image.width() / 2, image.height() / 2);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ui::kit::installAfterlabTheme(app);
    Fixture fixture;
    auto& session = fixture.session;
    expect(session.addSolidLayer("Red", core::Color4d{1.0, 0.0, 0.0, 1.0}), "red solid added");
    const auto composition = session.compositionId();

    ui::AssetsEditor editor(session);
    editor.resize(480, 320);
    editor.show();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    auto* thumbnail = editor.findChild<ui::kit::KThumbnail*>("assetsSelectionThumbnail");
    if (tree == nullptr || thumbnail == nullptr) {
        std::cerr << "FAIL: Assets header widgets\n";
        return 1;
    }
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == "Main")
            tree->setCurrentItem(*it);
    // The first header shows the kind glyph while the worker prepares the frame.
    expect(!fixture.thumbnails.current(composition), "the frame is not ready at once");
    QApplication::processEvents();
    expect(fixture.thumbnails.submissionCount() == 1, "selecting a composition asks for a frame");
    expect(
        bloom::test_support::awaitQtCondition("the header shows the composition's rendered frame",
                                              [&] { return centre(*thumbnail).red() > 200; }),
        "the header shows the composition's rendered frame");
    const auto red = centre(*thumbnail);
    expect(red.green() < 60 && red.blue() < 60, "the rendered frame is the red solid");
    expect(fixture.thumbnails.lastWorkerThread().has_value() &&
               *fixture.thumbnails.lastWorkerThread() != std::this_thread::get_id(),
           "the frame was prepared on a worker, not the interface thread");
    const auto pixmap = thumbnail->pixmap();
    expect(std::max(pixmap.width(), pixmap.height()) <=
               ui::CompositionThumbnailController::kMaximumEdge,
           "the frame is a small proxy");

    // Cached by revision: refreshing the header (selection, panel updates) prepares nothing new.
    for (int i = 0; i < 3; ++i) {
        tree->clearSelection();
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == "Main")
                tree->setCurrentItem(*it);
        QApplication::processEvents();
    }
    expect(fixture.thumbnails.submissionCount() == 1, "the frame is reused for the same revision");

    // An edit prepares the new revision; the previous frame stays up until it arrives.
    expect(session.addSolidLayer("Blue", core::Color4d{0.0, 0.0, 1.0, 1.0}), "blue solid added");
    QApplication::processEvents();
    expect(fixture.thumbnails.submissionCount() == 2, "an edit asks for a new frame");
    expect(!fixture.thumbnails.thumbnail(composition).isNull(),
           "the previous frame is shown while the new one is prepared");
    expect(bloom::test_support::awaitQtCondition("the header updates to the edited composition",
                                                 [&] { return centre(*thumbnail).blue() > 200; }),
           "the header updates to the edited composition");
    expect(fixture.thumbnails.current(composition), "the frame now matches the revision");

    // Issue #409: under ACES CG the thumbnail uses the Viewer's default display/view (sRGB -
    // Display / Un-tone-mapped), not the config's tone-mapped default: scene-linear 18% gray is
    // sRGB code value 118, as the Viewer shows it.
    {
        const auto revision = color::ocioBuiltInContentRevision(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri);
        expect(revision.has_value(), "the ACES config is available");
        if (revision) {
            Fixture aces;
            auto settings = aces.session.colorSettings();
            settings.processColorSpaceId = std::string(color::kAcesCgV1SceneLinearColorSpaceId);
            settings.ocioConfig.locator =
                document::BuiltInOcioConfigLocator{std::string(color::kAcesCgV1ConfigUri)};
            settings.ocioConfig.expectedRevision = {
                .algorithm = document::OcioRevisionAlgorithm::Sha256, .digest = *revision};
            aces.session.setColorSettings(settings);
            expect(aces.session.addSolidLayer("Gray", core::Color4d{0.18, 0.18, 0.18, 1.0}),
                   "gray solid added");
            const auto id = aces.session.compositionId();
            (void)aces.thumbnails.thumbnail(id); // asks for the frame on a worker
            expect(bloom::test_support::awaitQtCondition("the ACES composition frame is prepared",
                                                         [&] {
                                                             (void)aces.thumbnails.thumbnail(id);
                                                             return aces.thumbnails.current(id);
                                                         }),
                   "the ACES composition frame is prepared");
            const auto image = aces.thumbnails.thumbnail(id);
            const auto gray =
                image.isNull() ? QColor{} : image.pixelColor(image.width() / 2, image.height() / 2);
            std::cerr << "  ACES composition thumbnail centre: " << gray.red() << '\n';
            expect(gray.red() >= 116 && gray.red() <= 119,
                   "the ACES composition thumbnail shows 18% gray un-tone-mapped (118)");
        }
    }

    if (failures + bloom::test_support::awaitFailureCount() != 0)
        return 1;
    std::cout << "PASS: composition thumbnails\n";
    return 0;
}

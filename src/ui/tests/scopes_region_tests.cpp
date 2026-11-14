// #724 Scopes: the scope region in the Viewer, on 64x64 compositions through the Viewer's own mouse
// and key handling.
//
// Asserted:
//   * a drag while armed draws a whole-pixel rectangle (x and y floored, the far edge raised);
//     arming clears after the drag; Escape cancels a drag in flight and keeps the previous region,
//     and disarms when no drag runs; a drag with an empty result is ignored;
//   * the body moves, each of the eight handles resizes, and the result is clipped to the frame, at
//     the fitted zoom, zoomed out and in, and at half preview resolution;
//   * not armed and off the region, a press reaches the Viewer's other gestures (marquee, layer
//     move), and the region of interest tool and its rectangle are untouched by scope edits;
//   * a region belongs to its composition; two Viewers of one composition both draw it; with no
//     visible Scopes panel nothing is drawn and no input is taken;
//   * the region is no project state: no revision, no undo step, no new frame, and a project saved
//     with a region set is the very file a save without one gives.
//   * the region's edges snap to device-pixel centers at scales 1, 1.5 and 2.

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/scopes_editor.hpp>
#include <bloom/ui/scopes_region_store.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QWheelEvent>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>

namespace {

namespace document = bloom::document;
namespace runtime = bloom::runtime;
namespace ui = bloom::ui;
using namespace std::chrono_literals;
using Region = runtime::ScopeRegion;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// Polls a condition while the event loop runs; the bound only ends a run that already failed.
template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

constexpr std::uint32_t kSize = 64;

document::CompositionFormat format() {
    const auto value = document::CompositionFormat::create(kSize, kSize);
    if (!value.has_value())
        std::abort();
    return *value;
}

document::Composition secondComposition() {
    using namespace bloom::document;
    const auto stackId = NodeId::fromRaw(100);
    const auto outputId = NodeId::fromRaw(101);
    CanonicalGraph graph(stackId);
    const bool built =
        graph.addNode(
            {stackId, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}) &&
        graph.addNode({outputId,
                       std::string(kCompositionOutputNodeType),
                       {},
                       kCompositionOutputNodeSchemaVersion}) &&
        graph.addEdge({EdgeId::fromRaw(100),
                       {stackId, std::string(kLayerStackOutputPort)},
                       NodeInputRef{outputId, std::string(kCompositionOutputInputPort)}});
    graph.setCompositionOutput({outputId, std::string(kCompositionOutputOutputPort)});
    if (!built)
        std::abort();
    return Composition(CompositionId::fromRaw(2), "Second",
                       bloom::core::RationalTime::fromInteger(10), std::move(graph), format());
}

document::NewProject project() {
    auto result = document::makeNewProject("Scope region", "Main",
                                           bloom::core::RationalTime::fromInteger(10), format());
    if (!result.project.addComposition(secondComposition()))
        std::abort();
    return result;
}

struct PipelineFixture final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

// One session, one preview controller, the shared region store, a Viewer and a Scopes panel that
// follows it (no measuring: the panel's controller is not under test here).
struct Fixture final {
    document::Document doc;
    bloom::commands::CommandStack commands;
    ui::CompositionSession session;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    ui::CompositionPreviewController controller;
    ui::ScopesRegionStore store;
    ui::ViewerEditor viewer;
    std::unique_ptr<ui::ScopesEditor> panel;

    explicit Fixture(document::NewProject newProject)
        : doc(std::move(newProject.project)), commands(doc),
          session(doc, commands, newProject.initialCompositionId),
          scheduler(runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                 .blockingIoWorkerCount = 1,
                                                 .cpuQueueCapacity = 16,
                                                 .blockingIoQueueCapacity = 4,
                                                 .terminalHistoryCapacity = 32,
                                                 .diagnosticsPerTask = 8,
                                                 .groupRegistryCapacity = 8}),
          bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline), viewer(session, controller) {
        viewer.setScopesRegionStore(&store);
        viewer.resize(400, 300);
        viewer.show();
        ui::ScopesServices services;
        services.regions = &store;
        panel = std::make_unique<ui::ScopesEditor>(services);
        panel->show();
        panel->followViewer(&viewer);
    }

    ~Fixture() {
        controller.beginShutdown();
        bridge.beginShutdown();
        (void)waitUntil([&] { return scheduler.isQuiescent(); });
    }

    [[nodiscard]] bool ready() {
        return waitUntil([&] {
            const auto& frame = controller.state().frame;
            return frame &&
                   frame->desiredIdentity().sourceRevision == session.snapshot().revision() &&
                   frame->desiredIdentity().time == session.currentTime() &&
                   controller.state().activity == ui::PreviewActivity::Ready &&
                   controller.state().desiredIdentity &&
                   frame->desiredIdentity() == *controller.state().desiredIdentity &&
                   viewer.mappingForTest().has_value();
        });
    }

    [[nodiscard]] ui::ScopesRegionStore::Key key() const {
        return {session.snapshot().project().id(), session.compositionId()};
    }
};

// A real project session with the same rig: the project can be saved to a file.
struct SavedFixture final {
    std::unique_ptr<bloom::scripting::Session> scripting;
    document::CompositionId composition;
    std::unique_ptr<ui::CompositionSession> session;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    std::unique_ptr<ui::CompositionPreviewController> controller;
    ui::ScopesRegionStore store;
    std::unique_ptr<ui::ViewerEditor> viewer;
    std::unique_ptr<ui::ScopesEditor> panel;

    SavedFixture()
        : scheduler(runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                 .blockingIoWorkerCount = 1,
                                                 .cpuQueueCapacity = 16,
                                                 .blockingIoQueueCapacity = 4,
                                                 .terminalHistoryCapacity = 32,
                                                 .diagnosticsPerTask = 8,
                                                 .groupRegistryCapacity = 8}),
          bridge(scheduler, nullptr, 1ms) {
        auto created = bloom::scripting::Session::createNew(
            "Scope region", "Main", bloom::core::RationalTime::fromInteger(10), format());
        if (!created)
            std::abort();
        scripting = std::move(created).takeSession();
        composition = scripting->snapshot().project().compositions().front().id();
        session = std::make_unique<ui::CompositionSession>(*scripting->document(),
                                                           *scripting->commandStack(), composition);
        controller = std::make_unique<ui::CompositionPreviewController>(*session, scheduler, bridge,
                                                                        pipeline.pipeline);
        viewer = std::make_unique<ui::ViewerEditor>(*session, *controller);
        viewer->setScopesRegionStore(&store);
        viewer->resize(400, 300);
        viewer->show();
        ui::ScopesServices services;
        services.regions = &store;
        panel = std::make_unique<ui::ScopesEditor>(services);
        panel->show();
        panel->followViewer(viewer.get());
    }

    ~SavedFixture() {
        panel.reset();
        viewer.reset();
        controller->beginShutdown();
        bridge.beginShutdown();
        (void)waitUntil([&] { return scheduler.isQuiescent(); });
    }

    [[nodiscard]] bool ready() {
        return waitUntil([&] {
            const auto& frame = controller->state().frame;
            return frame && controller->state().activity == ui::PreviewActivity::Ready &&
                   viewer->mappingForTest().has_value();
        });
    }
};

[[nodiscard]] std::string fileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Pointer input in composition pixels, through the Viewer's own event handlers.
QPointF screen(ui::ViewerEditor& viewer, const QPointF compositionPoint) {
    // The Viewer maps against what it painted last; after a zoom or resolution change that is the
    // next frame.
    (void)waitUntil([&] {
        (void)viewer.grab();
        return viewer.mappingForTest().has_value();
    });
    const auto mapping = viewer.mappingForTest();
    if (!mapping) {
        std::cerr << "the Viewer has no mapping\n";
        std::abort();
    }
    return mapping->toScreen({compositionPoint.x(), compositionPoint.y()});
}

void mouse(ui::ViewerEditor& viewer, const QEvent::Type type, const QPointF compositionPoint,
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    if (type == QEvent::MouseButtonPress)
        (void)viewer.grab();
    const auto at = screen(viewer, compositionPoint);
    QMouseEvent event(type, at, at, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      modifiers);
    QCoreApplication::sendEvent(&viewer, &event);
}

void drag(ui::ViewerEditor& viewer, const QPointF from, const QPointF to,
          const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(viewer, QEvent::MouseButtonPress, from, modifiers);
    mouse(viewer, QEvent::MouseMove, (from + to) / 2.0, modifiers);
    mouse(viewer, QEvent::MouseMove, to, modifiers);
    mouse(viewer, QEvent::MouseButtonRelease, to, modifiers);
}

void escape(ui::ViewerEditor& viewer) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&viewer, &event);
}

[[nodiscard]] std::string text(const std::optional<Region>& region) {
    if (!region)
        return "none";
    return std::to_string(region->x) + "," + std::to_string(region->y) + " " +
           std::to_string(region->width) + "x" + std::to_string(region->height);
}

// The rectangle a drag from `from` by `delta` should leave, computed here independently:
// x and y floored, the far edge raised, clipped to the frame.
[[nodiscard]] std::optional<Region> wholePixels(const double left, const double top,
                                                const double right, const double bottom) {
    const auto clamp = [](const double v) {
        return std::min(std::max(v, 0.0), static_cast<double>(kSize));
    };
    const double x0 = std::floor(clamp(std::min(left, right)));
    const double y0 = std::floor(clamp(std::min(top, bottom)));
    const double x1 = std::ceil(clamp(std::max(left, right)));
    const double y1 = std::ceil(clamp(std::max(top, bottom)));
    if (x1 <= x0 || y1 <= y0)
        return std::nullopt;
    return Region{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
                  static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)};
}

// Zooms the Viewer about its center with the wheel, as a user does.
void wheelZoom(Fixture& f, const int notches) {
    const QPointF at = f.viewer.contentRectForTest().center();
    QWheelEvent event(at, f.viewer.mapToGlobal(at), QPoint(), QPoint(0, 120 * notches),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&f.viewer, &event);
    // The Viewer maps gestures against what it painted last.
    (void)f.viewer.grab();
}

// ---- draw, arming, Escape -------------------------------------------------------------------

void testDrawAndEscape(Expectations& e) {
    Fixture f(project());
    e.expect(f.ready(), "the preview is ready");
    const auto key = f.key();
    e.expect(f.store.visiblePanels() == 1 && f.store.active(),
             "the shown panel makes the store active");

    // Not armed: a drag over empty space does not draw.
    drag(f.viewer, {10.4, 12.7}, {30.3, 40.4});
    e.expect(!f.store.region(key), "not armed: a drag draws nothing");

    f.panel->setRegionArmed(true);
    drag(f.viewer, {10.4, 12.7}, {30.3, 40.4});
    e.expect(f.store.region(key) == wholePixels(10.4, 12.7, 30.3, 40.4) &&
                 f.store.region(key) == Region{10, 12, 21, 29},
             "armed: a drag draws a whole-pixel rectangle, got " + text(f.store.region(key)));
    e.expect(!f.store.armed() && !f.panel->regionArmed(), "arming clears after the drag");
    e.expect(f.panel->region() == f.store.region(key) &&
                 f.panel->measureSettings().region == f.store.region(key),
             "the panel's region and measure settings carry it");
    e.expect(f.viewer.scopesRegionDrawnForTest() == f.store.region(key), "the Viewer draws it");

    // A drag drawn right to left, bottom to top gives the same rectangle.
    f.panel->clearRegion();
    f.panel->setRegionArmed(true);
    drag(f.viewer, {30.3, 40.4}, {10.4, 12.7});
    e.expect(f.store.region(key) == Region{10, 12, 21, 29}, "the drag direction does not matter");

    // Escape cancels a drag in flight and leaves the previous region.
    f.panel->setRegionArmed(true);
    mouse(f.viewer, QEvent::MouseButtonPress, {40.2, 41.1});
    mouse(f.viewer, QEvent::MouseMove, {55.5, 58.5});
    e.expect(f.viewer.scopesRegionGestureForTest() &&
                 f.viewer.scopesRegionDrawnForTest() == wholePixels(40.2, 41.1, 55.5, 58.5),
             "the drag in flight is shown live");
    e.expect(f.store.region(key) == Region{10, 12, 21, 29}, "and is not stored before release");
    escape(f.viewer);
    mouse(f.viewer, QEvent::MouseButtonRelease, {55.5, 58.5});
    e.expect(!f.viewer.scopesRegionGestureForTest() &&
                 f.store.region(key) == Region{10, 12, 21, 29},
             "Escape cancels the drag and leaves the previous region");
    e.expect(f.store.armed(), "a cancelled drag keeps the toggle armed");
    escape(f.viewer);
    e.expect(!f.store.armed() && !f.panel->regionArmed(), "Escape with no drag disarms the toggle");

    // Clear Region removes it.
    f.panel->clearRegion();
    e.expect(!f.store.region(key) && !f.viewer.scopesRegionDrawnForTest(),
             "Clear Region removes it");

    // An empty result is ignored and keeps the toggle armed: a click, and a drag outside the frame.
    f.panel->setRegion(Region{5, 5, 8, 8});
    f.panel->setRegionArmed(true);
    mouse(f.viewer, QEvent::MouseButtonPress, {30.0, 30.0});
    mouse(f.viewer, QEvent::MouseButtonRelease, {30.0, 30.0});
    e.expect(f.store.region(key) == Region{5, 5, 8, 8} && f.store.armed(),
             "a click draws nothing and changes nothing");
    drag(f.viewer, {-6.0, 20.0}, {-2.0, 40.0});
    e.expect(f.store.region(key) == Region{5, 5, 8, 8} && f.store.armed(),
             "a drag wholly outside the frame is ignored");
    // Clipped to the frame: a drag that leaves the frame keeps the part inside.
    drag(f.viewer, {50.5, 50.5}, {80.0, 90.0});
    e.expect(f.store.region(key) == Region{50, 50, 14, 14},
             "a drag past the frame is clipped, got " + text(f.store.region(key)));
}

// ---- move, resize, clip, rounding -----------------------------------------------------------

struct Handle final {
    const char* name;
    int sx; // -1 left, 0 none, 1 right
    int sy;
};

void moveAndResize(Expectations& e, Fixture& f, const std::string& where) {
    const auto key = f.key();
    // Move by the body: whole pixels, and kept whole inside the frame.
    f.store.setRegion(key, Region{10, 10, 20, 20});
    // Not armed, the body belongs to what is under it: the region stays.
    drag(f.viewer, {20.0, 20.0}, {25.4, 23.1});
    e.expect(f.store.region(key) == Region{10, 10, 20, 20},
             where + ": not armed, the body does not move the region, got " +
                 text(f.store.region(key)));
    f.session.clearSelection();
    f.store.setArmed(true);
    drag(f.viewer, {20.0, 20.0}, {25.4, 23.1});
    e.expect(f.store.region(key) == Region{15, 13, 20, 20},
             where + ": the body moves by whole pixels, got " + text(f.store.region(key)));
    drag(f.viewer, {25.0, 25.0}, {125.0, 125.0});
    e.expect(f.store.region(key) == Region{44, 44, 20, 20},
             where + ": a move stays inside the frame, got " + text(f.store.region(key)));
    e.expect(f.store.armed(), where + ": moving leaves the toggle armed");
    f.store.setArmed(false);

    // Each handle resizes: x and y floored, the far edge raised.
    const double dx = 3.3;
    const double dy = 2.6;
    for (const auto& handle :
         {Handle{"left", -1, 0}, Handle{"right", 1, 0}, Handle{"top", 0, -1},
          Handle{"bottom", 0, 1}, Handle{"top-left", -1, -1}, Handle{"top-right", 1, -1},
          Handle{"bottom-left", -1, 1}, Handle{"bottom-right", 1, 1}}) {
        f.store.setRegion(key, Region{20, 20, 20, 20});
        const QPointF at(30.0 + handle.sx * 10.0, 30.0 + handle.sy * 10.0);
        drag(f.viewer, at, at + QPointF(dx, dy));
        double left = 20.0;
        double top = 20.0;
        double right = 40.0;
        double bottom = 40.0;
        if (handle.sx < 0)
            left += dx;
        if (handle.sx > 0)
            right += dx;
        if (handle.sy < 0)
            top += dy;
        if (handle.sy > 0)
            bottom += dy;
        const auto expected = wholePixels(left, top, right, bottom);
        e.expect(f.store.region(key) == expected,
                 where + ": the " + handle.name + " handle resizes, expected " + text(expected) +
                     " got " + text(f.store.region(key)));
    }
    // Resizing past the frame clips; dragging a handle across the far edge flips the rectangle.
    f.store.setRegion(key, Region{20, 20, 20, 20});
    drag(f.viewer, {40.0, 30.0}, {90.0, 30.0});
    e.expect(f.store.region(key) == Region{20, 20, 44, 20},
             where + ": a resize is clipped to the frame, got " + text(f.store.region(key)));
    f.store.setRegion(key, Region{20, 20, 20, 20});
    drag(f.viewer, {20.0, 30.0}, {-10.0, 30.0});
    e.expect(f.store.region(key) == Region{0, 20, 40, 20},
             where + ": the left handle clips at the frame, got " + text(f.store.region(key)));
}

void testMoveResizeClip(Expectations& e) {
    Fixture f(project());
    e.expect(f.ready(), "the preview is ready");
    moveAndResize(e, f, "fit");
    const double fitted = f.viewer.mappingForTest()->displayRect.width();
    wheelZoom(f, -3);
    e.expect(f.ready(), "the preview is ready zoomed out");
    const double zoomed = f.viewer.mappingForTest()->displayRect.width();
    e.expect(!f.viewer.viewTransformForTest().fitToWindow && zoomed < fitted * 0.8,
             "the Viewer is zoomed out, " + std::to_string(zoomed) + " of " +
                 std::to_string(fitted));
    moveAndResize(e, f, "zoomed out");
    wheelZoom(f, 5);
    e.expect(f.ready(), "the preview is ready zoomed in");
    e.expect(f.viewer.mappingForTest()->displayRect.width() > fitted, "the Viewer is zoomed in");
    moveAndResize(e, f, "zoomed in");
    f.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Half);
    e.expect(f.ready(), "the half resolution preview is ready");
    const auto& frame = f.controller.state().frame;
    e.expect(frame != nullptr && f.viewer.mappingForTest()->compositionFormat.width() == kSize,
             "the mapping is in full composition pixels at half resolution");
    moveAndResize(e, f, "half resolution");
    f.panel->setRegionArmed(true);
    drag(f.viewer, {10.4, 12.7}, {30.3, 40.4});
    e.expect(f.store.region(f.key()) == Region{10, 12, 21, 29},
             "a drawn rectangle is the same at half resolution");
}

// ---- other gestures ---------------------------------------------------------------------------

void testOtherGestures(Expectations& e) {
    Fixture f(project());
    e.expect(f.ready(), "the preview is ready");
    const auto key = f.key();
    bloom::commands::ShapeLayerGeometry geometry;
    geometry.position = document::Vec2d{12.0, 12.0};
    geometry.size = document::Vec2d{12.0, 10.0};
    e.expect(f.session.addShapeLayer(document::ShapeKind::Rectangle, geometry), "a shape layer");
    f.session.clearSelection();
    e.expect(f.ready(), "the preview shows the layer");
    f.store.setRegion(key, Region{40, 40, 20, 20});

    // Off the region and not armed: the marquee selects the layer.
    drag(f.viewer, {30.0, 30.0}, {0.5, 0.5});
    e.expect(!f.session.selectedNodes().empty(), "off the region, a marquee reaches the Viewer");
    e.expect(f.store.region(key) == Region{40, 40, 20, 20}, "and the region is untouched");
    f.session.clearSelection();
    e.expect(f.ready(), "the preview is ready");
    // A press on the layer, off the region, selects and moves it: one history entry.
    const auto before = f.commands.size();
    drag(f.viewer, {17.0, 15.0}, {23.0, 19.0});
    e.expect(!f.session.selectedNodes().empty() && f.commands.size() == before + 1,
             "off the region, a layer press selects and moves the layer");
    e.expect(f.store.region(key) == Region{40, 40, 20, 20}, "and the region is untouched");

    // The region of interest tool: Ctrl+Alt over the region's body is the ROI's drag, and scope
    // edits leave the ROI's rectangle as it was.
    const auto modifiers = Qt::ControlModifier | Qt::AltModifier;
    drag(f.viewer, {44.0, 44.0}, {58.0, 58.0}, modifiers);
    const auto roi = f.controller.regionOfInterest();
    e.expect(roi.has_value() && f.store.region(key) == Region{40, 40, 20, 20},
             "Ctrl+Alt is the region of interest's drag, even over the scope region");
    f.panel->setRegionArmed(true);
    drag(f.viewer, {2.0, 2.0}, {10.0, 10.0});
    drag(f.viewer, {5.0, 5.0}, {9.0, 9.0});
    e.expect(f.controller.regionOfInterest() == roi, "scope region edits leave the ROI unchanged");
    e.expect(f.store.region(key).has_value(), "the scope region is there");
}

// ---- per composition, two Viewers, no panel ----------------------------------------------------

void testPerCompositionAndVisibility(Expectations& e) {
    Fixture f(project());
    e.expect(f.ready(), "the preview is ready");
    const auto first = f.key();
    const ui::ScopesRegionStore::Key second{first.project, document::CompositionId::fromRaw(2)};
    f.panel->setRegion(Region{4, 6, 10, 12});
    e.expect(f.store.region(first) == Region{4, 6, 10, 12} && !f.store.region(second),
             "the region belongs to its composition");

    // A second Viewer of the same composition draws it too.
    ui::ViewerEditor other(f.session, f.controller);
    other.setScopesRegionStore(&f.store);
    other.resize(400, 300);
    other.show();
    e.expect(waitUntil([&] { return other.mappingForTest().has_value(); }),
             "the second Viewer has a mapping");
    e.expect(f.viewer.scopesRegionDrawnForTest() == Region{4, 6, 10, 12} &&
                 other.scopesRegionDrawnForTest() == Region{4, 6, 10, 12},
             "both Viewers of one composition draw it");
    // The overlay is on the picture: the grab differs from the grab with no region.
    const QImage withRegion = other.grab().toImage();
    f.panel->clearRegion();
    const QImage without = other.grab().toImage();
    e.expect(withRegion != without, "the overlay changes the Viewer's picture");
    f.panel->setRegion(Region{4, 6, 10, 12});
    e.expect(other.grab().toImage() == withRegion, "and the same region draws the same overlay");

    // Another composition keeps its own region, and the panel follows it.
    e.expect(f.session.setComposition(second.composition),
             "the session shows the second composition");
    e.expect(waitUntil([&] { return f.viewer.scopesRegionKey() == second; }) &&
                 waitUntil([&] { return !f.panel->region().has_value(); }),
             "the panel follows the Viewer to the other composition, which has no region");
    f.panel->setRegion(Region{30, 30, 4, 4});
    e.expect(f.store.region(first) == Region{4, 6, 10, 12} &&
                 f.store.region(second) == Region{30, 30, 4, 4},
             "two compositions keep separate regions");
    e.expect(f.session.setComposition(first.composition), "back to the first composition");
    e.expect(waitUntil([&] { return f.panel->region() == Region{4, 6, 10, 12}; }),
             "the panel follows back to the first composition's region");
    e.expect(f.ready(), "the preview is ready again");
    other.hide();

    // No visible Scopes panel: nothing is drawn and no input is taken.
    const QImage visible = f.viewer.grab().toImage();
    f.panel->hide();
    e.expect(f.store.visiblePanels() == 0 && !f.store.active() &&
                 !f.viewer.scopesRegionDrawnForTest(),
             "no visible panel: the Viewer draws no region");
    e.expect(f.viewer.grab().toImage() != visible, "the overlay is gone from the picture");
    f.store.setArmed(true);
    drag(f.viewer, {30.0, 30.0}, {50.0, 50.0});
    drag(f.viewer, {6.0, 8.0}, {16.0, 18.0});
    e.expect(f.store.region(first) == Region{4, 6, 10, 12} &&
                 !f.viewer.scopesRegionGestureForTest(),
             "no visible panel: a drag neither draws nor moves");
    f.panel->show();
    e.expect(f.store.active() && f.viewer.scopesRegionDrawnForTest() == Region{4, 6, 10, 12},
             "showing the panel brings the overlay back");
}

// ---- not project state ------------------------------------------------------------------------

void testNotProjectState(Expectations& e) {
    Fixture f(project());
    e.expect(f.ready(), "the preview is ready");
    const auto revision = f.session.snapshot().revision();
    const auto history = f.commands.size();
    const bool canUndo = f.session.canUndo();
    const auto frame = f.controller.state().frame;
    f.panel->setRegionArmed(true);
    drag(f.viewer, {10.4, 12.7}, {30.3, 40.4});
    drag(f.viewer, {20.0, 25.0}, {30.0, 30.0});
    drag(f.viewer, {41.0, 53.0}, {45.0, 55.0});
    f.panel->clearRegion();
    f.panel->setRegion(Region{1, 1, 5, 5});
    QCoreApplication::processEvents();
    e.expect(f.session.snapshot().revision() == revision, "the project's revision is unchanged");
    e.expect(f.commands.size() == history && f.session.canUndo() == canUndo,
             "no undo step is added");
    e.expect(f.ready() && f.controller.state().frame == frame, "the preview is not rendered again");
    // Undo does not touch the region either.
    (void)f.session.undo();
    e.expect(f.store.region(f.key()) == Region{1, 1, 5, 5}, "undo leaves the region as it was");
}

// The region is not saved: a project saved with a region set is byte for byte the project saved
// without one. (Two saves of one unchanged project are byte-stable, which the first check proves,
// so the comparison is exact rather than a search for region data inside a compressed archive.)
void testRegionIsNotSaved(Expectations& e) {
    SavedFixture f;
    e.expect(f.ready(), "the preview is ready");
    QTemporaryDir directory;
    e.expect(directory.isValid(), "a temporary directory exists");
    const std::filesystem::path folder = directory.path().toStdString();
    const auto none = folder / "none.bloom";
    const auto again = folder / "again.bloom";
    const auto withRegion = folder / "region.bloom";

    e.expect(f.scripting->saveAs(none).succeeded(), "the project saves with no region");
    e.expect(f.scripting->saveAs(again).succeeded(), "the unchanged project saves again");
    const auto bytesNone = fileBytes(none);
    e.expect(!bytesNone.empty() && bytesNone == fileBytes(again),
             "two saves of the unchanged project are byte-identical");

    f.panel->setRegionArmed(true);
    drag(*f.viewer, {10.4, 12.7}, {30.3, 40.4});
    e.expect(f.store.region({f.session->snapshot().project().id(), f.session->compositionId()}) ==
                 Region{10, 12, 21, 29},
             "a region is set");
    e.expect(f.scripting->saveAs(withRegion).succeeded(), "the project saves with a region set");
    e.expect(fileBytes(withRegion) == bytesNone,
             "the file saved with a region set equals the file saved without one");
}

} // namespace

// ---- device-pixel snapping --------------------------------------------------------------------

void testDevicePixelSnapping(Expectations& e) {
    const auto onCenter = [](const double value, const double scale) {
        const double device = value * scale - 0.5;
        return std::abs(device - std::round(device)) < 1e-9;
    };
    const auto within = [](const double snapped, const double input, const double scale) {
        return std::abs(snapped - input) <= 1.0 / scale + 1e-9;
    };
    const QRectF inputs[] = {QRectF(QPointF(10.3, 20.7), QPointF(55.55, 61.1)),
                             QRectF(QPointF(0.01, 3.99), QPointF(7.5, 9.25)),
                             QRectF(QPointF(100.49, 0.0), QPointF(101.0, 12.99))};
    for (const double scale : {1.0, 1.5, 2.0}) {
        bool centered = true;
        bool close = true;
        for (const QRectF& input : inputs) {
            const QRectF out = ui::snapRectToDevicePixelCenters(input, scale);
            centered = centered && onCenter(out.left(), scale) && onCenter(out.top(), scale) &&
                       onCenter(out.right(), scale) && onCenter(out.bottom(), scale);
            close = close && within(out.left(), input.left(), scale) &&
                    within(out.top(), input.top(), scale) &&
                    within(out.right(), input.right(), scale) &&
                    within(out.bottom(), input.bottom(), scale);
        }
        e.expect(centered,
                 "every snapped edge is a device-pixel center at scale " + std::to_string(scale));
        e.expect(close,
                 "every snapped edge is within one device pixel at scale " + std::to_string(scale));
        const QRectF on = ui::snapRectToDevicePixelCenters(inputs[0], scale);
        e.expect(ui::snapRectToDevicePixelCenters(on, scale) == on,
                 "a rectangle already on centers is unchanged at scale " + std::to_string(scale));
    }
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    application.setOrganizationName(QStringLiteral("BloomScopesRegionTests"));
    application.setApplicationName(QStringLiteral("ScopesRegion"));
    ui::kit::installAfterlabTheme(application);
    Expectations e;
    testDrawAndEscape(e);
    testMoveResizeClip(e);
    testOtherGestures(e);
    testPerCompositionAndVisibility(e);
    testNotProjectState(e);
    testRegionIsNotSaved(e);
    testDevicePixelSnapping(e);
    if (e.failures() != 0) {
        std::cerr << e.failures() << " scope region check(s) failed\n";
        return 1;
    }
    std::cout << "scope region tests passed\n";
    return 0;
}

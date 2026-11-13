// #510 the on-Viewer handles of Procedural Warp on a real document: a frame-sized Linear Gradient
// through the warp. Selecting the node draws its handles: the centre as a point, the radius as a
// ring about it (the fade's start as a dashed ring inside), Magnify's pull point and Split's line as
// points. Dragging a point moves it, dragging the ring sets the radius; each gesture is one command,
// Escape confirms it and a mode with no point or radius has no handle. With BLOOM_GRAMMAR_ARTIFACT_DIR
// set it writes each Viewer to <dir>/<name>-viewer.png.

#include "await_qt.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTest>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
// The ceiling for a broken run; a passing wait returns as soon as its condition holds.
constexpr std::chrono::milliseconds kWait{15000};

using namespace bloom;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;
constexpr double kCentreX = kWidth / 2.0;
constexpr double kCentreY = kHeight / 2.0;

struct Scene final {
    document::NewProject initial{document::makeNewProject(
        "Handles", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)))};
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    document::NodeId warp;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Handles UI", session.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = session.executeNodeTransaction(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    void set(const document::NodeId node, const std::string& role, document::ParameterValue value) {
        const auto id = ui::nodeRoleParameter(session, node, role);
        if (!id || !session.setParameterValue(*id, std::move(value), QStringLiteral("Set")))
            throw std::runtime_error("fixture value refused: " + role);
    }

    explicit Scene(const std::int64_t mode) {
        const auto composition = session.compositionId();
        const auto layer = run<commands::AddSolidLayer>(composition, "Plate",
                                                        core::Color4d{0.8, 0.6, 0.4, 1.0});
        const auto layerNode =
            *layer.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        warp = *run<commands::AddNode>(composition, std::string(document::kProceduralWarpNodeType),
                                       document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
        const auto ramp =
            *run<commands::AddNode>(composition, std::string(document::kLinearGradientNodeType),
                                    document::Vec2d{})
                 .outputId<document::NodeId>(commands::kAddNodeOutput);
        set(ramp, "width", static_cast<double>(kWidth));
        set(ramp, "height", static_cast<double>(kHeight));
        set(ramp, "start", document::Vec2d{0.0, 0.0});
        set(ramp, "end", document::Vec2d{static_cast<double>(kWidth), 0.0});
        set(warp, "mode", mode);
        run<commands::ConnectPorts>(composition, document::OutputPortRef{ramp, "image"},
                                    document::NodeInputRef{warp, "input"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{warp, "image"},
                                    document::NodeInputRef{layerNode, "image"});
    }
    [[nodiscard]] double real(const std::string& role) {
        const auto id = ui::nodeRoleParameter(session, warp, role);
        const auto value = id ? session.liveValue(*id) : std::nullopt;
        const auto* stored = value ? std::get_if<double>(&*value) : nullptr;
        return stored ? *stored : std::nan("");
    }
    [[nodiscard]] document::Vec2d point(const std::string& role) {
        const auto id = ui::nodeRoleParameter(session, warp, role);
        const auto value = id ? session.liveValue(*id) : std::nullopt;
        const auto* stored = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
        return stored ? *stored : document::Vec2d{std::nan(""), std::nan("")};
    }
};

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    Pipeline() {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("built-in definitions");
        definitions.freeze();
        compiler = std::make_unique<runtime::SnapshotCompiler>(definitions);
    }
};

// One Viewer on `scene`, its node selected.
struct Harness final {
    Scene& scene;
    Pipeline pipeline;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge{scheduler, nullptr, std::chrono::milliseconds{1}};
    ui::CompositionPreviewController preview;
    QWidget window;
    ui::ViewerEditor* viewer = nullptr;
    std::optional<ui::ViewerMapping> mapping;

    explicit Harness(Scene& target)
        : scene(target),
          preview(scene.session, scheduler, bridge,
                  ui::makeCompositionPreviewPipeline(*pipeline.compiler, pipeline.evaluator,
                                                     pipeline.displayPreparer, pipeline.qualified)) {
        viewer = new ui::ViewerEditor(scene.session, preview, nullptr, &window);
        viewer->setGeometry(0, 0, 720, 480);
        window.resize(720, 480);
        window.show();
        scene.session.selectNode(scene.warp);
        QApplication::processEvents();
        expect(bloom::test_support::awaitQtCondition(
                   "the Viewer shows the composition",
                   [&] {
                       mapping = viewer->mappingForTest();
                       return mapping.has_value() &&
                              preview.state().activity == ui::PreviewActivity::Ready;
                   },
                   kWait),
               "the Viewer shows the composition");
    }
    [[nodiscard]] QPoint at(const double offsetX, const double offsetY) const {
        return mapping->toScreen(document::Vec2d{kCentreX + offsetX, kCentreY + offsetY}).toPoint();
    }
    void press(const QPoint point) {
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, point);
        QApplication::processEvents();
    }
    void move(const QPoint point) {
        QTest::mouseMove(viewer, point);
        QApplication::processEvents();
    }
    void release(const QPoint point) {
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, point);
        QApplication::processEvents();
    }
    void drag(const QPoint from, const QPoint to) {
        press(from);
        move(QPoint((from.x() + to.x()) / 2, (from.y() + to.y()) / 2));
        move(to);
        release(to);
    }
    void capture(const QString& name) {
        const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        if (directory.isEmpty())
            return;
        QDir().mkpath(directory);
        expect(bloom::test_support::awaitQtCondition(
                   "the preview is ready",
                   [&] { return preview.state().activity == ui::PreviewActivity::Ready; }, kWait),
               "the preview is ready");
        QApplication::processEvents();
        if (!viewer->grab().save(directory + QLatin1Char('/') + name + QStringLiteral("-viewer.png")))
            std::cerr << "could not write " << name.toStdString() << '\n';
    }
};

bool near(const double value, const double expected, const double tolerance) {
    return std::abs(value - expected) <= tolerance;
}

// Twirl: the centre is a point, the radius a ring about it, the fade a dashed ring inside.
void testTwirl() {
    Scene scene(document::kProceduralWarpTwirl);
    scene.set(scene.warp, "radius", 60.0);
    scene.set(scene.warp, "falloff", 50.0);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("procedural-warp-twirl"));
    // The ring: a press on its line, away from the centre and the points, grabs the radius.
    const auto before = scene.stack.size();
    harness.drag(harness.at(0.0, -60.0), harness.at(0.0, -85.0));
    expect(near(scene.real("radius"), 85.0, 1.5),
           "dragging the ring sets the radius: " + std::to_string(scene.real("radius")));
    expect(scene.stack.size() == before + 1, "the ring drag is one command");
    expect(near(scene.real("falloff"), 50.0, 0.0), "the ring drag leaves the falloff");
    harness.capture(QStringLiteral("procedural-warp-twirl-radius"));
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(near(scene.real("radius"), 60.0, 0.0), "one undo restores the radius");
    // The centre handle (inside the ring) moves the centre, an offset from the frame's centre.
    const auto beforeCentre = scene.stack.size();
    harness.drag(harness.at(0.0, 0.0), harness.at(40.0, -20.0));
    const auto centre = scene.point("center");
    expect(near(centre.x, 40.0, 1.5) && near(centre.y, -20.0, 1.5),
           "dragging the centre moves it: " + std::to_string(centre.x) + ", " +
               std::to_string(centre.y));
    static_cast<void>(beforeCentre); // the undo below proves it is one command
    expect(near(scene.real("radius"), 60.0, 0.0), "the centre drag leaves the radius");
    harness.capture(QStringLiteral("procedural-warp-twirl-moved"));
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(near(scene.point("center").x, 0.0, 0.0) && near(scene.point("center").y, 0.0, 0.0),
           "one undo restores the centre");
    // Escape confirms a drag as it stands, one command; the release after it changes nothing; Undo
    // discards it (the Viewer's rule).
    harness.press(harness.at(0.0, 0.0));
    harness.move(harness.at(30.0, 20.0));
    expect(!near(scene.point("center").x, 0.0, 0.5), "the drag edits live");
    QTest::keyClick(harness.viewer, Qt::Key_Escape);
    QApplication::processEvents();
    expect(!scene.session.valueEditActive(), "Escape ends the drag");
    const auto kept = scene.point("center");
    harness.release(harness.at(60.0, 40.0));
    expect(near(scene.point("center").x, kept.x, 0.0), "the release after Escape changes nothing");
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(near(scene.point("center").x, 0.0, 0.0), "Undo discards the drag as one command");
}

// Magnify: the centre, the pull point and the ring (a square for the square shape; with the link the
// ring is the radius times the magnification).
void testMagnify() {
    Scene scene(document::kProceduralWarpMagnify);
    scene.set(scene.warp, "radius", 40.0);
    scene.set(scene.warp, "pullPoint", document::Vec2d{80.0, 0.0});
    scene.set(scene.warp, "pull", 50.0);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("procedural-warp-magnify"));
    const auto before = scene.stack.size();
    harness.drag(harness.at(80.0, 0.0), harness.at(110.0, 30.0));
    const auto pull = scene.point("pullPoint");
    expect(near(pull.x, 110.0, 1.5) && near(pull.y, 30.0, 1.5),
           "dragging the pull point moves it: " + std::to_string(pull.x) + ", " +
               std::to_string(pull.y));
    expect(scene.stack.size() == before + 1, "the pull point drag is one command");
    expect(near(scene.real("pull"), 50.0, 0.0) && near(scene.real("radius"), 40.0, 0.0),
           "the pull point drag leaves the pull and the radius");
    harness.capture(QStringLiteral("procedural-warp-magnify-pulled"));
    // The ring is a square: its edge is the radius along an axis.
    scene.set(scene.warp, "magnifyShape", std::int64_t{1});
    QApplication::processEvents();
    harness.drag(harness.at(0.0, -40.0), harness.at(0.0, -55.0));
    expect(near(scene.real("radius"), 55.0, 1.5),
           "dragging a square Magnify's edge sets the radius: " +
               std::to_string(scene.real("radius")));
    // Linked to the magnification (200%): the ring is twice the radius, and a drag sets the radius
    // that makes it so.
    scene.set(scene.warp, "magnifyLink", std::int64_t{1});
    scene.set(scene.warp, "radius", 30.0);
    QApplication::processEvents();
    harness.capture(QStringLiteral("procedural-warp-magnify-linked"));
    harness.drag(harness.at(0.0, -60.0), harness.at(0.0, -80.0));
    expect(near(scene.real("radius"), 40.0, 1.5),
           "a linked ring is the radius times the magnification: " +
               std::to_string(scene.real("radius")));
}

// Split: the two ends of the line.
void testSplit() {
    Scene scene(document::kProceduralWarpSplit);
    scene.set(scene.warp, "lineStart", document::Vec2d{-60.0, 50.0});
    scene.set(scene.warp, "lineEnd", document::Vec2d{60.0, -50.0});
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("procedural-warp-split"));
    const auto before = scene.stack.size();
    harness.drag(harness.at(-60.0, 50.0), harness.at(-80.0, 20.0));
    const auto start = scene.point("lineStart");
    expect(near(start.x, -80.0, 1.5) && near(start.y, 20.0, 1.5),
           "dragging the line's start moves it");
    expect(scene.stack.size() == before + 1, "the start drag is one command");
    const auto end = scene.point("lineEnd");
    expect(near(end.x, 60.0, 0.0) && near(end.y, -50.0, 0.0), "the start drag leaves the end");
    harness.drag(harness.at(60.0, -50.0), harness.at(70.0, -60.0));
    expect(near(scene.point("lineEnd").x, 70.0, 1.5) && near(scene.point("lineEnd").y, -60.0, 1.5),
           "dragging the line's end moves it");
    harness.capture(QStringLiteral("procedural-warp-split-moved"));
}

// Bend (a centre and no radius), Drizzle (a radius about the middle and no centre).
void testOthers() {
    {
        Scene scene(document::kProceduralWarpBend);
        Harness harness(scene);
        if (!harness.mapping)
            return;
        harness.capture(QStringLiteral("procedural-warp-bend"));
        harness.drag(harness.at(0.0, 0.0), harness.at(-30.0, 25.0));
        expect(near(scene.point("center").x, -30.0, 1.5) && near(scene.point("center").y, 25.0, 1.5),
               "dragging a Bend's centre moves it");
    }
    {
        Scene scene(document::kProceduralWarpDrizzle);
        scene.set(scene.warp, "radius", 50.0);
        Harness harness(scene);
        if (!harness.mapping)
            return;
        harness.capture(QStringLiteral("procedural-warp-drizzle"));
        harness.drag(harness.at(50.0, 0.0), harness.at(75.0, 0.0));
        expect(near(scene.real("radius"), 75.0, 1.5),
               "dragging a Drizzle's ring sets the longest ripple: " +
                   std::to_string(scene.real("radius")));
    }
}

// A Wave has no point and no radius: a press where a handle would be edits nothing.
void testNoHandle() {
    Scene scene(document::kProceduralWarpWave);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    const auto before = scene.stack.size();
    harness.press(harness.at(0.0, 0.0));
    expect(!scene.session.valueEditActive(), "a Wave has no Viewer handle to press");
    harness.release(harness.at(0.0, 0.0));
    harness.press(harness.at(300.0, 0.0));
    expect(!scene.session.valueEditActive(), "a Wave has no ring to grab");
    harness.release(harness.at(300.0, 0.0));
    expect(scene.stack.size() == before, "the presses edit nothing");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    testTwirl();
    testMagnify();
    testSplit();
    testOthers();
    testNoHandle();
    if (failures + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << failures << " warp handle expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: procedural warp viewer handles\n";
    return 0;
}

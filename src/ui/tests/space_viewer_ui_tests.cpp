// #591 the on-Viewer handles of a Coordinate Space on a real document: a plate layer under a selected
// Coordinate Space. Selecting the node draws the points its mode defines it by -- the origin, the
// rectangle's centre or the four corners -- with the range's outline and the axes through its map.
// Dragging a point moves it in composition pixels; each gesture is one command, Escape confirms it,
// and a space defined in another space shows no handles. With BLOOM_GRAMMAR_ARTIFACT_DIR set it
// writes each Viewer to <dir>/<name>-viewer.png.

#include "await_qt.hpp"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QTest>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
// The ceiling for a broken run; a passing wait returns as soon as its condition holds.
constexpr std::chrono::milliseconds kWait{15000};

using namespace bloom;
using document::Vec2d;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;

struct Scene final {
    document::NewProject initial{document::makeNewProject(
        "Space Handles", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)))};
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    document::NodeId space;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Space handles UI", session.snapshot().revision());
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
    document::NodeId add(const std::string_view type) {
        return *run<commands::AddNode>(session.compositionId(), std::string(type), Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }

    explicit Scene(const document::SpaceMode mode) {
        run<commands::AddSolidLayer>(session.compositionId(), "Plate",
                                     core::Color4d{0.3, 0.3, 0.35, 1.0});
        space = add(document::kCoordinateSpaceNodeType);
        set(space, "mode", static_cast<std::int64_t>(mode));
        set(space, "origin", Vec2d{160.0, 90.0});
        set(space, "pixelsPerUnit", Vec2d{20.0, 20.0});
        set(space, "rangeMin", Vec2d{-3.0, -2.0});
        set(space, "rangeMax", Vec2d{3.0, 2.0});
        set(space, "center", Vec2d{150.0, 100.0});
        set(space, "size", Vec2d{180.0, 100.0});
        set(space, "topLeft", Vec2d{90.0, 40.0});
        set(space, "topRight", Vec2d{230.0, 50.0});
        set(space, "bottomRight", Vec2d{250.0, 150.0});
        set(space, "bottomLeft", Vec2d{70.0, 140.0});
    }
    [[nodiscard]] Vec2d point(const std::string& role) {
        const auto id = ui::nodeRoleParameter(session, space, role);
        const auto value = id ? session.liveValue(*id) : std::nullopt;
        const auto* stored = value ? std::get_if<Vec2d>(&*value) : nullptr;
        return stored ? *stored : Vec2d{std::nan(""), std::nan("")};
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

// One Viewer on `scene`, its space selected.
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
        scene.session.selectNode(scene.space);
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
    [[nodiscard]] QPoint at(const Vec2d pixel) const { return mapping->toScreen(pixel).toPoint(); }
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
    // The pixels per composition pixel on screen, to state how close a drag can land.
    [[nodiscard]] double tolerance() const {
        const auto one = mapping->toScreen(Vec2d{0.0, 0.0});
        const auto two = mapping->toScreen(Vec2d{1.0, 0.0});
        return 1.0 / std::max(std::abs(two.x() - one.x()), 1e-9) + 0.05;
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

bool near(const Vec2d value, const Vec2d expected, const double tolerance) {
    return std::abs(value.x - expected.x) <= tolerance && std::abs(value.y - expected.y) <= tolerance;
}
std::string text(const Vec2d value) {
    return std::to_string(value.x) + ", " + std::to_string(value.y);
}

// Origin and scale: the origin is the one handle; the rectangle's centre is not shown.
void testOrigin() {
    Scene scene(document::SpaceMode::Scale);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("coordinate-space-origin"));
    const auto reach = harness.tolerance();
    const auto before = scene.stack.size();
    harness.drag(harness.at({160.0, 90.0}), harness.at({200.0, 70.0}));
    expect(near(scene.point("origin"), {200.0, 70.0}, reach),
           "dragging the origin moves it: " + text(scene.point("origin")));
    expect(scene.stack.size() == before + 1, "the origin drag is one command");
    expect(near(scene.point("pixelsPerUnit"), {20.0, 20.0}, 0.0),
           "the origin drag leaves the scale");
    harness.capture(QStringLiteral("coordinate-space-origin-moved"));
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(near(scene.point("origin"), {160.0, 90.0}, 0.0), "one undo restores the origin");
    // The rectangle's centre is not this mode's: a press there edits nothing.
    harness.press(harness.at({150.0, 100.0}));
    expect(!scene.session.valueEditActive(), "an inactive point has no handle");
    harness.release(harness.at({150.0, 100.0}));
    // That press landed on the plate and selected it: select the space again.
    scene.session.selectNode(scene.space);
    QApplication::processEvents();
    // Escape confirms a drag as it stands, one command; Undo discards it.
    harness.press(harness.at({160.0, 90.0}));
    harness.move(harness.at({190.0, 110.0}));
    expect(!near(scene.point("origin"), {160.0, 90.0}, 0.5), "the drag edits live");
    QTest::keyClick(harness.viewer, Qt::Key_Escape);
    QApplication::processEvents();
    expect(!scene.session.valueEditActive(), "Escape ends the drag");
    const auto kept = scene.point("origin");
    harness.release(harness.at({230.0, 130.0}));
    expect(near(scene.point("origin"), kept, 0.0), "the release after Escape changes nothing");
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(near(scene.point("origin"), {160.0, 90.0}, 0.0), "Undo discards the drag as one command");
}

// Range to rectangle: the centre moves the rectangle.
void testRectangle() {
    Scene scene(document::SpaceMode::Range);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("coordinate-space-rectangle"));
    const auto before = scene.stack.size();
    harness.drag(harness.at({150.0, 100.0}), harness.at({120.0, 80.0}));
    expect(near(scene.point("center"), {120.0, 80.0}, harness.tolerance()),
           "dragging the centre moves the rectangle: " + text(scene.point("center")));
    expect(scene.stack.size() == before + 1 && near(scene.point("size"), {180.0, 100.0}, 0.0),
           "the centre drag is one command and leaves the size");
}

// Range to corners: each corner is its own handle.
void testCorners() {
    Scene scene(document::SpaceMode::Corners);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("coordinate-space-corners"));
    const auto reach = harness.tolerance();
    harness.drag(harness.at({230.0, 50.0}), harness.at({260.0, 30.0}));
    expect(near(scene.point("topRight"), {260.0, 30.0}, reach),
           "dragging the top right corner moves it: " + text(scene.point("topRight")));
    expect(near(scene.point("topLeft"), {90.0, 40.0}, 0.0) &&
               near(scene.point("bottomRight"), {250.0, 150.0}, 0.0) &&
               near(scene.point("bottomLeft"), {70.0, 140.0}, 0.0),
           "the corner drag leaves the other corners");
    harness.drag(harness.at({70.0, 140.0}), harness.at({60.0, 160.0}));
    expect(near(scene.point("bottomLeft"), {60.0, 160.0}, reach), "the bottom left corner moves");
    harness.capture(QStringLiteral("coordinate-space-corners-moved"));
}

// A space defined in another has its points in the parent's units: no handles.
void testParented() {
    Scene scene(document::SpaceMode::Scale);
    const auto parent = scene.add(document::kCoordinateSpaceNodeType);
    scene.run<commands::ConnectPorts>(
        scene.session.compositionId(), document::OutputPortRef{parent, "space"},
        document::NodeInputRef{scene.space, "parent"});
    Harness harness(scene);
    if (!harness.mapping)
        return;
    const auto before = scene.stack.size();
    harness.press(harness.at({160.0, 90.0}));
    expect(!scene.session.valueEditActive(), "a parented space has no Viewer handle");
    harness.release(harness.at({160.0, 90.0}));
    expect(scene.stack.size() == before, "the press edits nothing");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    testOrigin();
    testRectangle();
    testCorners();
    testParented();
    if (failures + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << failures << " coordinate space handle expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: coordinate space viewer handles\n";
    return 0;
}

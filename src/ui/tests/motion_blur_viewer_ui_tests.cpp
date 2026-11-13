// #484 the on-Viewer handles of Directional, Spin and Zoom Blur on a real document: a frame-sized
// Linear Gradient through the blur. Selecting the node draws its handle; dragging the tip sets the
// direction and length, the turn (unwrapped past a whole revolution) or the zoom; dragging Spin's
// or Zoom's centre moves it; each gesture is one command and Escape confirms it. The handles keep
// working from the frame on screen while the new one renders (a press right after an edit). With
// BLOOM_GRAMMAR_ARTIFACT_DIR set it writes each Viewer to <dir>/<name>-viewer.png.

#include "await_qt.hpp"
#include <QApplication>
#include <QDir>

#include <QImage>
#include <QTest>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
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
// 20% of the shorter side, the handles' ring.
constexpr double kRing = 0.2 * kHeight;

struct Scene final {
    document::NewProject initial{document::makeNewProject(
        "Handles", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)))};
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    document::NodeId blur;

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

    explicit Scene(const std::string_view type) {
        const auto composition = session.compositionId();
        const auto layer = run<commands::AddSolidLayer>(composition, "Plate",
                                                        core::Color4d{0.8, 0.6, 0.4, 1.0});
        const auto layerNode =
            *layer.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        blur = *run<commands::AddNode>(composition, std::string(type), document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
        const auto ramp =
            *run<commands::AddNode>(composition, std::string(document::kLinearGradientNodeType),
                                    document::Vec2d{})
                 .outputId<document::NodeId>(commands::kAddNodeOutput);
        set(ramp, "width", static_cast<double>(kWidth));
        set(ramp, "height", static_cast<double>(kHeight));
        set(ramp, "start", document::Vec2d{0.0, 0.0});
        set(ramp, "end", document::Vec2d{static_cast<double>(kWidth), 0.0});
        set(blur, "quality", document::kMotionQualityDraft);
        run<commands::ConnectPorts>(composition, document::OutputPortRef{ramp, "image"},
                                    document::NodeInputRef{blur, "input"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{blur, "image"},
                                    document::NodeInputRef{layerNode, "image"});
    }
    [[nodiscard]] double real(const std::string& role) {
        const auto id = ui::nodeRoleParameter(session, blur, role);
        const auto value = id ? session.liveValue(*id) : std::nullopt;
        const auto* stored = value ? std::get_if<double>(&*value) : nullptr;
        return stored ? *stored : std::nan("");
    }
    [[nodiscard]] document::Vec2d point(const std::string& role) {
        const auto id = ui::nodeRoleParameter(session, blur, role);
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
        scene.session.selectNode(scene.blur);
        QApplication::processEvents();
        expect(bloom::test_support::awaitQtCondition("Viewer shows the composition",
                                                     [&] {
                                                         mapping = viewer->mappingForTest();
                                                         return mapping.has_value() &&
                                                                preview.state().activity ==
                                                                    ui::PreviewActivity::Ready;
                                                     }),
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
                   "preview ready before capture",
                   [&] { return preview.state().activity == ui::PreviewActivity::Ready; }),
               "the preview is ready before the capture is grabbed");
        QApplication::processEvents();
        if (!viewer->grab().save(directory + QLatin1Char('/') + name + QStringLiteral("-viewer.png")))
            std::cerr << "could not write " << name.toStdString() << '\n';
    }
};

bool near(const double value, const double expected, const double tolerance) {
    return std::abs(value - expected) <= tolerance;
}

void testDirectional() {
    Scene scene(document::kDirectionalBlurNodeType);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("directional"));
    // A click away from the handle does nothing to the blur (it selects the layer; select the node
    // again for its handle).
    QTest::mouseClick(harness.viewer, Qt::LeftButton, Qt::NoModifier, harness.at(-100.0, -60.0));
    QApplication::processEvents();
    expect(near(scene.real("length"), 32.0, 0.0) && near(scene.real("angle"), 0.0, 0.0),
           "a click away from the handle leaves the blur alone");
    scene.session.selectNode(scene.blur);
    QApplication::processEvents();
    const auto beforeDrag = scene.stack.size();
    harness.drag(harness.at(32.0, 0.0), harness.at(0.0, 40.0));
    expect(near(scene.real("angle"), 90.0, 2.0) && near(scene.real("length"), 40.0, 1.5),
           "dragging the tip sets the direction and the length");
    expect(scene.stack.size() == beforeDrag + 1, "the drag is one command");
    harness.capture(QStringLiteral("directional-dragged"));
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(near(scene.real("length"), 32.0, 0.0) && near(scene.real("angle"), 0.0, 0.0),
           "one undo restores the direction and the length");
    // Escape confirms a Viewer drag as it stands, one command; Undo discards it (the Viewer's rule).
    harness.press(harness.at(32.0, 0.0));
    harness.move(harness.at(10.0, 60.0));
    expect(!near(scene.real("length"), 32.0, 0.5), "the drag edits live");
    QTest::keyClick(harness.viewer, Qt::Key_Escape);
    QApplication::processEvents();
    expect(!scene.session.valueEditActive(), "Escape ends the drag");
    const auto kept = scene.real("length");
    harness.release(harness.at(40.0, 70.0));
    expect(near(scene.real("length"), kept, 0.0), "the release after Escape changes nothing");
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(near(scene.real("length"), 32.0, 0.0), "Undo discards the drag as one command");
}

void testSpin() {
    Scene scene(document::kSpinBlurNodeType);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("spin"));
    const double turn = 10.0 * std::numbers::pi / 180.0;
    const auto tip = harness.at(kRing * std::sin(turn), -kRing * std::cos(turn));
    const auto before = scene.stack.size();
    harness.drag(tip, harness.at(kRing, 0.0));
    expect(near(scene.real("amount"), 90.0, 3.0), "dragging the tip round sets the turn");
    expect(scene.stack.size() == before + 1, "the drag is one command");
    harness.capture(QStringLiteral("spin-dragged"));
    // Round past the bottom and on past where it started: the turn unwraps instead of jumping.
    harness.press(harness.at(kRing, 0.0));
    harness.move(harness.at(0.0, kRing));
    harness.move(harness.at(-kRing, 0.0));
    harness.move(harness.at(0.0, -kRing));
    harness.move(harness.at(kRing, 0.0));
    harness.release(harness.at(kRing, 0.0));
    expect(near(scene.real("amount"), 450.0, 6.0),
           "a whole turn more unwraps past 360 degrees: " + std::to_string(scene.real("amount")));
    // The centre handle (over everything at the middle of the frame) moves the centre.
    harness.drag(harness.at(0.0, 0.0), harness.at(40.0, -20.0));
    const auto centre = scene.point("centre");
    expect(near(centre.x, 40.0, 1.5) && near(centre.y, -20.0, 1.5),
           "dragging the centre moves it, an offset from the frame's centre: " +
               std::to_string(centre.x) + ", " + std::to_string(centre.y));
}

void testZoom() {
    Scene scene(document::kZoomBlurNodeType);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("zoom"));
    const auto reach = [](const double amount) {
        return kRing * (1.0 + amount) * std::numbers::sqrt2 * 0.5;
    };
    const auto before = scene.stack.size();
    harness.drag(harness.at(reach(0.1), -reach(0.1)), harness.at(reach(0.5), -reach(0.5)));
    expect(near(scene.real("amount"), 0.5, 0.04), "dragging along the ray sets the zoom");
    expect(scene.stack.size() == before + 1, "the drag is one command");
    harness.capture(QStringLiteral("zoom-dragged"));
    // Off the ray only the part along it counts.
    harness.drag(harness.at(reach(0.5), -reach(0.5)),
                 harness.at(reach(0.5) + 15.0, -reach(0.5) + 15.0));
    expect(near(scene.real("amount"), 0.5, 0.04), "a drag across the ray leaves the zoom");
}

void testOtherNodesDrawNoHandle() {
    Scene scene(document::kMotionBlurNodeType);
    Harness harness(scene);
    if (!harness.mapping)
        return;
    const auto shutter = scene.real("shutterAngle");
    harness.press(harness.at(32.0, 0.0));
    expect(!scene.session.valueEditActive(), "a Motion Blur has no Viewer handle to press");
    harness.release(harness.at(32.0, 0.0));
    expect(near(scene.real("shutterAngle"), shutter, 0.0), "the press edits no shutter");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    testDirectional();
    testSpin();
    testZoom();
    testOtherNodesDrawNoHandle();
    failures += bloom::test_support::awaitFailureCount();
    if (failures != 0) {
        std::cerr << failures << " motion handle expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: motion blur viewer handles\n";
    return 0;
}

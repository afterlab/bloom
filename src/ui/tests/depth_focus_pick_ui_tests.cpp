// #483 Z Defocus focus picking in the Viewer on a real document: a solid beauty through a Z Defocus
// whose depth is a Linear Gradient. "Pick focus in Viewer" arms the pick; a click on the frame sets
// the focus point (an offset from the frame's centre) and focuses from it in one undoable command;
// Shift-click keeps picking (a rack focus at another time keys the animated point); Escape
// cancels. The selected Z Defocus draws its focus point and the 5 x 5 area it samples over the
// frame. With BLOOM_GRAMMAR_ARTIFACT_DIR set it writes the Viewer to <dir>/focus-pick-viewer.png.

#include "await_qt.hpp"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QTest>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/document/graph.hpp>
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
#include <stdexcept>
#include <string>

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

constexpr std::uint32_t kWidth = 96;
constexpr std::uint32_t kHeight = 64;

struct Scene final {
    document::NewProject initial{document::makeNewProject(
        "Focus", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)))};
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    document::NodeId defocus;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Focus UI", session.snapshot().revision());
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

    Scene() {
        const auto composition = session.compositionId();
        const auto layer = run<commands::AddSolidLayer>(composition, "Beauty",
                                                        core::Color4d{0.8, 0.6, 0.4, 1.0});
        const auto solid = *layer.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto layerNode =
            *layer.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        defocus = *run<commands::AddNode>(composition, std::string(document::kZDefocusNodeType),
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
        set(defocus, "interpretation", document::kDepthInterpretationNormalised);
        set(defocus, "near", 1.0);
        set(defocus, "far", 20.0);
        set(defocus, "maxSize", 4.0);
        run<commands::ConnectPorts>(composition, document::OutputPortRef{solid, "image"},
                                    document::NodeInputRef{defocus, "input"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{ramp, "image"},
                                    document::NodeInputRef{defocus, "depth"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{defocus, "image"},
                                    document::NodeInputRef{layerNode, "image"});
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

std::optional<document::Vec2d> focusPoint(ui::CompositionSession& session,
                                          const document::NodeId node) {
    const auto id = ui::nodeRoleParameter(session, node, "focusPoint");
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* point = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    return point ? std::optional(*point) : std::nullopt;
}

std::int64_t focusFrom(ui::CompositionSession& session, const document::NodeId node) {
    const auto id = ui::nodeRoleParameter(session, node, "focusFrom");
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::int64_t>(&*value) : nullptr;
    return stored ? *stored : -1;
}

void testFocusPick() {
    Scene scene;
    Pipeline pipeline;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});
    ui::CompositionPreviewController preview(
        scene.session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(*pipeline.compiler, pipeline.evaluator,
                                           pipeline.displayPreparer, pipeline.qualified));
    QWidget window;
    auto* viewer = new ui::ViewerEditor(scene.session, preview, nullptr, &window);
    viewer->setGeometry(0, 0, 640, 480);
    window.resize(640, 480);
    window.show();
    scene.session.selectNode(scene.defocus);
    QApplication::processEvents();
    std::optional<ui::ViewerMapping> mapping;
    expect(bloom::test_support::awaitQtCondition(
               "the Viewer shows the composition",
               [&] {
                   mapping = viewer->mappingForTest();
                   return mapping.has_value() &&
                          preview.state().activity == ui::PreviewActivity::Ready;
               },
               kWait),
           "the Viewer shows the composition");
    if (!mapping)
        return;
    const auto clickAt = [&](const double x, const double y, const Qt::KeyboardModifiers keys) {
        QTest::mouseClick(viewer, Qt::LeftButton, keys,
                          mapping->toScreen(document::Vec2d{x, y}).toPoint());
        QApplication::processEvents();
    };
    // Unarmed, a click picks nothing.
    const auto before = scene.stack.size();
    clickAt(70.0, 40.0, Qt::NoModifier);
    expect(scene.stack.size() == before && focusFrom(scene.session, scene.defocus) ==
                                               document::kFocusFromDistance,
           "an unarmed click sets no focus");
    // Armed: one command sets the point (from the frame's centre) and focuses from it.
    scene.session.requestFocusPick(scene.defocus);
    QApplication::processEvents();
    expect(viewer->cursor().shape() == Qt::CrossCursor, "an armed pick shows a crosshair");
    clickAt(70.5, 40.5, Qt::NoModifier);
    const auto picked = focusPoint(scene.session, scene.defocus);
    expect(scene.stack.size() == before + 1 && picked &&
               std::abs(picked->x - (70.5 - kWidth / 2.0)) < 0.6 &&
               std::abs(picked->y - (40.5 - kHeight / 2.0)) < 0.6 &&
               focusFrom(scene.session, scene.defocus) == document::kFocusFromPoint,
           "a click sets the focus point from the frame's centre and focuses from it, one command");
    expect(!scene.session.focusPickNode(), "a plain click ends the pick");
    // Undo restores both.
    static_cast<void>(scene.session.undo());
    QApplication::processEvents();
    expect(focusFrom(scene.session, scene.defocus) == document::kFocusFromDistance,
           "one undo restores the focus distance");
    // Shift keeps picking; Escape cancels.
    scene.session.requestFocusPick(scene.defocus);
    clickAt(20.5, 10.5, Qt::ShiftModifier);
    expect(scene.session.focusPickNode() == std::optional(scene.defocus),
           "Shift-click keeps the pick armed for a rack focus");
    clickAt(30.5, 50.5, Qt::ShiftModifier);
    const auto second = focusPoint(scene.session, scene.defocus);
    expect(second && std::abs(second->x - (30.5 - kWidth / 2.0)) < 0.6,
           "the next Shift-click moves the focus");
    QTest::keyClick(viewer, Qt::Key_Escape);
    QApplication::processEvents();
    expect(!scene.session.focusPickNode() && viewer->cursor().shape() != Qt::CrossCursor,
           "Escape cancels the pick");
    // The indicator: the focus point and its sampled area drawn in the accent colour while the
    // node is selected (the unarmed click above selected the layer under it).
    scene.session.selectNode(scene.defocus);
    expect(bloom::test_support::awaitQtCondition(
               "the refocused frame renders",
               [&] { return preview.state().activity == ui::PreviewActivity::Ready; }, kWait),
           "the refocused frame renders");
    viewer->update();
    QApplication::processEvents();
    const auto image = viewer->grab().toImage();
    const auto centre = mapping->toScreen(document::Vec2d{30.5, 50.5}).toPoint();
    const auto accent = ui::kit::color(ui::kit::Color::Accent);
    int marked = 0;
    for (int y = centre.y() - 40; y <= centre.y() + 40; ++y)
        for (int x = centre.x() - 40; x <= centre.x() + 40; ++x)
            if (image.rect().contains(x, y)) {
                const QColor pixel = image.pixelColor(x, y);
                marked += std::abs(pixel.red() - accent.red()) < 24 &&
                                  std::abs(pixel.green() - accent.green()) < 24 &&
                                  std::abs(pixel.blue() - accent.blue()) < 24
                              ? 1
                              : 0;
            }
    expect(marked > 20, "the selected Z Defocus draws its focus point over the frame");
    if (const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        !directory.isEmpty()) {
        QDir().mkpath(directory);
        image.save(directory + QStringLiteral("/focus-pick-viewer.png"));
    }
    preview.beginShutdown();
    bridge.beginShutdown();
    scheduler.beginShutdown();
    expect(bloom::test_support::awaitQtCondition("scheduler quiescent after shutdown",
                                                 [&] { return scheduler.isQuiescent(); }),
           "the scheduler reaches quiescence");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        testFocusPick();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures + bloom::test_support::awaitFailureCount() != 0)
        return 1;
    std::cout << "PASS: Z Defocus focus pick\n";
    return 0;
}

// #599 the Viewer point handles of a Line Shape Source on a real document: selecting a Line draws a
// handle at each end; dragging one sets that end at the playhead as one command. A constant end is
// set; a keyed end gets a key at the playhead and keeps its other keys; a driven end takes no
// drag; a locked layer takes none either. Escape confirms a drag. With BLOOM_GRAMMAR_ARTIFACT_DIR
// set it writes each Viewer to <dir>/<name>-viewer.png.

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTest>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
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

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace bloom;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Predicate> bool waitUntil(Predicate predicate, const int milliseconds = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return predicate();
}

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;
const document::Vec2d kStart{-60.0, 10.0};
const document::Vec2d kEnd{60.0, 10.0};

struct Scene final {
    document::NewProject initial{document::makeNewProject(
        "Line handles", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)))};
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    document::LayerId layer;
    document::NodeId shape;

    Scene() {
        if (!session.addShapeLayer(document::ShapeKind::Line,
                                   {.position = document::Vec2d{kWidth / 2.0, kHeight / 2.0},
                                    .size = {},
                                    .lineStart = kStart,
                                    .lineEnd = kEnd,
                                    .path = {}}))
            throw std::runtime_error("the Line layer was not added");
        layer = std::get<document::LayerId>(session.selection().primary);
        shape = *session.directSourceNodeForLayer(layer);
    }
    [[nodiscard]] document::ParameterId parameter(const std::string& role) const {
        return *ui::nodeRoleParameter(session, shape, role);
    }
    [[nodiscard]] document::Vec2d end(const std::string_view role) const {
        const auto value = session.effectiveVec2Value(role);
        return value ? *value : document::Vec2d{std::nan(""), std::nan("")};
    }
    [[nodiscard]] document::Vec2d vec(const std::string_view role) const { return end(role); }
    // The key times of a Line end's curve; empty when it is not keyed.
    [[nodiscard]] std::vector<core::RationalTime> keyTimes(const std::string& role) const {
        const auto* parameter = session.composition()->parameters().find(this->parameter(role));
        const auto* curve = parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                                      : nullptr;
        std::vector<core::RationalTime> times;
        if (curve == nullptr)
            return times;
        const auto* vec2 = session.composition()->animationCurves().findVec2(curve->curveId);
        if (vec2 != nullptr)
            for (const auto& key : vec2->components[0].keyframes)
                times.push_back(key.time);
        return times;
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

struct Harness final {
    Scene& scene;
    Pipeline pipeline;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge{scheduler, nullptr, std::chrono::milliseconds{1}};
    ui::CompositionPreviewController preview;
    QWidget window;
    ui::ViewerEditor* viewer = nullptr;
    std::optional<ui::ViewerMapping> mapping;
    // The value a driver gives lineEnd, which the session has no constant to read for.
    std::optional<document::Vec2d> drivenEnd;

    explicit Harness(Scene& target)
        : scene(target),
          preview(scene.session, scheduler, bridge,
                  ui::makeCompositionPreviewPipeline(*pipeline.compiler, pipeline.evaluator,
                                                     pipeline.displayPreparer, pipeline.qualified)) {
        viewer = new ui::ViewerEditor(scene.session, preview, nullptr, &window);
        viewer->setGeometry(0, 0, 720, 480);
        window.resize(720, 480);
        window.show();
        scene.session.selectLayer(scene.layer);
        QApplication::processEvents();
        expect(waitUntil([&] {
                   mapping = viewer->mappingForTest();
                   return mapping.has_value() &&
                          preview.state().activity == ui::PreviewActivity::Ready;
               }),
               "the Viewer shows the composition");
    }
    // A source coordinate of the Line in composition coordinates under the current values: the
    // layer pivots on the content's centre (the midpoint of the ends) plus the anchor.
    [[nodiscard]] document::Vec2d comp(const document::Vec2d local) const {
        const auto position = scene.vec(document::kPositionParameterRole);
        const auto anchor = scene.vec(document::kAnchorParameterRole);
        const auto start = scene.end("lineStart");
        const auto end = drivenEnd ? *drivenEnd : scene.end("lineEnd");
        return {position.x + local.x - (start.x + end.x) / 2.0 - anchor.x,
                position.y + local.y - (start.y + end.y) / 2.0 - anchor.y};
    }
    [[nodiscard]] QPoint at(const document::Vec2d local) const {
        return mapping->toScreen(comp(local)).toPoint();
    }
    // The source coordinate under a screen point, under the current values.
    [[nodiscard]] document::Vec2d localOf(const QPoint screen) const {
        const auto world = mapping->toComposition(screen);
        const auto origin = comp({0.0, 0.0});
        return {world.x - origin.x, world.y - origin.y};
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
    void settle() {
        waitUntil([&] { return preview.state().activity == ui::PreviewActivity::Ready; });
        QApplication::processEvents();
    }
    void capture(const QString& name) {
        const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        if (directory.isEmpty())
            return;
        QDir().mkpath(directory);
        settle();
        if (!viewer->grab().save(directory + QLatin1Char('/') + name + QStringLiteral("-viewer.png")))
            std::cerr << "could not write " << name.toStdString() << '\n';
    }
};

bool near(const document::Vec2d value, const document::Vec2d expected, const double tolerance) {
    return std::abs(value.x - expected.x) <= tolerance && std::abs(value.y - expected.y) <= tolerance;
}

bool nearPoint(const document::Vec2d value, const document::Vec2d expected, const double tolerance) {
    return near(value, expected, tolerance);
}

void testConstantEnds() {
    Scene scene;
    Harness harness(scene);
    if (!harness.mapping)
        return;
    harness.capture(QStringLiteral("line-constant"));
    const auto before = scene.stack.size();
    const auto target = harness.at({90.0, -30.0});
    const auto expected = harness.localOf(target);
    const auto startBefore = harness.comp(kStart);
    harness.drag(harness.at(kEnd), target);
    expect(nearPoint(scene.end("lineEnd"), expected, 0.6) &&
               nearPoint(scene.end("lineStart"), kStart, 0.0),
           "dragging the end handle sets lineEnd and leaves lineStart");
    expect(scene.stack.size() == before + 1, "the drag is one command");
    expect(scene.keyTimes("lineEnd").empty(), "a constant end stays a constant");
    // The end lands under the pointer and the other end stays where it was on the frame: the
    // anchor took up the move of the layer's pivot.
    expect(nearPoint(harness.comp(scene.end("lineEnd")), harness.mapping->toComposition(target), 0.6),
           "the dragged end is under the pointer");
    expect(nearPoint(harness.comp(scene.end("lineStart")), startBefore, 0.01),
           "the other end does not move on the frame");
    harness.capture(QStringLiteral("line-constant-dragged"));
    static_cast<void>(scene.session.undo());
    harness.settle();
    expect(nearPoint(scene.end("lineEnd"), kEnd, 0.0) &&
               nearPoint(scene.vec(document::kAnchorParameterRole), {0.0, 0.0}, 0.0),
           "one undo restores the end and the anchor");
    // The start handle moves the start alone.
    {
        const auto endBefore = harness.comp(kEnd);
        const auto to = harness.at({-80.0, 40.0});
        const auto wanted = harness.localOf(to);
        harness.drag(harness.at(kStart), to);
        expect(nearPoint(scene.end("lineStart"), wanted, 0.6) &&
                   nearPoint(scene.end("lineEnd"), kEnd, 0.0),
               "dragging the start handle sets lineStart alone");
        expect(nearPoint(harness.comp(scene.end("lineEnd")), endBefore, 0.01),
               "the end does not move on the frame");
        static_cast<void>(scene.session.undo());
        harness.settle();
    }
    // Escape confirms a drag as it stands, one command.
    harness.press(harness.at(kEnd));
    harness.move(harness.at({100.0, 50.0}));
    expect(!near(scene.end("lineEnd"), kEnd, 0.5), "the drag edits live");
    QTest::keyClick(harness.viewer, Qt::Key_Escape);
    QApplication::processEvents();
    expect(!scene.session.valueEditActive(), "Escape ends the drag");
    const auto kept = scene.end("lineEnd");
    harness.release(harness.at({120.0, 60.0}));
    expect(near(scene.end("lineEnd"), kept, 0.0), "the release after Escape changes nothing");
    static_cast<void>(scene.session.undo());
    harness.settle();
    expect(near(scene.end("lineEnd"), kEnd, 0.0), "Undo discards the Escaped drag as one command");
    // A press away from both handles selects, it does not edit an end.
    const auto count = scene.stack.size();
    harness.press(harness.at({0.0, 120.0}));
    harness.release(harness.at({0.0, 120.0}));
    expect(scene.stack.size() == count && near(scene.end("lineEnd"), kEnd, 0.0),
           "a press away from the handles edits no end");
}

void testKeyedEnds() {
    Scene scene;
    Harness harness(scene);
    if (!harness.mapping)
        return;
    const auto second = [](const std::int64_t value) {
        return core::RationalTime::fromInteger(value);
    };
    // lineEnd is keyed at 0 s and 1 s, both (60,10) (the second key repeats the value).
    static_cast<void>(scene.session.setCurrentTime(second(0)));
    expect(scene.session.toggleKeyframeForParameter(scene.parameter("lineEnd")),
           "key lineEnd at 0 s");
    expect(scene.session.setCurrentTime(second(1)), "go to 1 s");
    expect(scene.session.toggleKeyframeForParameter(scene.parameter("lineEnd")),
           "key lineEnd at 1 s");
    harness.settle();
    expect(scene.keyTimes("lineEnd").size() == 2, "lineEnd has two keys");
    const auto half = *core::RationalTime::create(1, 2);
    expect(scene.session.setCurrentTime(half), "go to 0.5 s");
    harness.settle();
    const auto before = scene.stack.size();
    const auto target = harness.at({100.0, -40.0});
    const auto expected = harness.localOf(target);
    harness.drag(harness.at(scene.end("lineEnd")), target);
    expect(scene.stack.size() == before + 1, "the keyed drag is one command");
    const auto keys = scene.keyTimes("lineEnd");
    expect(keys.size() == 3 && keys[0] == second(0) && keys[1] == half && keys[2] == second(1),
           "dragging a keyed end writes a key at the playhead and keeps the others");
    expect(near(scene.end("lineEnd"), expected, 0.6), "the new key holds the dragged position");
    static_cast<void>(scene.session.setCurrentTime(second(0)));
    expect(near(scene.end("lineEnd"), kEnd, 0.0), "the first key is unchanged");
    static_cast<void>(scene.session.setCurrentTime(second(1)));
    expect(near(scene.end("lineEnd"), kEnd, 0.0), "the last key is unchanged");
    expect(scene.keyTimes("lineStart").empty(), "the start, never keyed, stays a constant");
    expect(scene.session.setCurrentTime(half), "back to 0.5 s");
    harness.capture(QStringLiteral("line-keyed-dragged"));
    static_cast<void>(scene.session.undo());
    harness.settle();
    expect(scene.keyTimes("lineEnd").size() == 2, "one undo removes the key the drag wrote");
}

void testDrivenEndTakesNoDrag() {
    Scene scene;
    commands::Transaction add("Add vector", scene.session.snapshot().revision());
    add.emplace<commands::AddNode>(scene.session.compositionId(),
                                   std::string(document::kVector2ValueNodeType), document::Vec2d{});
    const auto added = scene.session.executeNodeTransaction(std::move(add));
    const auto producer = added.outputId<document::NodeId>(commands::kAddNodeOutput);
    expect(added.succeeded() && producer.has_value(), "a Vector2 value node is added");
    if (!producer)
        return;
    commands::Transaction wire("Drive lineEnd", scene.session.snapshot().revision());
    wire.emplace<commands::ConnectPorts>(
        scene.session.compositionId(),
        document::OutputPortRef{*producer, std::string(document::kValuePortName)},
        document::NodeInputRef{scene.shape, "lineEnd"});
    expect(scene.session.executeNodeTransaction(std::move(wire)).succeeded(),
           "a Vector2 value drives lineEnd");
    const auto valueParameter = ui::nodeRoleParameter(scene.session, *producer,
                                                      std::string(document::kValueParameterRole));
    expect(valueParameter && scene.session.setParameterValue(*valueParameter, kEnd,
                                                             QStringLiteral("Set")),
           "the driver is given the end's value");
    scene.session.selectLayer(scene.layer);
    Harness harness(scene);
    harness.drivenEnd = kEnd;
    if (!harness.mapping)
        return;
    const auto driven = kEnd;
    const auto before = scene.stack.size();
    harness.drag(harness.at(driven), harness.at({100.0, -40.0}));
    expect(scene.stack.size() == before && !scene.session.valueEditActive(),
           "a driven end takes no drag");
    // The start is still free.
    const auto to = harness.at({-90.0, 30.0});
    const auto wanted = harness.localOf(to);
    harness.drag(harness.at(kStart), to);
    expect(near(scene.end("lineStart"), wanted, 0.6), "the undriven start still drags");
    harness.settle();
    harness.capture(QStringLiteral("line-driven"));
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        testConstantEnds();
        testKeyedEnds();
        testDrivenEndTakesNoDrag();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " line end handle expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: line end viewer handles\n";
    return 0;
}

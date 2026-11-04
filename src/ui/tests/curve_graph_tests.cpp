// #493: the Curves node's graph editor. A click adds a point and the drag that follows moves it, as
// one command; a press that moves nothing commits nothing; Escape cancels; a double-click or Delete
// removes a point (a channel curve keeps two); the drag is live before it commits; a keyed curve
// takes the drag as a key at the playhead; and the picture's ranges and painting.

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <cmath>
#include <iostream>

using namespace bloom;
using document::GradeCurve;
using document::Vec2d;

namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

document::NodeId addNode(ui::CompositionSession& session, const std::string_view type) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type), Vec2d{});
    const auto result = session.executeNodeTransaction(std::move(transaction));
    return result.outputId<document::NodeId>(commands::kAddNodeOutput).value_or(document::NodeId{});
}

QPointF at(ui::CurveGraphCanvas& canvas, const Vec2d value) {
    return ui::curveGraphPoint(canvas.plotRect(), canvas.range(), value);
}

void mouse(QWidget& widget, const QEvent::Type type, const QPointF position,
           const Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : button;
    QMouseEvent event(type, position, widget.mapToGlobal(position), button,
                      type == QEvent::MouseMove ? Qt::MouseButtons(Qt::LeftButton) : held,
                      Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

void key(QWidget& widget, const int which) {
    QKeyEvent event(QEvent::KeyPress, which, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

bool near(const Vec2d a, const Vec2d b, const double tolerance = 0.02) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}

std::vector<Vec2d> pointsOf(const std::optional<document::PathValue>& value) {
    std::vector<Vec2d> points;
    if (value)
        for (const auto& anchor : value->anchors)
            points.push_back(anchor.point);
    return points;
}

const document::ParameterRecord* recordOf(ui::CompositionSession& session,
                                          const document::ParameterId id) {
    return session.composition()->parameters().find(id);
}

void channelGestures() {
    auto project = document::makeNewProject("Curves", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto node = addNode(session, document::kCurvesNodeType);
    expect(node.isValid(), "Curves is added");
    ui::CurveGraphField field(session, node,
                              {GradeCurve::Master, GradeCurve::Red, GradeCurve::Green,
                               GradeCurve::Blue, GradeCurve::Alpha});
    field.resize(320, field.sizeHint().height());
    field.show();
    QApplication::processEvents();
    auto& canvas = *field.canvas();
    expect(field.tabs()->count() == 5 && canvas.curve() == GradeCurve::Master,
           "the channel graph opens on the RGB master");
    const auto parameter = canvas.parameter();
    expect(parameter.has_value() &&
               pointsOf(canvas.value()) == std::vector<Vec2d>{{0.0, 0.0}, {1.0, 1.0}},
           "the master curve starts as the identity");
    if (!parameter)
        return;
    const auto range = canvas.range();
    expect(range.left == 0.0 && range.bottom == 0.0 && range.top > 1.0 && range.right > 1.0,
           "the graph shows the 0..1 box and HDR range above it");

    // Click to add, drag to move: one command, live while dragging.
    const auto commands = stack.size();
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, {0.5, 0.3}));
    expect(canvas.dragging() && pointsOf(canvas.value()).size() == 3,
           "a click adds a point and starts dragging it");
    mouse(canvas, QEvent::MouseMove, at(canvas, {0.5, 0.7}));
    const auto stored = std::get<document::ConstantValueSource>(recordOf(session, *parameter)->source);
    expect(pointsOf(canvas.value()).size() == 3 && near(pointsOf(canvas.value())[2], {0.5, 0.7}) &&
               stack.size() == commands &&
               std::get<document::PathValue>(stored.value).anchors.size() == 2,
           "the drag is live through the session and commits nothing yet");
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.5, 0.7}));
    expect(stack.size() == commands + 1 && !canvas.dragging(),
           "adding and dragging a point is one command");
    const auto added = pointsOf(canvas.value());
    expect(added.size() == 3 && near(added[2], {0.5, 0.7}), "the point lands where it was dropped");
    expect(session.undo() && pointsOf(canvas.value()).size() == 2 && session.redo() &&
               pointsOf(canvas.value()) == added,
           "the gesture undoes and redoes as one step");

    // A press that does not move commits nothing.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, added[2]));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, added[2]));
    expect(stack.size() == commands + 1 && canvas.selected() == std::optional<std::size_t>{2},
           "pressing a point selects it and commits nothing");

    // Escape cancels a drag back to the stored curve.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, added[2]));
    mouse(canvas, QEvent::MouseMove, at(canvas, {0.6, 0.2}));
    expect(!near(pointsOf(canvas.value())[2], added[2]), "the drag moves the point");
    key(canvas, Qt::Key_Escape);
    expect(!canvas.dragging() && pointsOf(canvas.value()) == added && stack.size() == commands + 1,
           "Escape cancels the drag and commits nothing");
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.6, 0.2}));
    expect(stack.size() == commands + 1, "the release after a cancel commits nothing");

    // Double-click removes a point as one command.
    mouse(canvas, QEvent::MouseButtonDblClick, at(canvas, added[2]));
    expect(pointsOf(canvas.value()).size() == 2 && stack.size() == commands + 2,
           "a double-click removes the point as one command");

    // Delete removes the selected point, but a channel curve keeps two.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, {1.0, 1.0}));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {1.0, 1.0}));
    key(canvas, Qt::Key_Delete);
    expect(pointsOf(canvas.value()).size() == 2 && stack.size() == commands + 2,
           "a channel curve keeps its last two points");

    // Any number of points: a third, a fourth.
    for (const auto& point : {Vec2d{0.25, 0.4}, Vec2d{0.75, 0.6}}) {
        mouse(canvas, QEvent::MouseButtonPress, at(canvas, point));
        mouse(canvas, QEvent::MouseButtonRelease, at(canvas, point));
    }
    expect(pointsOf(canvas.value()).size() == 4 && stack.size() == commands + 4,
           "each click adds a point as its own command");
    key(canvas, Qt::Key_Delete);
    expect(pointsOf(canvas.value()).size() == 3 && stack.size() == commands + 5,
           "Delete removes the selected point as one command");

    // Another tab edits another curve.
    field.setCurve(GradeCurve::Red);
    expect(canvas.curve() == GradeCurve::Red && canvas.parameter() != parameter &&
               pointsOf(canvas.value()).size() == 2,
           "a tab shows its own curve");

    // A keyed curve: the drag at another time adds a key there, as one command.
    field.setCurve(GradeCurve::Master);
    const auto* record = recordOf(session, *parameter);
    expect(record && session.togglePathParameterKey(*record), "the curve is keyed");
    expect(std::holds_alternative<document::AnimationCurveSource>(
               recordOf(session, *parameter)->source),
           "a keyed curve is animated as a whole");
    const auto keyed = pointsOf(canvas.value());
    expect(session.setCurrentTime(core::RationalTime::fromInteger(2)), "the playhead moves");
    const auto before = stack.size();
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, keyed[1]));
    mouse(canvas, QEvent::MouseMove, at(canvas, {0.5, 0.9}));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.5, 0.9}));
    const auto* source =
        std::get_if<document::AnimationCurveSource>(&recordOf(session, *parameter)->source);
    const auto* curve =
        source ? session.composition()->animationCurves().findPath(source->curveId) : nullptr;
    expect(stack.size() == before + 1 && curve && curve->keyframes.size() == 2,
           "a drag on a keyed curve sets a key at the playhead as one command");
    expect(session.setCurrentTime(core::RationalTime::fromInteger(0)) &&
               pointsOf(canvas.value()) == keyed,
           "the earlier key keeps its points");
}

void hueGestures() {
    auto project = document::makeNewProject("Hue", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto node = addNode(session, document::kCurvesNodeType);
    ui::CurveGraphField field(session, node,
                              {GradeCurve::HueVsHue, GradeCurve::HueVsSaturation,
                               GradeCurve::HueVsLuma, GradeCurve::LumaVsSaturation});
    field.resize(320, field.sizeHint().height());
    field.show();
    QApplication::processEvents();
    auto& canvas = *field.canvas();
    expect(field.tabs()->count() == 4 && canvas.curve() == GradeCurve::HueVsHue &&
               pointsOf(canvas.value()).empty(),
           "the hue graph opens on an empty Hue vs Hue");
    const auto range = canvas.range();
    expect(range.left == 0.0 && range.right == 1.0 && range.bottom == -0.5 && range.top == 0.5,
           "Hue vs Hue spans the hue strip and half a turn either way");
    field.setCurve(GradeCurve::HueVsSaturation);
    const auto commands = stack.size();
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, {0.3, 1.5}));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.3, 1.5}));
    expect(pointsOf(canvas.value()).size() == 1 && near(pointsOf(canvas.value())[0], {0.3, 1.5}) &&
               stack.size() == commands + 1,
           "a click adds a hue curve point as one command");
    // A drag past the strip's edge stays on it.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, {0.3, 1.5}));
    mouse(canvas, QEvent::MouseMove, at(canvas, {1.4, 1.5}) + QPointF(40.0, 0.0));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {1.4, 1.5}) + QPointF(40.0, 0.0));
    expect(pointsOf(canvas.value()).size() == 1 && pointsOf(canvas.value())[0].x <= 1.0,
           "a hue point stays on the hue strip");
    key(canvas, Qt::Key_Delete);
    expect(pointsOf(canvas.value()).empty() && stack.size() == commands + 3,
           "a hue curve may be emptied back to the identity");
    canvas.hide();
    expect(!canvas.dragging(), "a hidden graph holds no drag");
}

void pictures() {
    using ui::curveGraphRange;
    const auto hdr = document::gradeCurvePath({{0.0, 0.0}, {2.0, 3.0}});
    const auto range = curveGraphRange(GradeCurve::Red, std::span(&hdr, 1));
    expect(range.top >= 3.0 && range.right >= 2.0 && range.top == range.right,
           "a channel view holds every HDR point and stays square");
    const auto gain = curveGraphRange(GradeCurve::HueVsLuma, {});
    expect(gain.bottom == 0.0 && gain.top == 2.0 && gain.left == 0.0 && gain.right == 1.0,
           "a hue gain view spans 0..2 over the hue strip");
    const QRectF plot(10, 10, 200, 100);
    const auto mapped =
        ui::curveGraphValue(plot, range, ui::curveGraphPoint(plot, range, {0.4, 1.2}));
    expect(near(mapped, {0.4, 1.2}, 1e-9), "values and positions map both ways");
    for (const auto curve : document::kGradeCurves) {
        QImage image(240, 160, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        ui::CurveGraphPicture picture;
        picture.active = curve;
        for (std::size_t index = 0; index < document::kGradeCurveCount; ++index)
            picture.curves[index] = document::defaultGradeCurve(document::kGradeCurves[index]);
        picture.curves[document::gradeCurveIndex(curve)] =
            document::gradeCurvePath({{0.1, document::isHueGradeCurve(curve) &&
                                                    curve == GradeCurve::HueVsHue
                                                ? 0.2
                                                : 0.6},
                                      {0.7, 0.4}});
        picture.histogram.assign(128, 0.5F);
        picture.selected = 0;
        picture.range = curveGraphRange(curve, {});
        ui::paintCurveGraph(painter, QRectF(4, 4, 232, 152), picture);
        painter.end();
        bool drawn = false;
        for (int y = 0; y < image.height() && !drawn; y += 4)
            for (int x = 0; x < image.width() && !drawn; x += 4)
                drawn = qAlpha(image.pixel(x, y)) != 0;
        expect(drawn, "every curve paints its graph");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ui::kit::installKinetikTheme(application);
    channelGestures();
    hueGestures();
    pictures();
    if (failures != 0)
        return 1;
    std::cout << "PASS: curve graph\n";
    return 0;
}

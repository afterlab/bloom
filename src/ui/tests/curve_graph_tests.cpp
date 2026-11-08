// #493, #498: the Curves node's graph editor, on the node card and in Properties. A click adds a
// point and the drag that follows moves it, as one command; a press on a point drags it relative
// to where it was pressed (Ctrl a tenth as fast, and a modifier change mid-drag does not jump);
// dragging a point off the graph removes it; Escape cancels; a double-click resets the curve as
// one step; the right-click menu deletes a point, resets the curve and keys it; the Preset menu
// applies a preset as one command (Reset alone for a hue curve); a keyed curve takes a drag as a
// key at the playhead; the card shows the graph rather than any point list; and the pictures.

#include "node_editor_items.hpp"

#include <QAction>
#include <QApplication>
#include <QGraphicsProxyWidget>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/grade_curve.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <cmath>
#include <iostream>
#include <memory>

using namespace bloom;
using document::GradeCurve;
using document::GradeCurvePreset;
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
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           const Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : button;
    QMouseEvent event(type, position, widget.mapToGlobal(position), button,
                      type == QEvent::MouseMove ? Qt::MouseButtons(Qt::LeftButton) : held,
                      modifiers);
    QApplication::sendEvent(&widget, &event);
}

void click(QWidget& widget, const QPointF position) {
    mouse(widget, QEvent::MouseButtonPress, position);
    mouse(widget, QEvent::MouseButtonRelease, position);
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
        for (const auto& anchor : value->firstContour().anchors)
            points.push_back(anchor.point);
    return points;
}

const document::ParameterRecord* recordOf(ui::CompositionSession& session,
                                          const document::ParameterId id) {
    return session.composition()->parameters().find(id);
}

QAction* action(QMenu& menu, const QString& name) {
    for (auto* candidate : menu.actions())
        if (candidate->objectName() == name)
            return candidate;
    return nullptr;
}

// A card paints nothing outside its own rectangle.
bool paintsInsideCard(ui::node_editor::NodeItem& item) {
    const QRectF card = item.cardRect();
    constexpr int margin = 48;
    QImage image(static_cast<int>(card.width()) + 2 * margin,
                 static_cast<int>(card.height()) + 2 * margin, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.translate(margin, margin);
        QStyleOptionGraphicsItem option;
        option.rect = card.toRect();
        option.exposedRect = card.adjusted(-margin, -margin, margin, margin);
        item.paint(&painter, &option, nullptr);
    }
    const QRectF inside = card.translated(margin, margin).adjusted(-1.0, -1.0, 1.0, 1.0);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (!inside.contains(QPointF(x + 0.5, y + 0.5)) && qAlpha(image.pixel(x, y)) != 0)
                return false;
    return true;
}

// Every shown widget of a card sits inside it, and no label or tab it shows is cut short.
bool cardContentFits(ui::node_editor::NodeItem& item) {
    const QRectF bounds = item.cardRect().adjusted(-0.5, -0.5, 0.5, 0.5);
    bool fits = true;
    for (auto* child : item.childItems()) {
        auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
        if (!child->isVisible() || proxy == nullptr || proxy->widget() == nullptr)
            continue;
        if (!bounds.contains(child->mapRectToParent(child->boundingRect()))) {
            std::cerr << "a card widget lies outside the card\n";
            fits = false;
        }
        for (auto* label : proxy->widget()->findChildren<QLabel*>()) {
            if (!label->isVisible() || label->text().isEmpty())
                continue;
            if (label->text().contains(QChar(0x2026)) ||
                label->width() < label->fontMetrics().horizontalAdvance(label->text())) {
                std::cerr << "label '" << label->text().toStdString() << "' is cut short\n";
                fits = false;
            }
        }
        for (auto* tabs : proxy->widget()->findChildren<ui::kit::KTabBar*>())
            if (tabs->isVisible() && tabs->tabRect(tabs->count() - 1).right() >= tabs->width()) {
                std::cerr << "a tab bar is clipped\n";
                fits = false;
            }
        for (auto* widget : proxy->widget()->findChildren<QWidget*>())
            if (widget->isVisible() && widget->parentWidget() == proxy->widget() &&
                widget->geometry().right() >= proxy->widget()->width()) {
                std::cerr << "a card widget part runs past its right edge\n";
                fits = false;
            }
    }
    return fits;
}

// The card: the widget instead of point lists, and every gesture on the card itself.
void cardGestures() {
    auto project = document::makeNewProject("Curves", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto node = addNode(session, document::kCurvesNodeType);
    expect(node.isValid(), "Curves is added");
    QWidget window;
    auto* properties = new ui::PropertiesEditor(session, &window);
    auto* editor = new ui::NodeGraphEditor(session, &window);
    properties->setGeometry(0, 0, 420, 900);
    editor->setGeometry(420, 0, 1000, 900);
    window.resize(1420, 900);
    window.show();
    session.selectNode(node);
    QApplication::processEvents();

    auto* item = ui::node_editor::nodeItemAncestor(editor->graphScene()->findNodeItem(node));
    auto* card = item ? qobject_cast<ui::CurvesCardWidget*>(item->cardWidget()) : nullptr;
    expect(card != nullptr, "the Curves card draws its graph");
    auto* panel = properties->findChild<ui::CurvesCardWidget*>("propertiesCurvesGraph");
    expect(panel != nullptr, "Properties draws the same graph");
    if (!card || !panel || !item)
        return;
    QApplication::processEvents();

    // No point lists: the only row the card shows is Clamp, and no text names anchors or points.
    int rows = 0;
    bool listed = false;
    for (auto* child : item->childItems())
        if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
            proxy && proxy->isVisible() && proxy->widget() && proxy->widget() != card) {
            ++rows;
            for (auto* label : proxy->widget()->findChildren<QLabel*>())
                listed = listed || label->text().contains(QStringLiteral("anchor"));
        }
    expect(rows == 1 && !listed, "the card shows the graph and Clamp, not the curves' points");
    expect(paintsInsideCard(*item), "the Curves card paints only inside it");
    expect(cardContentFits(*item), "the Curves card's content fits it, nothing elided");
    expect(card->tabs()->count() == 6 && card->curve() == GradeCurve::Master,
           "the card opens on the RGB master, with a tab per channel and one for the hue curves");
    expect(!card->diamond()->isHidden() && card->input()->isVisible() &&
               card->output()->isVisible(),
           "the curve's diamond and the selected point's numbers are on the card itself");

    auto& canvas = *card->canvas();
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
           "a click on the card's graph adds a point and starts dragging it");
    mouse(canvas, QEvent::MouseMove, at(canvas, {0.5, 0.7}));
    const auto stored =
        std::get<document::ConstantValueSource>(recordOf(session, *parameter)->source);
    expect(pointsOf(canvas.value()).size() == 3 && near(pointsOf(canvas.value())[2], {0.5, 0.7}) &&
               stack.size() == commands &&
               std::get<document::PathValue>(stored.value).firstContour().anchors.size() == 2,
           "the drag is live through the session and commits nothing yet");
    expect(pointsOf(panel->canvas()->value()).size() == 3,
           "Properties follows the card's drag before it commits");
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.5, 0.7}));
    expect(stack.size() == commands + 1 && !canvas.dragging(),
           "adding and dragging a point is one command");
    const auto added = pointsOf(canvas.value());
    expect(added.size() == 3 && near(added[2], {0.5, 0.7}), "the point lands where it was dropped");
    expect(canvas.selected() == std::optional<std::size_t>{2} &&
               std::abs(card->output()->value() - 0.7) < 0.02,
           "the selected point's output is shown under the graph");
    expect(session.undo() && pointsOf(canvas.value()).size() == 2 && session.redo() &&
               pointsOf(canvas.value()) == added,
           "the gesture undoes and redoes as one step");

    // A press off a point's centre does not jump it, and a press that moves nothing commits
    // nothing.
    const auto grab = at(canvas, added[2]) + QPointF(6.0, -5.0);
    mouse(canvas, QEvent::MouseButtonPress, grab);
    mouse(canvas, QEvent::MouseMove, grab);
    expect(near(pointsOf(canvas.value())[2], added[2], 1e-9),
           "pressing a point off its centre does not jump it");
    mouse(canvas, QEvent::MouseButtonRelease, grab);
    expect(stack.size() == commands + 1 && canvas.selected() == std::optional<std::size_t>{2},
           "pressing a point selects it and commits nothing");

    // Relative drag, then Ctrl for a tenth of the speed: the change of modifier does not jump.
    const auto start = at(canvas, added[2]);
    const auto step = at(canvas, {0.6, 0.7}) - at(canvas, {0.5, 0.7});
    mouse(canvas, QEvent::MouseButtonPress, start);
    mouse(canvas, QEvent::MouseMove, start + step);
    const auto coarse = pointsOf(canvas.value())[2];
    mouse(canvas, QEvent::MouseMove, start + step, Qt::ControlModifier);
    expect(near(pointsOf(canvas.value())[2], coarse, 1e-9),
           "pressing Ctrl mid-drag does not jump the point");
    mouse(canvas, QEvent::MouseMove, start + 2.0 * step, Qt::ControlModifier);
    const auto fine = pointsOf(canvas.value())[2];
    expect(near(coarse, {0.6, 0.7}) && std::abs(fine.x - 0.61) < 0.005,
           "Ctrl moves the point a tenth as far as the pointer");
    mouse(canvas, QEvent::MouseMove, start + 2.0 * step);
    expect(near(pointsOf(canvas.value())[2], fine, 1e-9),
           "releasing Ctrl mid-drag does not jump the point");
    mouse(canvas, QEvent::MouseButtonRelease, start + 2.0 * step);
    expect(stack.size() == commands + 2, "the drag is one command");
    const auto moved = pointsOf(canvas.value());

    // Escape cancels a drag back to the stored curve.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, moved[2]));
    mouse(canvas, QEvent::MouseMove, at(canvas, {0.2, 0.2}));
    key(canvas, Qt::Key_Escape);
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.2, 0.2}));
    expect(!canvas.dragging() && pointsOf(canvas.value()) == moved && stack.size() == commands + 2,
           "Escape cancels the drag and commits nothing");

    // Dragging a point off the graph removes it; dragging it back restores it.
    const auto outside = QPointF(canvas.width() + 40.0, at(canvas, moved[2]).y());
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, moved[2]));
    mouse(canvas, QEvent::MouseMove, outside);
    expect(pointsOf(canvas.value()).size() == 2, "a point dragged off the graph is removed");
    mouse(canvas, QEvent::MouseMove, at(canvas, moved[2]));
    expect(pointsOf(canvas.value()).size() == 3, "dragging it back restores it");
    mouse(canvas, QEvent::MouseMove, outside);
    mouse(canvas, QEvent::MouseButtonRelease, outside);
    expect(pointsOf(canvas.value()).size() == 2 && stack.size() == commands + 3,
           "releasing off the graph removes the point as one command");
    // A channel curve keeps two points: its ends cannot be dragged off.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, {1.0, 1.0}));
    mouse(canvas, QEvent::MouseMove, outside);
    mouse(canvas, QEvent::MouseButtonRelease, outside);
    expect(pointsOf(canvas.value()).size() == 2, "a channel curve keeps its last two points");
    const auto commandsNow = stack.size();

    // The right-click menu: Delete Point on a point, Reset Curve, and the curve's key.
    click(canvas, at(canvas, {0.3, 0.5}));
    const auto withPoint = pointsOf(canvas.value());
    expect(withPoint.size() == 3 && stack.size() == commandsNow + 1, "a third point is added");
    {
        const std::unique_ptr<QMenu> menu(canvas.makeContextMenu(at(canvas, withPoint[2])));
        auto* remove = action(*menu, QStringLiteral("curveGraphDeletePoint"));
        expect(remove && remove->isEnabled() && action(*menu, QStringLiteral("curveGraphReset")) &&
                   action(*menu, QStringLiteral("curveGraphKey")),
               "the right-click menu offers Delete Point, Reset Curve and a key");
        if (remove)
            remove->trigger();
    }
    expect(pointsOf(canvas.value()).size() == 2 && stack.size() == commandsNow + 2,
           "Delete Point removes the point as one command");
    {
        const std::unique_ptr<QMenu> menu(canvas.makeContextMenu(at(canvas, {0.5, 0.9})));
        auto* remove = action(*menu, QStringLiteral("curveGraphDeletePoint"));
        expect(remove && !remove->isEnabled(), "Delete Point needs a point under the pointer");
    }
    // A double-click is not a delete: on a point it resets the curve.
    click(canvas, at(canvas, {0.3, 0.5}));
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, {0.3, 0.5}));
    mouse(canvas, QEvent::MouseMove, at(canvas, {0.3, 0.6}));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.3, 0.6}));
    expect(pointsOf(canvas.value()).size() == 3, "the curve has a third point to reset");
    const auto beforeReset = stack.size();
    const auto shaped = pointsOf(canvas.value());
    mouse(canvas, QEvent::MouseButtonDblClick, at(canvas, shaped[2]));
    expect(pointsOf(canvas.value()) == std::vector<Vec2d>{{0.0, 0.0}, {1.0, 1.0}} &&
               stack.size() == beforeReset + 1,
           "a double-click on the graph resets the curve as one command");
    expect(session.undo() && pointsOf(canvas.value()) == shaped && session.redo(),
           "the reset undoes as one step");
    // A double-click on the background: its first click adds a point, which the reset takes back,
    // so undoing the reset returns the curve as it was before the double-click.
    expect(session.undo(), "back to the shaped curve");
    click(canvas, at(canvas, {0.8, 0.3}));
    mouse(canvas, QEvent::MouseButtonDblClick, at(canvas, {0.8, 0.3}));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, {0.8, 0.3}));
    expect(pointsOf(canvas.value()) == std::vector<Vec2d>{{0.0, 0.0}, {1.0, 1.0}} &&
               stack.undoLabel() == std::optional<std::string_view>{"Reset Curve"} &&
               !stack.canRedo(),
           "a double-click on the background resets the curve as one command");
    expect(session.undo() && pointsOf(canvas.value()) == shaped,
           "undoing the reset leaves no point from the double-click's first click");

    // Presets: one command each, on the active curve.
    card->setCurve(GradeCurve::Red);
    expect(canvas.curve() == GradeCurve::Red && card->tabs()->currentIndex() == 1,
           "the R tab shows the red curve");
    {
        const std::unique_ptr<QMenu> menu(card->makePresetMenu());
        expect(menu->actions().size() == static_cast<int>(document::kGradeCurvePresets.size()),
               "a channel curve offers every preset");
        if (auto* contrast =
                action(*menu, QStringLiteral("curvesGraphPreset.%1")
                                  .arg(static_cast<int>(GradeCurvePreset::MediumContrast))))
            contrast->trigger();
        expect(
            stack.undoLabel() == std::optional<std::string_view>{"Curve Preset Medium Contrast"} &&
                canvas.value() ==
                    document::gradeCurvePreset(GradeCurve::Red, GradeCurvePreset::MediumContrast),
            "Medium Contrast applies to the active curve as one command");
        expect(session.undo() && canvas.value() == document::defaultGradeCurve(GradeCurve::Red) &&
                   session.redo(),
               "the preset undoes as one step");
    }
    card->setCurve(GradeCurve::HueVsSaturation);
    expect(card->tabs()->currentIndex() == 5 && card->hueCurves()->isVisible() &&
               card->hueCurves()->currentText() == QStringLiteral("Hue vs Saturation"),
           "the Hue tab names the hue curve it shows");
    {
        const std::unique_ptr<QMenu> menu(card->makePresetMenu());
        expect(menu->actions().size() == 1 && menu->actions().front()->text() == "Reset",
               "a hue curve offers Reset alone");
    }
    expect(paintsInsideCard(*item) && cardContentFits(*item),
           "the hue curve's card fits, nothing elided");

    // A keyed curve: the drag at another time adds a key there, as one command.
    card->setCurve(GradeCurve::Master);
    const auto* record = recordOf(session, *parameter);
    {
        const std::unique_ptr<QMenu> menu(canvas.makeContextMenu(QPointF{}));
        auto* keyAction = action(*menu, QStringLiteral("curveGraphKey"));
        expect(keyAction && keyAction->text() == "Add Key", "the menu offers a key");
        if (keyAction)
            keyAction->trigger();
    }
    record = recordOf(session, *parameter);
    expect(record && std::holds_alternative<document::AnimationCurveSource>(record->source),
           "a keyed curve is animated as a whole");
    const auto keyed = pointsOf(canvas.value());
    expect(session.setCurrentTime(core::RationalTime::fromInteger(2)), "the playhead moves");
    const auto before = stack.size();
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, keyed[1]));
    mouse(canvas, QEvent::MouseMove, at(canvas, keyed[1]) + QPointF(-20.0, 10.0));
    mouse(canvas, QEvent::MouseButtonRelease, at(canvas, keyed[1]) + QPointF(-20.0, 10.0));
    const auto* source =
        std::get_if<document::AnimationCurveSource>(&recordOf(session, *parameter)->source);
    const auto* curve =
        source ? session.composition()->animationCurves().findPath(source->curveId) : nullptr;
    expect(stack.size() == before + 1 && curve && curve->keyframes.size() == 2,
           "a drag on a keyed curve sets a key at the playhead as one command");
    expect(session.setCurrentTime(core::RationalTime::fromInteger(0)) &&
               pointsOf(canvas.value()) == keyed,
           "the earlier key keeps its points");
    // #498: the curve's keys select, move and delete like any other animated parameter's.
    if (curve && curve->keyframes.size() == 2) {
        const auto curveId = source->curveId;
        const auto second = curve->keyframes[1].id;
        session.selectKeyframe(curveId, second);
        expect(session.moveKeyframes({{{curveId, second}, core::RationalTime::fromInteger(4)}},
                                     session.snapshot().revision()),
               "a curve key moves in time");
        const auto* shifted = session.composition()->animationCurves().findPath(curveId);
        expect(shifted && shifted->keyframes.size() == 2 &&
                   shifted->keyframes[1].time == core::RationalTime::fromInteger(4),
               "the moved key lands at its new time");
        session.selectKeyframe(curveId, second);
        expect(session.deleteSelectedKeyframes(), "a selected curve key deletes");
        const auto* left = session.composition()->animationCurves().findPath(curveId);
        expect(left && left->keyframes.size() == 1, "one key is left");
    }
}

void hueGestures() {
    auto project = document::makeNewProject("Hue", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto node = addNode(session, document::kCurvesNodeType);
    ui::CurvesCardWidget widget(session, node, false);
    widget.resize(widget.sizeHint());
    widget.show();
    QApplication::processEvents();
    widget.setCurve(GradeCurve::HueVsHue);
    auto& canvas = *widget.canvas();
    expect(canvas.curve() == GradeCurve::HueVsHue && pointsOf(canvas.value()).empty(),
           "the hue tab opens on an empty Hue vs Hue");
    const auto range = canvas.range();
    expect(range.left == 0.0 && range.right == 1.0 && range.bottom == -0.5 && range.top == 0.5,
           "Hue vs Hue spans the hue strip and half a turn either way");
    widget.setCurve(GradeCurve::HueVsSaturation);
    const auto commands = stack.size();
    click(canvas, at(canvas, {0.3, 1.5}));
    expect(pointsOf(canvas.value()).size() == 1 && near(pointsOf(canvas.value())[0], {0.3, 1.5}) &&
               stack.size() == commands + 1,
           "a click adds a hue curve point as one command");
    expect(std::abs(widget.input()->value() - 0.3 * 360.0) < 4.0 &&
               widget.input()->unit() == QStringLiteral("°") &&
               widget.output()->unit() == QStringLiteral("×"),
           "a hue point shows its hue in degrees and its gain as a multiplier");
    // A drag past the strip's edge stays on it.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, {0.3, 1.5}));
    // Just past the strip's edge, but not far enough off the graph to remove the point.
    const auto edge = at(canvas, {1.0, 1.5}) + QPointF(8.0, 0.0);
    mouse(canvas, QEvent::MouseMove, at(canvas, {0.95, 1.5}));
    mouse(canvas, QEvent::MouseMove, edge);
    mouse(canvas, QEvent::MouseButtonRelease, edge);
    expect(pointsOf(canvas.value()).size() == 1 && pointsOf(canvas.value())[0].x <= 1.0,
           "a hue point stays on the hue strip");
    // A hue curve may be emptied: by Delete, or by dragging its point off.
    mouse(canvas, QEvent::MouseButtonPress, at(canvas, pointsOf(canvas.value())[0]));
    mouse(canvas, QEvent::MouseMove, QPointF(-60.0, -60.0));
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(-60.0, -60.0));
    expect(pointsOf(canvas.value()).empty() && stack.size() == commands + 3,
           "a hue curve may be emptied back to the identity");
    canvas.hide();
    expect(!canvas.dragging(), "a hidden graph holds no drag");
}

void presets() {
    for (const auto preset : document::kGradeCurvePresets) {
        const auto value = document::gradeCurvePreset(GradeCurve::Master, preset);
        expect(document::isGradeCurveValueValid(GradeCurve::Master, value),
               "every preset is a valid channel curve");
        const auto shape = runtime::prepareGradeCurveShape(GradeCurve::Master, value);
        expect(shape.has_value(), "every preset prepares");
        if (!shape)
            continue;
        const auto y = [&](const double x) { return runtime::sampleGradeCurve(*shape, x); };
        switch (preset) {
        case GradeCurvePreset::Linear:
            expect(value == document::defaultGradeCurve(GradeCurve::Master),
                   "Linear is the default diagonal");
            break;
        case GradeCurvePreset::MediumContrast:
        case GradeCurvePreset::StrongContrast:
            expect(y(0.25) < 0.25 && y(0.75) > 0.75 && std::abs(y(0.5) - 0.5) < 1e-6,
                   "a contrast preset is an S-curve through mid grey");
            break;
        case GradeCurvePreset::LowContrast:
            expect(y(0.25) > 0.25 && y(0.75) < 0.75, "Low Contrast is an inverse S-curve");
            break;
        case GradeCurvePreset::Lighten:
            expect(y(0.5) > 0.5 && y(0.0) == 0.0 && y(1.0) == 1.0, "Lighten raises the midtones");
            break;
        case GradeCurvePreset::Darken:
            expect(y(0.5) < 0.5 && y(0.0) == 0.0 && y(1.0) == 1.0, "Darken lowers the midtones");
            break;
        case GradeCurvePreset::Fade:
            expect(y(0.0) > 0.0 && y(1.0) < 1.0, "Fade lifts the blacks and softens the whites");
            break;
        case GradeCurvePreset::CrushBlacks:
            expect(y(0.05) == 0.0 && y(1.0) == 1.0, "Crush Blacks takes the shadows to black");
            break;
        case GradeCurvePreset::Invert:
            expect(y(0.0) == 1.0 && y(1.0) == 0.0, "Invert swaps black and white");
            break;
        }
        // Past 1 a channel curve continues in a straight line.
        expect(std::isfinite(y(2.0)), "a preset extends past 1");
        expect(document::gradeCurvePreset(GradeCurve::HueVsLuma, preset)
                   .firstContour()
                   .anchors.empty(),
               "a hue curve's only preset is its empty default");
    }
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
    expect(ui::curveGraphOutputName(GradeCurve::HueVsHue) == "Hue shift" &&
               ui::curveGraphInputName(GradeCurve::LumaVsSaturation) == "Luma",
           "the hue curves name their axes");
    for (const auto curve : document::kGradeCurves) {
        QImage image(240, 160, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        ui::CurveGraphPicture picture;
        picture.active = curve;
        for (std::size_t index = 0; index < document::kGradeCurveCount; ++index)
            picture.curves[index] = document::defaultGradeCurve(document::kGradeCurves[index]);
        picture.curves[document::gradeCurveIndex(curve)] = document::gradeCurvePath(
            {{0.1, curve == GradeCurve::HueVsHue ? 0.2 : 0.6}, {0.7, 0.4}});
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
        if (!document::isChannelGradeCurve(curve)) {
            // The axis bands: the left band's top and bottom differ (a gradient up the side).
            const auto top = image.pixelColor(6, 10);
            const auto bottom = image.pixelColor(6, 140);
            expect(top != bottom, "a hue curve's output axis draws its colour band");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ui::kit::installKinetikTheme(application);
    cardGestures();
    hueGestures();
    presets();
    pictures();
    if (failures != 0)
        return 1;
    std::cout << "PASS: curve graph\n";
    return 0;
}

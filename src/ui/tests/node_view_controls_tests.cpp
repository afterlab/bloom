// #695: the node editor's view controls. The footer's zoom out / zoom value / zoom in, the minimap
// overlay in the canvas corner, and the two Preferences that switch them. Zooming and panning are
// driven through the controls themselves (clicks, a drag on the value, a press and drag in the
// minimap), never through the view's own functions.

#include "node_editor_minimap.hpp"
#include "node_interaction_test_support.hpp"

#include <QLineEdit>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <cmath>

namespace bloom::ui::test {
namespace {
bool near(const double a, const double b, const double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

void sendMouse(QWidget& target, const QEvent::Type type, const QPointF local,
               const Qt::MouseButtons buttons) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, local, target.mapToGlobal(local.toPoint()), button, buttons,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &event);
}

struct Controls final {
    kit::KIconButton* out = nullptr;
    kit::KValueField* value = nullptr;
    kit::KIconButton* in = nullptr;
};

Controls findControls(QWidget& footer) {
    return {footer.findChild<kit::KIconButton*>(QStringLiteral("nodeZoomOutButton")),
            footer.findChild<kit::KValueField*>(QStringLiteral("nodeZoomValue")),
            footer.findChild<kit::KIconButton*>(QStringLiteral("nodeZoomInButton"))};
}

QPointF visibleCenter(NodeGraphicsView& view) {
    return view.mapToScene(view.viewport()->rect()).boundingRect().center();
}

void testZoomControls() {
    Fixture f;
    auto* footer = f.editor.footerWidgetForTest();
    footer->resize(700, 40);
    footer->show();
    QCoreApplication::processEvents();
    const auto controls = findControls(*footer);
    expect(controls.out && controls.value && controls.in, "the footer carries the zoom group");
    if (!controls.out || !controls.value || !controls.in)
        return;
    expect(footer->findChild<QWidget*>(QStringLiteral("nodeZoomDropdown")) == nullptr,
           "the zoom dropdown is gone");
    expect(controls.out->height() == kit::px(kit::Size::Control) &&
               controls.in->height() == kit::px(kit::Size::Control) &&
               controls.value->height() == kit::px(kit::Size::Control),
           "the zoom controls are Size::Control tall");
    auto& view = *f.view();

    // The buttons: exactly one canvas step each way.
    const double base = view.zoomFactor();
    controls.in->click();
    expect(near(view.zoomFactor(), base * kZoomStepFactor), "Zoom In steps by kZoomStepFactor");
    expect(near(controls.value->value(), view.zoomFactor() * 100.0) &&
               controls.value->displayedValue() ==
                   QString::number(std::lround(view.zoomFactor() * 100.0)),
           "the value follows the button");
    controls.out->click();
    expect(near(view.zoomFactor(), base), "Zoom Out steps back by exactly one step");
    controls.out->click();
    expect(near(view.zoomFactor(), base / kZoomStepFactor), "Zoom Out steps by kZoomStepFactor");
    controls.in->click();

    // Dragging the value up zooms in continuously; dragging down zooms out; the view's center
    // stays put.
    view.zoomToPercent(100);
    QCoreApplication::processEvents();
    const QPointF center = visibleCenter(view);
    const double threshold = QApplication::startDragDistance();
    const QPointF start(controls.value->width() / 2.0, controls.value->height() / 2.0);
    sendMouse(*controls.value, QEvent::MouseButtonPress, start, Qt::LeftButton);
    double previous = view.zoomFactor();
    bool monotonic = true;
    for (int step = 1; step <= 6; ++step) {
        sendMouse(*controls.value, QEvent::MouseMove, start - QPointF(0, step * 6.0),
                  Qt::LeftButton);
        monotonic = monotonic && view.zoomFactor() >= previous;
        previous = view.zoomFactor();
    }
    expect(monotonic && view.zoomFactor() > 1.0, "dragging the value up zooms in continuously");
    expect(near(view.zoomFactor(), (100.0 + 36.0 - threshold) / 100.0, 1e-6),
           "one percent per pixel past the drag threshold");
    expect(near(controls.value->value(), view.zoomFactor() * 100.0),
           "the value shows the zoom while it is dragged");
    sendMouse(*controls.value, QEvent::MouseMove, start + QPointF(0, 20.0), Qt::LeftButton);
    expect(near(view.zoomFactor(), (100.0 - (20.0 - threshold)) / 100.0, 1e-6),
           "dragging the value down zooms out");
    sendMouse(*controls.value, QEvent::MouseButtonRelease, start + QPointF(0, 20.0), Qt::NoButton);
    expect(near(visibleCenter(view).x(), center.x(), 1e-6) &&
               near(visibleCenter(view).y(), center.y(), 1e-6),
           "zooming by the value holds the view center");

    // Typing a value.
    sendMouse(*controls.value, QEvent::MouseButtonPress, start, Qt::LeftButton);
    sendMouse(*controls.value, QEvent::MouseButtonRelease, start, Qt::NoButton);
    expect(controls.value->isEditing(), "a click on the value opens it for typing");
    if (controls.value->lineEdit() != nullptr) {
        controls.value->lineEdit()->setText(QStringLiteral("250"));
        QTest::keyClick(controls.value->lineEdit(), Qt::Key_Return);
    }
    expect(near(view.zoomFactor(), 2.5), "a typed value sets the zoom");

    // Presets live in the value's actions.
    auto* preset = controls.value->findChild<QAction*>(QStringLiteral("nodeZoomPreset50Action"));
    expect(preset != nullptr, "the value offers the zoom presets");
    if (preset != nullptr) {
        preset->trigger();
        expect(near(view.zoomFactor(), 0.5), "a preset sets the zoom");
    }
    auto* fit = controls.value->findChild<QAction*>(QStringLiteral("nodeZoomFitPresetAction"));
    expect(fit != nullptr, "the value offers Fit");
    if (fit != nullptr) {
        fit->trigger();
        expect(!view.viewAdjusted(), "Fit keeps framing the graph");
    }

    // The limits are the canvas's own.
    controls.value->setValue(controls.value->maximum());
    view.zoomToFactor(1000.0);
    expect(near(view.zoomFactor(), ViewTransform::kMaxZoom) && !controls.in->isEnabled(),
           "Zoom In stops at the canvas maximum");
    view.zoomToFactor(0.0001);
    expect(near(view.zoomFactor(), ViewTransform::kMinZoom) && !controls.out->isEnabled(),
           "Zoom Out stops at the canvas minimum");
}

void testMinimapPan() {
    Fixture f;
    f.add(document::kSolidSourceNodeType, {0, 100});
    f.add(document::kLayerOutputNodeType, {1800, 900});
    const auto selected = f.add(document::kSolidSourceNodeType, {900, 1600});
    f.session.selectNodes({selected}, selected);
    QCoreApplication::processEvents();
    auto& view = *f.view();
    auto* minimap = f.editor.minimapForTest();
    expect(minimap != nullptr && minimap->parentWidget() == view.viewport(),
           "the minimap is a child of the canvas viewport");
    if (minimap == nullptr)
        return;
    view.zoomToPercent(100);
    QCoreApplication::processEvents();
    expect(!minimap->isHidden(), "the minimap shows for a non-empty graph");
    expect(minimap->cardRects().size() >= 3, "the minimap caches every card");
    expect(minimap->width() == kit::px(kit::Size::NodeMinimapWidth) &&
               minimap->height() == kit::px(kit::Size::NodeMinimapHeight),
           "the minimap takes its size from the tokens");
    const QRect corner = minimap->geometry();
    expect(corner.right() < view.viewport()->width() &&
               corner.bottom() < view.viewport()->height() &&
               corner.right() > view.viewport()->width() - 2 * kit::px(kit::Spacing::S) - 1 &&
               corner.bottom() > view.viewport()->height() - 2 * kit::px(kit::Spacing::S) - 1,
           "the minimap sits in the bottom-right corner");

    // The viewport rectangle is the visible scene rectangle.
    const QRectF visible = view.mapToScene(view.viewport()->rect()).boundingRect();
    expect(near(minimap->visibleSceneRect().width(), visible.width(), 1e-3) &&
               near(minimap->visibleSceneRect().height(), visible.height(), 1e-3),
           "the minimap reads the visible scene rectangle");
    const QRectF mapped = minimap->viewportRect();
    expect(near(mapped.width() / mapped.height(), visible.width() / visible.height(), 1e-6) &&
               mapped.width() > 0.0 && mapped.width() < minimap->width(),
           "the viewport rectangle is smaller than the graph and keeps the canvas's aspect");
    const QPointF back = minimap->toScene(mapped.center());
    expect(near(back.x(), visible.center().x(), 1e-3) && near(back.y(), visible.center().y(), 1e-3),
           "the viewport rectangle sits at the visible center");
    view.zoomToFactor(view.zoomFactor() * 2.0);
    expect(minimap->viewportRect().width() < mapped.width(),
           "zooming in shrinks the viewport rectangle");
    view.zoomToFactor(view.zoomFactor() / 2.0);

    // A press and drag pans the view so the rectangle follows the pointer.
    const auto revision = f.session.snapshot().revision();
    const double zoom = view.zoomFactor();
    const QPointF first(minimap->width() * 0.25, minimap->height() * 0.30);
    const QPointF wanted = minimap->toScene(first);
    sendMouse(*minimap, QEvent::MouseButtonPress, first, Qt::LeftButton);
    expect(near(visibleCenter(view).x(), wanted.x(), 1e-3) &&
               near(visibleCenter(view).y(), wanted.y(), 1e-3),
           "pressing centers the view on the scene point under the pointer");
    const QPointF second(minimap->width() * 0.70, minimap->height() * 0.65);
    const QPointF wantedSecond = minimap->toScene(second);
    sendMouse(*minimap, QEvent::MouseMove, QPointF(minimap->width() * 0.5, minimap->height() * 0.5),
              Qt::LeftButton);
    sendMouse(*minimap, QEvent::MouseMove, second, Qt::LeftButton);
    expect(near(visibleCenter(view).x(), wantedSecond.x(), 1e-3) &&
               near(visibleCenter(view).y(), wantedSecond.y(), 1e-3),
           "dragging keeps the viewport rectangle under the pointer");
    const QPointF held = minimap->viewportRect().center();
    expect(near(held.x(), second.x(), 1e-3) && near(held.y(), second.y(), 1e-3),
           "the painted rectangle follows the pointer");
    sendMouse(*minimap, QEvent::MouseButtonRelease, second, Qt::NoButton);
    sendMouse(*minimap, QEvent::MouseMove, first, Qt::LeftButton);
    expect(near(visibleCenter(view).x(), wantedSecond.x(), 1e-3),
           "moves after the release do not pan");
    expect(near(view.zoomFactor(), zoom), "panning through the minimap keeps the zoom");
    expect(view.viewAdjusted(), "a minimap pan is the artist's own placement");
    expect(f.session.snapshot().revision() == revision, "the minimap never edits the document");
    expect(f.session.selectedNodes() == std::set{selected},
           "a press in the minimap does not touch the selection");
}

void testMinimapStates() {
    Fixture f;
    auto* minimap = f.editor.minimapForTest();
    expect(minimap != nullptr, "the minimap exists");
    if (minimap == nullptr)
        return;
    // A new composition already holds its output card, so the empty graph is made by removing it.
    std::set<document::NodeId> existing;
    for (const auto& node : f.session.composition()->graph().nodes())
        existing.insert(node.id);
    expect(!minimap->isHidden() && minimap->cardRects().size() == existing.size(),
           "the minimap shows the cards a new composition has");
    commands::Transaction removal("Clear", f.session.snapshot().revision());
    removal.emplace<commands::RemoveNodes>(f.session.compositionId(), existing);
    const bool cleared = f.scene()->submit(std::move(removal)).succeeded();
    QCoreApplication::processEvents();
    expect(!cleared || minimap->isHidden(), "the minimap hides for an empty graph");
    f.add(document::kSolidSourceNodeType, {0, 0});
    QCoreApplication::processEvents();
    expect(!minimap->isHidden(), "the minimap appears with a card");
    const QPointF where = minimap->pos() + QPointF(5, 5);
    f.editor.resize(1100, 700);
    QCoreApplication::processEvents();
    expect(minimap->pos() != where.toPoint() - QPoint(5, 5), "the minimap follows the corner");
    // Painted pixels use the tokens: the plate is the Surface step.
    QImage image(minimap->size(), QImage::Format_ARGB32_Premultiplied);
    minimap->render(&image);
    expect(image.pixelColor(image.width() / 2, 1) != QColor(), "the minimap paints");
}

void testPreferences() {
    Fixture f;
    f.add(document::kSolidSourceNodeType, {0, 0});
    auto* footer = f.editor.footerWidgetForTest();
    footer->resize(700, 40);
    footer->show();
    QCoreApplication::processEvents();
    const auto controls = findControls(*footer);
    auto* minimap = f.editor.minimapForTest();
    if (!controls.out || !controls.value || !controls.in || !minimap)
        return expect(false, "the controls exist");
    expect(!controls.value->isHidden() && !minimap->isHidden(), "both are on by default");
    ApplicationPreferences preferences;
    expect(preferences.nodeShowZoomControls && preferences.nodeShowMinimap,
           "both preferences default to on");
    preferences.nodeShowZoomControls = false;
    f.editor.applyApplicationPreferences(preferences);
    QCoreApplication::processEvents();
    expect(controls.out->isHidden() && controls.value->isHidden() && controls.in->isHidden(),
           "turning Show zoom controls off hides the buttons and the value");
    expect(!minimap->isHidden(), "the minimap is independent of the zoom controls");
    preferences.nodeShowZoomControls = true;
    preferences.nodeShowMinimap = false;
    f.editor.applyApplicationPreferences(preferences);
    QCoreApplication::processEvents();
    expect(!controls.out->isHidden() && !controls.value->isHidden() && !controls.in->isHidden(),
           "turning Show zoom controls on brings them back");
    expect(minimap->isHidden(), "turning Show minimap off hides the minimap");
    preferences.nodeShowMinimap = true;
    f.editor.applyApplicationPreferences(preferences);
    QCoreApplication::processEvents();
    expect(!minimap->isHidden(), "turning Show minimap on brings it back");
}
} // namespace
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testZoomControls();
        bloom::ui::test::testMinimapPan();
        bloom::ui::test::testMinimapStates();
        bloom::ui::test::testPreferences();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}

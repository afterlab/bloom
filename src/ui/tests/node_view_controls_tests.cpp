// #695, #721: the node editor's view controls. The footer's zoom preset dropdown, the minimap
// overlay in the canvas corner with the zoom row under it, the two Preferences that switch them,
// and the footer's selection count. Zooming and panning are driven through the controls themselves
// (a chosen preset, clicks, a drag on the slider, a press and drag in the minimap).

#include "node_editor_minimap.hpp"
#include "node_interaction_test_support.hpp"

#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/slider.hpp>
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

struct ZoomRow final {
    kit::KIconButton* out = nullptr;
    kit::KSlider* slider = nullptr;
    kit::KIconButton* in = nullptr;
};

ZoomRow findZoomRow(QWidget& minimap) {
    return {minimap.findChild<kit::KIconButton*>(QStringLiteral("nodeZoomOutButton")),
            minimap.findChild<kit::KSlider*>(QStringLiteral("nodeZoomSlider")),
            minimap.findChild<kit::KIconButton*>(QStringLiteral("nodeZoomInButton"))};
}

QString percentText(const NodeGraphicsView& view) {
    return QStringLiteral("%1%").arg(std::lround(view.zoomFactor() * 100.0));
}

QPointF visibleCenter(NodeGraphicsView& view) {
    return view.mapToScene(view.viewport()->rect()).boundingRect().center();
}

// #721: the footer's zoom preset dropdown is back, and always shows the current zoom.
void testZoomDropdown() {
    Fixture f;
    auto* footer = f.editor.footerWidgetForTest();
    footer->resize(700, 40);
    footer->show();
    QCoreApplication::processEvents();
    auto* dropdown = footer->findChild<kit::KDropdown*>(QStringLiteral("nodeZoomDropdown"));
    expect(dropdown != nullptr, "the footer carries the zoom dropdown");
    if (dropdown == nullptr)
        return;
    expect(footer->findChild<QWidget*>(QStringLiteral("nodeZoomValue")) == nullptr &&
               footer->findChild<QWidget*>(QStringLiteral("nodeZoomOutButton")) == nullptr &&
               footer->findChild<QWidget*>(QStringLiteral("nodeZoomInButton")) == nullptr &&
               footer->findChild<QWidget*>(QStringLiteral("nodeZoomSlider")) == nullptr,
           "the footer has no zoom buttons, value or slider");
    QStringList presets;
    for (int index = 0; index < 5 && index < dropdown->count(); ++index)
        presets << dropdown->itemText(index);
    expect(presets == QStringList{QStringLiteral("Fit"), QStringLiteral("100%"),
                                  QStringLiteral("75%"), QStringLiteral("50%"),
                                  QStringLiteral("25%")},
           "the dropdown lists Fit, 100%, 75%, 50% and 25%");
    auto& view = *f.view();

    dropdown->chooseIndex(3);
    expect(near(view.zoomFactor(), 0.5) && dropdown->currentText() == QStringLiteral("50%"),
           "choosing 50% zooms to 50% and shows it");
    dropdown->chooseIndex(2);
    expect(near(view.zoomFactor(), 0.75) && dropdown->currentText() == QStringLiteral("75%"),
           "choosing 75% zooms to 75% and shows it");
    // A zoom that is not a preset is shown too, as one trailing item.
    view.zoomStep(1);
    expect(dropdown->currentText() == percentText(view) && dropdown->count() == 6,
           "a wheel step shows the current zoom in the closed field");
    view.zoomStep(1);
    expect(dropdown->currentText() == percentText(view) && dropdown->count() == 6,
           "a later zoom renames that item instead of adding another");
    view.zoomToPercent(100);
    expect(dropdown->currentText() == QStringLiteral("100%"), "a zoom on a preset shows the preset");
    // Fit frames the graph and then shows the zoom the view ended at.
    view.zoomToPercent(25);
    dropdown->chooseIndex(0);
    expect(!view.viewAdjusted(), "Fit frames the graph");
    expect(dropdown->currentText() == percentText(view), "after Fit the field shows the zoom");
}

// #721: the zoom row under the minimap -- zoom out, a slider over the whole zoom range, zoom in.
void testMinimapZoomRow() {
    Fixture f;
    f.add(document::kSolidSourceNodeType, {0, 100});
    f.add(document::kSolidSourceNodeType, {900, 600});
    QCoreApplication::processEvents();
    auto& view = *f.view();
    auto* minimap = f.editor.minimapForTest();
    if (minimap == nullptr)
        return expect(false, "the minimap exists");
    const auto row = findZoomRow(*minimap);
    expect(row.out && row.slider && row.in, "the minimap carries the zoom row");
    if (!row.out || !row.slider || !row.in)
        return;
    expect(row.out->isVisible() && row.slider->isVisible() && row.in->isVisible(),
           "the zoom row shows with the minimap");
    const qreal mapBottom = minimap->mapRect().bottom();
    expect(row.out->geometry().top() >= mapBottom && row.slider->geometry().top() >= mapBottom &&
               row.in->geometry().top() >= mapBottom &&
               row.out->geometry().right() < row.slider->geometry().left() &&
               row.slider->geometry().right() < row.in->geometry().left() &&
               row.in->geometry().right() < minimap->width(),
           "zoom out, the slider and zoom in sit in one row under the map");
    expect(near(std::exp2(row.slider->minimum()), ViewTransform::kMinZoom) &&
               near(std::exp2(row.slider->maximum()), ViewTransform::kMaxZoom),
           "the slider covers the canvas's zoom range");

    // The slider follows every zoom change: a preset, a wheel step, Fit.
    const auto follows = [&] { return near(std::exp2(row.slider->value()), view.zoomFactor()); };
    view.zoomToPercent(100);
    expect(follows() && near(row.slider->normalizedValue(), 0.5),
           "at 100% the slider sits in the middle");
    view.zoomToPercent(25);
    expect(follows(), "the slider follows a preset");
    view.zoomStep(2);
    expect(follows(), "the slider follows a wheel step");
    view.frameGraph();
    expect(follows(), "the slider follows Fit");

    // The buttons step like the Zoom In and Zoom Out commands.
    view.zoomToPercent(100);
    row.in->click();
    expect(near(view.zoomFactor(), kZoomStepFactor) && follows(),
           "Zoom In steps by one canvas step and the slider follows");
    row.out->click();
    row.out->click();
    expect(near(view.zoomFactor(), 1.0 / kZoomStepFactor) && follows(),
           "Zoom Out steps by one canvas step and the slider follows");

    // Dragging the slider zooms about the view's center.
    view.zoomToPercent(100);
    QCoreApplication::processEvents();
    const QPointF center = visibleCenter(view);
    const double middle = row.slider->height() / 2.0;
    const QPointF grip = row.slider->handleRect().center();
    sendMouse(*row.slider, QEvent::MouseButtonPress, grip, Qt::LeftButton);
    sendMouse(*row.slider, QEvent::MouseMove, QPointF(row.slider->positionOf(1.5), middle),
              Qt::LeftButton);
    expect(view.zoomFactor() > 2.0 && near(std::exp2(row.slider->value()), view.zoomFactor()),
           "dragging the slider right zooms in");
    sendMouse(*row.slider, QEvent::MouseMove, QPointF(row.slider->positionOf(-1.5), middle),
              Qt::LeftButton);
    expect(view.zoomFactor() < 0.5 && near(std::exp2(row.slider->value()), view.zoomFactor()),
           "dragging the slider left zooms out");
    sendMouse(*row.slider, QEvent::MouseButtonRelease, QPointF(row.slider->positionOf(-1.5), middle),
              Qt::NoButton);
    expect(near(visibleCenter(view).x(), center.x(), 1e-6) &&
               near(visibleCenter(view).y(), center.y(), 1e-6),
           "zooming by the slider holds the view center");

    // The limits are the canvas's own.
    view.zoomToFactor(1000.0);
    expect(near(view.zoomFactor(), ViewTransform::kMaxZoom) && !row.in->isEnabled() &&
               near(row.slider->normalizedValue(), 1.0),
           "Zoom In stops at the canvas maximum");
    view.zoomToFactor(0.0001);
    expect(near(view.zoomFactor(), ViewTransform::kMinZoom) && !row.out->isEnabled() &&
               near(row.slider->normalizedValue(), 0.0),
           "Zoom Out stops at the canvas minimum");
}

// #721: one selected node reads "1 node".
void testSelectionCount() {
    Fixture f;
    const auto first = f.add(document::kSolidSourceNodeType, {0, 0});
    const auto second = f.add(document::kSolidSourceNodeType, {400, 0});
    auto* readout = f.editor.findChild<kit::KLabel*>(QStringLiteral("nodeSelectionReadout"));
    if (readout == nullptr)
        return expect(false, "the footer has the selection count");
    f.session.selectNodes({first}, first);
    QCoreApplication::processEvents();
    expect(readout->text() == QStringLiteral("1 node"), "one selected node reads \"1 node\"");
    f.session.selectNodes({first, second}, second);
    QCoreApplication::processEvents();
    expect(readout->text() == QStringLiteral("2 nodes"), "two selected nodes read \"2 nodes\"");
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
               minimap->mapRect().height() == kit::px(kit::Size::NodeMinimapHeight) &&
               minimap->height() == kit::px(kit::Size::NodeMinimapHeight) +
                                        kit::px(kit::Spacing::XS) + kit::px(kit::Size::Control),
           "the map and the zoom row under it take their sizes from the tokens");
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
    const QRectF map = minimap->mapRect();
    const QPointF first(map.width() * 0.25, map.height() * 0.30);
    const QPointF wanted = minimap->toScene(first);
    sendMouse(*minimap, QEvent::MouseButtonPress, first, Qt::LeftButton);
    expect(near(visibleCenter(view).x(), wanted.x(), 1e-3) &&
               near(visibleCenter(view).y(), wanted.y(), 1e-3),
           "pressing centers the view on the scene point under the pointer");
    const QPointF second(map.width() * 0.70, map.height() * 0.65);
    const QPointF wantedSecond = minimap->toScene(second);
    sendMouse(*minimap, QEvent::MouseMove, QPointF(map.width() * 0.5, map.height() * 0.5),
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
    QCoreApplication::processEvents();
    auto* minimap = f.editor.minimapForTest();
    if (minimap == nullptr)
        return expect(false, "the minimap exists");
    const auto row = findZoomRow(*minimap);
    if (!row.out || !row.slider || !row.in)
        return expect(false, "the zoom row exists");
    const auto rowShown = [&] {
        return row.out->isVisible() && row.slider->isVisible() && row.in->isVisible();
    };
    const auto rowHidden = [&] {
        return !row.out->isVisible() && !row.slider->isVisible() && !row.in->isVisible();
    };
    expect(minimap->isVisible() && rowShown(), "the minimap and its zoom row are on by default");
    ApplicationPreferences preferences;
    expect(preferences.nodeShowZoomControls && preferences.nodeShowMinimap,
           "both preferences default to on");
    // Show minimap switches the map and the zoom row together.
    preferences.nodeShowMinimap = false;
    f.editor.applyApplicationPreferences(preferences);
    QCoreApplication::processEvents();
    expect(!minimap->isVisible() && rowHidden(),
           "turning Show minimap off hides the minimap and its zoom row");
    preferences.nodeShowMinimap = true;
    f.editor.applyApplicationPreferences(preferences);
    QCoreApplication::processEvents();
    expect(minimap->isVisible() && rowShown(), "turning Show minimap on brings both back");
    // Show zoom controls switches the zoom row alone.
    preferences.nodeShowZoomControls = false;
    f.editor.applyApplicationPreferences(preferences);
    QCoreApplication::processEvents();
    expect(minimap->isVisible() && rowHidden() &&
               minimap->height() == kit::px(kit::Size::NodeMinimapHeight),
           "turning Show zoom controls off leaves the map alone, without the row");
    preferences.nodeShowZoomControls = true;
    f.editor.applyApplicationPreferences(preferences);
    QCoreApplication::processEvents();
    expect(minimap->isVisible() && rowShown(), "turning Show zoom controls on brings the row back");
}
} // namespace
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testZoomDropdown();
        bloom::ui::test::testMinimapZoomRow();
        bloom::ui::test::testSelectionCount();
        bloom::ui::test::testMinimapPan();
        bloom::ui::test::testMinimapStates();
        bloom::ui::test::testPreferences();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}

// #698: marquee selection of layers in the Viewer.
//
// With the Select tool, a drag that starts on empty space draws a rectangle and, on release,
// selects every layer the rectangle touches: plain replaces the selection, Shift adds to it and
// the platform's toggle modifier (Ctrl; Command on macOS) toggles the touched layers. A press
// released in place is a click on empty space and clears the selection, as it always did (with
// Shift or the toggle modifier held it leaves the selection alone). Escape cancels the drag.
//
// The layers are the ones the displayed frame drew, by their evaluated outlines, so the marquee
// selects what is seen; the selection itself is the session's (CompositionSession::selectLayers),
// the one the Timeline's layer rows, the node cards and agents read.
//
// The rectangle is drawn in widget pixels, so its border is one hairline at every zoom.

#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>

namespace bloom::ui {

void ViewerEditor::beginLayerMarquee(QMouseEvent* event) {
    layerMarquee_ = LayerMarquee{event->position(), event->position(), event->modifiers(), false};
    event->accept();
}

bool ViewerEditor::layerMarqueeMove(QMouseEvent* event) {
    if (!layerMarquee_)
        return false;
    layerMarquee_->pointer = event->position();
    layerMarquee_->modifiers = event->modifiers();
    if (!layerMarquee_->moved &&
        (layerMarquee_->pointer - layerMarquee_->origin).manhattanLength() >=
            QApplication::startDragDistance())
        layerMarquee_->moved = true;
    if (layerMarquee_->moved)
        update();
    event->accept();
    return true;
}

bool ViewerEditor::layerMarqueeRelease(QMouseEvent* event) {
    if (!layerMarquee_ || event->button() != Qt::LeftButton)
        return false;
    const auto marquee = *std::exchange(layerMarquee_, std::nullopt);
    const bool add = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool toggle = event->modifiers().testFlag(Qt::ControlModifier);
    event->accept();
    update();
    if (!marquee.moved) {
        // A click on empty space.
        if (!add && !toggle)
            session_.clearSelection();
        return true;
    }
    const auto touched =
        layersTouching(QRectF(marquee.origin, event->position()).normalized());
    using Mode = CompositionSession::LayerSelectionMode;
    if (touched.empty()) {
        if (!add && !toggle)
            session_.clearSelection();
        return true;
    }
    session_.selectLayers(touched, toggle ? Mode::Toggle : add ? Mode::Add : Mode::Replace);
    return true;
}

void ViewerEditor::cancelLayerMarquee() {
    if (!layerMarquee_)
        return;
    layerMarquee_.reset();
    update();
}

// The layers of the composition's own stack whose outline in the displayed frame meets the
// rectangle (widget coordinates), top of the stack first.
std::vector<document::LayerId> ViewerEditor::layersTouching(const QRectF& widgetRect) const {
    std::vector<document::LayerId> touched;
    const auto mapping = currentMapping();
    const auto actual = actualDisplayDescriptor();
    const auto frame = actual ? actual->frame : nullptr;
    const auto* stack = session_.timelineMerge();
    if (!mapping || stack == nullptr || !frameInCurrentDomain(frame) ||
        frame->desiredIdentity().time != session_.currentTime())
        return touched;
    const auto a = mapping->toComposition(widgetRect.topLeft());
    const auto b = mapping->toComposition(widgetRect.bottomRight());
    const QRectF area = QRectF(QPointF(a.x, a.y), QPointF(b.x, b.y)).normalized();
    const QPolygonF marquee(area);
    const auto bounds = previewController_->currentLayerBounds(frame);
    for (const auto& entry : stack->entries()) {
        const auto found =
            std::ranges::find(bounds, entry.layerId, &runtime::EvaluatedOperationBounds::layerId);
        if (found == bounds.end())
            continue;
        QPolygonF outline;
        for (const auto& corner : found->polygon)
            outline << QPointF(corner.x, corner.y);
        // A degenerate outline (a layer that draws nothing) is not touched by anything.
        if (outline.boundingRect().isEmpty())
            continue;
        if (outline.intersects(marquee))
            touched.push_back(entry.layerId);
    }
    return touched;
}

void ViewerEditor::paintLayerMarquee(QPainter& painter) const {
    if (!layerMarquee_ || !layerMarquee_->moved)
        return;
    painter.save();
    // Widget pixels, whatever the Viewer's zoom: the border is one hairline.
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(kit::color(kit::Color::Accent));
    pen.setWidthF(kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(kit::withOpacity(kit::color(kit::Color::Accent), kit::kMarqueeFillOpacity));
    painter.drawRect(QRectF(layerMarquee_->origin, layerMarquee_->pointer)
                         .normalized()
                         .intersected(contentRect()));
    painter.restore();
}

} // namespace bloom::ui

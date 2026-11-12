#include "node_editor_minimap.hpp"

#include "node_editor_items.hpp"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_editor.hpp>

namespace bloom::ui {

NodeMinimap::NodeMinimap(NodeGraphicsView& view) : QWidget(view.viewport()), view_(view) {
    setObjectName(QStringLiteral("nodeMinimap"));
    setAccessibleName(tr("Node graph minimap"));
    setAccessibleDescription(
        tr("Shows the whole graph and the part the canvas shows. Drag to move the view."));
    setFixedSize(kit::px(kit::Size::NodeMinimapWidth), kit::px(kit::Size::NodeMinimapHeight));
    setCursor(Qt::PointingHandCursor);
    view_.viewport()->installEventFilter(this);
    connect(&view_, &NodeGraphicsView::viewChanged, this, qOverload<>(&QWidget::update));
    if (auto* scene = view_.scene())
        connect(scene, &QGraphicsScene::changed, this, [this] { refreshCards(); });
    refreshCards();
    place();
}

QRectF NodeMinimap::visibleSceneRect() const {
    const QRectF port(view_.viewport()->rect());
    return QRectF(view_.sceneFromViewport(port.topLeft()), view_.sceneFromViewport(port.bottomRight()))
        .normalized();
}

void NodeMinimap::refreshCards() {
    std::vector<QRectF> cards;
    std::vector<QRectF> frames;
    if (auto* graph = qobject_cast<NodeGraphicsScene*>(view_.scene())) {
        for (const auto* item : graph->nodeItems())
            cards.push_back(item->sceneBoundingRect());
        for (const auto* item : graph->items())
            if (item->parentItem() == nullptr)
                if (const auto* group = dynamic_cast<const node_editor::NodeGroupItem*>(item))
                    frames.push_back(group->frameRect());
    }
    if (cards == cards_ && frames == frames_)
        return;
    cards_ = std::move(cards);
    frames_ = std::move(frames);
    updateVisibility();
    update();
}

void NodeMinimap::setEnabledByPreference(const bool enabled) {
    enabled_ = enabled;
    updateVisibility();
}

void NodeMinimap::updateVisibility() {
    const QSize port = view_.viewport()->size();
    // Leave room around it: a canvas smaller than twice the minimap in either direction would be
    // mostly covered by it.
    const bool roomy = port.width() >= 2 * width() && port.height() >= 2 * height();
    const bool shown = enabled_ && !cards_.empty() && roomy;
    if (shown) {
        place();
        raise();
    }
    setVisible(shown);
}

void NodeMinimap::place() {
    const int margin = kit::px(kit::Spacing::S);
    const QSize port = view_.viewport()->size();
    move(port.width() - width() - margin, port.height() - height() - margin);
}

bool NodeMinimap::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_.viewport() && event->type() == QEvent::Resize) {
        place();
        updateVisibility();
    }
    return QWidget::eventFilter(watched, event);
}

QRectF NodeMinimap::plate() const { return QRectF(rect()); }

QRectF NodeMinimap::inner() const {
    const qreal inset = kit::px(kit::Spacing::S);
    return plate().adjusted(inset, inset, -inset, -inset);
}

QRectF NodeMinimap::contentRect() const {
    QRectF content = dragging_ ? frozenContent_ : QRectF();
    if (!dragging_) {
        for (const auto& card : cards_)
            content = content.isNull() ? card : content.united(card);
        for (const auto& frame : frames_)
            content = content.isNull() ? frame : content.united(frame);
        content = content.isNull() ? visibleSceneRect() : content.united(visibleSceneRect());
    }
    return content;
}

QRectF NodeMinimap::toMinimap(const QRectF& scene) const {
    const QRectF content = contentRect();
    const QRectF box = inner();
    if (content.width() <= 0.0 || content.height() <= 0.0)
        return {};
    const qreal scale = std::min(box.width() / content.width(), box.height() / content.height());
    const QPointF origin(box.center().x() - content.width() * scale / 2.0,
                         box.center().y() - content.height() * scale / 2.0);
    return QRectF(origin.x() + (scene.left() - content.left()) * scale,
                  origin.y() + (scene.top() - content.top()) * scale, scene.width() * scale,
                  scene.height() * scale);
}

QPointF NodeMinimap::toScene(const QPointF& minimapPoint) const {
    const QRectF content = contentRect();
    const QRectF unit = toMinimap(content);
    if (unit.width() <= 0.0 || unit.height() <= 0.0)
        return content.center();
    const qreal scale = unit.width() / content.width();
    return QPointF(content.left() + (minimapPoint.x() - unit.left()) / scale,
                   content.top() + (minimapPoint.y() - unit.top()) / scale);
}

QRectF NodeMinimap::viewportRect() const { return toMinimap(visibleSceneRect()); }

void NodeMinimap::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    kit::fillRoundedSurface(painter, plate(), kit::color(kit::Color::Surface),
                            kit::color(kit::Color::Border), kit::Radius::Small);
    painter.setClipRect(inner().adjusted(-1.0, -1.0, 1.0, 1.0));

    const qreal least = kit::px(kit::Size::Hairline);
    const auto drawable = [least](QRectF mapped) {
        mapped.setWidth(std::max(mapped.width(), least));
        mapped.setHeight(std::max(mapped.height(), least));
        return mapped;
    };
    // Group frames first, as outlines, so the cards read on top of them.
    kit::applyHairlinePen(painter, kit::color(kit::Color::BorderHover));
    painter.setBrush(Qt::NoBrush);
    for (const auto& frame : frames_)
        painter.drawRect(drawable(toMinimap(frame)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::Field));
    for (const auto& card : cards_)
        painter.drawRect(drawable(toMinimap(card)));

    const QColor accent = kit::color(kit::Color::Accent);
    const QRectF visible = viewportRect();
    painter.setBrush(kit::withOpacity(accent, kit::kSelectionWashOpacity));
    kit::applyHairlinePen(painter, accent);
    painter.drawRect(visible);
}

void NodeMinimap::panTo(const QPointF& minimapPoint) {
    const QRectF box = inner();
    const QPointF held(std::clamp(minimapPoint.x(), box.left(), box.right()),
                       std::clamp(minimapPoint.y(), box.top(), box.bottom()));
    view_.centerOnScenePoint(toScene(held));
}

void NodeMinimap::mousePressEvent(QMouseEvent* event) {
    event->accept();
    if (event->button() != Qt::LeftButton)
        return;
    // One continuous pan: the mapping stays where it was at the press for the whole drag, so the
    // viewport rectangle can follow the pointer instead of the picture rescaling under it.
    frozenContent_ = contentRect();
    dragging_ = true;
    panTo(event->position());
}

void NodeMinimap::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
    if (dragging_)
        panTo(event->position());
}

void NodeMinimap::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        update();
    }
}

void NodeMinimap::wheelEvent(QWheelEvent* event) { event->accept(); }

} // namespace bloom::ui

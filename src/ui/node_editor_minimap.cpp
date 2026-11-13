#include "node_editor_minimap.hpp"

#include "node_editor_items.hpp"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <cmath>

namespace bloom::ui {

NodeMinimap::NodeMinimap(NodeGraphicsView& view) : QWidget(view.viewport()), view_(view) {
    setObjectName(QStringLiteral("nodeMinimap"));
    setAccessibleName(tr("Node graph minimap"));
    setAccessibleDescription(
        tr("Shows the whole graph and the part the canvas shows. Drag to move the view."));
    setCursor(Qt::PointingHandCursor);
    buildZoomRow();
    layoutZoomRow();
    view_.viewport()->installEventFilter(this);
    connect(&view_, &NodeGraphicsView::viewChanged, this, qOverload<>(&QWidget::update));
    connect(&view_, &NodeGraphicsView::viewChanged, this, &NodeMinimap::refreshZoomRow);
    refreshZoomRow();
    // #721: never QGraphicsScene::changed. Connecting anything to that signal switches the scene
    // to its compatibility update path for every view (Qt keeps it for code written before 4.5):
    // items stop sending their repaints straight to the view, and an item that is removed or
    // shrinks leaves its old pixels on the canvas until something else repaints the area. The
    // cards are re-read when the canvas repaints (a card that moves, grows or goes is drawn
    // again) and when the scene has projected a new document.
    if (auto* scene = qobject_cast<NodeGraphicsScene*>(view_.scene()))
        connect(scene, &NodeGraphicsScene::projected, this, &NodeMinimap::scheduleRefresh);
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
    const QSizeF map = plate().size();
    const bool roomy = port.width() / 2.0 >= map.width() && port.height() / 2.0 >= map.height();
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
    // The canvas drew: whatever moved on it may have moved a card.
    if (watched == view_.viewport() && event->type() == QEvent::Paint)
        scheduleRefresh();
    return QWidget::eventFilter(watched, event);
}

void NodeMinimap::scheduleRefresh() {
    // After the paint or the projection has finished, once for any number of them.
    if (refreshPending_)
        return;
    refreshPending_ = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            refreshPending_ = false;
            refreshCards();
        },
        Qt::QueuedConnection);
}

// --- The zoom row (#721) ------------------------------------------------------------------------

void NodeMinimap::buildZoomRow() {
    const auto button = [this](const kit::IconId icon, const QString& name, const QString& text) {
        auto* control = new kit::KIconButton(this);
        control->setObjectName(name);
        control->setIcon(kit::icon(icon, kit::IconRole::Chrome));
        control->setToolTip(text);
        control->setAccessibleName(text);
        control->setCursor(Qt::ArrowCursor);
        return control;
    };
    zoomOut_ = button(kit::IconId::Subtract, QStringLiteral("nodeZoomOutButton"), tr("Zoom Out"));
    connect(zoomOut_, &QAbstractButton::clicked, this, [this] { view_.zoomStep(-1); });
    zoomIn_ = button(kit::IconId::Add, QStringLiteral("nodeZoomInButton"), tr("Zoom In"));
    connect(zoomIn_, &QAbstractButton::clicked, this, [this] { view_.zoomStep(1); });

    // Logarithmic: the slider's value is log2 of the zoom, so the middle of a 1/16..16 range is
    // 100% and equal travel is an equal zoom ratio anywhere along it.
    zoomSlider_ = new kit::KSlider(this);
    zoomSlider_->setObjectName(QStringLiteral("nodeZoomSlider"));
    zoomSlider_->setAccessibleName(tr("Zoom"));
    zoomSlider_->setToolTip(tr("Zoom. Drag to zoom about the center of the view."));
    zoomSlider_->setRange(std::log2(ViewTransform::kMinZoom), std::log2(ViewTransform::kMaxZoom));
    zoomSlider_->setDefaultValue(0.0);
    zoomSlider_->setResetText(tr("Actual Size"));
    connect(zoomSlider_, &kit::KSlider::valueChanged, this, [this](const double exponent) {
        if (!zoomSyncing_)
            view_.zoomToFactor(std::exp2(exponent));
    });
}

void NodeMinimap::layoutZoomRow() {
    const int mapWidth = kit::px(kit::Size::NodeMinimapWidth);
    const int mapHeight = kit::px(kit::Size::NodeMinimapHeight);
    const int rowHeight = kit::px(kit::Size::Control);
    const int gap = kit::px(kit::Spacing::XS);
    for (QWidget* control : {static_cast<QWidget*>(zoomOut_), static_cast<QWidget*>(zoomSlider_),
                             static_cast<QWidget*>(zoomIn_)})
        control->setVisible(zoomRowEnabled_);
    if (!zoomRowEnabled_) {
        setFixedSize(mapWidth, mapHeight);
        return;
    }
    setFixedSize(mapWidth, mapHeight + gap + rowHeight);
    const int top = mapHeight + gap;
    zoomOut_->setGeometry(0, top, rowHeight, rowHeight);
    zoomIn_->setGeometry(mapWidth - rowHeight, top, rowHeight, rowHeight);
    const int inset = rowHeight + gap;
    zoomSlider_->setGeometry(inset, top, mapWidth - inset - inset, rowHeight);
}

void NodeMinimap::refreshZoomRow() {
    if (zoomSlider_ == nullptr)
        return;
    const double zoom = view_.zoomFactor();
    if (!(zoom > 0.0))
        return;
    // A projection of the view, never an edit; a slider being dragged keeps its own value.
    zoomSyncing_ = true;
    zoomSlider_->setValue(std::log2(zoom));
    zoomSyncing_ = false;
    zoomOut_->setEnabled(zoom > ViewTransform::kMinZoom * (1.0 + 1e-9));
    zoomIn_->setEnabled(zoom < ViewTransform::kMaxZoom * (1.0 - 1e-9));
}

void NodeMinimap::setZoomRowEnabledByPreference(const bool enabled) {
    if (zoomRowEnabled_ == enabled)
        return;
    zoomRowEnabled_ = enabled;
    layoutZoomRow();
    updateVisibility();
    update();
}

// The map's plate: the widget's top, above the zoom row.
QRectF NodeMinimap::plate() const {
    return QRectF(0.0, 0.0, kit::px(kit::Size::NodeMinimapWidth),
                  kit::px(kit::Size::NodeMinimapHeight));
}

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
    if (zoomRowEnabled_) {
        // The zoom row sits on a plate of its own so it reads over any graph beneath it.
        const QRectF row(0.0, plate().bottom() + kit::px(kit::Spacing::XS), width(),
                         kit::px(kit::Size::Control));
        kit::fillRoundedSurface(painter, row, kit::color(kit::Color::Surface),
                                kit::color(kit::Color::Border), kit::Radius::Small);
    }
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
    // Only the map pans; the strip around the zoom row does nothing.
    if (event->button() != Qt::LeftButton || !plate().contains(event->position()))
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

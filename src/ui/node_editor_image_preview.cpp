// #490: the Image Preview node's pixels in the node editor -- the card thumbnail and the
// background behind the graph. Both only paint what ImagePreviewController has already prepared on
// workers; a busy or failed preview draws a placeholder and never stalls the canvas.

#include "node_editor_items.hpp"


#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_editor.hpp>

#include <QPaintDevice>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

// Device pixels per item unit at `painter`'s current transform.
[[nodiscard]] qreal deviceScale(const QPainter& painter) {
    const auto transform = painter.worldTransform();
    const qreal scale = std::hypot(transform.m11(), transform.m12());
    const qreal ratio = painter.device() != nullptr ? painter.device()->devicePixelRatioF() : 1.0;
    return (scale > 0.0 ? scale : 1.0) * ratio;
}

// The image fitted into `bounds` at its own aspect, centered.
[[nodiscard]] QRectF fitted(const QRectF& bounds, const QSize image) {
    if (image.isEmpty() || bounds.isEmpty())
        return bounds;
    const qreal scale = std::min(bounds.width() / image.width(), bounds.height() / image.height());
    const QSizeF size(image.width() * scale, image.height() * scale);
    return {bounds.center() - QPointF(size.width() / 2.0, size.height() / 2.0), size};
}

// The image, its alpha shown over the Viewer's checkerboard in the RGBA view, and a "CPU" label
// when the pixels did not come from the GPU route.
void paintPreviewImage(QPainter& painter, const QRectF& target,
                       const ImagePreviewController::Preview& preview,
                       const document::ImagePreviewDisplay display) {
    painter.save();
    if (display == document::ImagePreviewDisplay::Rgba)
        kit::drawAlphaCheckerboard(painter, target, kit::px(kit::Spacing::S),
                                   kit::Color::ViewerCheckerLight, kit::Color::ViewerCheckerDark);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setOpacity(painter.opacity() *
                       (preview.state == ImagePreviewController::State::Ready ? 1.0 : 0.6));
    painter.drawImage(target, preview.image);
    painter.restore();
}

void paintBadge(QPainter& painter, const QRectF& target, const QString& text) {
    painter.save();
    painter.setFont(kit::font(kit::TypeRole::UiSmall));
    const QFontMetricsF metrics(painter.font());
    const qreal pad = kit::px(kit::Spacing::XS);
    const QRectF badge(target.right() - metrics.horizontalAdvance(text) - 3.0 * pad,
                       target.top() + pad, metrics.horizontalAdvance(text) + 2.0 * pad,
                       metrics.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::Surface));
    painter.drawRoundedRect(badge, pad, pad);
    painter.setPen(kit::color(kit::Color::Muted));
    painter.drawText(badge, Qt::AlignCenter, text);
    painter.restore();
}

} // namespace

namespace node_editor {

void paintImagePreview(QPainter& painter, const QRectF& rect, CompositionSession* session,
                       const document::NodeId node) {
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::Background));
    painter.drawRect(rect);
    auto* previews = session != nullptr ? session->imagePreviews() : nullptr;
    if (previews == nullptr) {
        painter.restore();
        return;
    }
    const qreal scale = deviceScale(painter);
    const QSize device(static_cast<int>(std::ceil(rect.width() * scale)),
                       static_cast<int>(std::ceil(rect.height() * scale)));
    const auto preview = previews->preview(node, device);
    const auto display = previews->display(node);
    if (!preview.image.isNull()) {
        const auto target = fitted(rect, preview.image.size());
        paintPreviewImage(painter, target, preview, display);
        if (preview.cpu)
            paintBadge(painter, target, QObject::tr("CPU"));
    }
    if (preview.image.isNull() || preview.state == ImagePreviewController::State::Failed) {
        painter.setFont(kit::font(kit::TypeRole::UiSmall));
        painter.setPen(kit::color(preview.state == ImagePreviewController::State::Failed
                                      ? kit::Color::Warn
                                      : kit::Color::Muted));
        const qreal inset = kit::px(kit::Spacing::XS);
        painter.drawText(rect.adjusted(inset, inset, -inset, -inset),
                         Qt::AlignCenter | Qt::TextWordWrap, preview.message);
    }
    painter.restore();
}

} // namespace node_editor

// --- NodeGraphicsView: the background ----------------------------------------------------------

void NodeGraphicsView::setImagePreviews(ImagePreviewController* previews) {
    previews_ = previews;
    viewport()->update();
}

QRectF NodeGraphicsView::backgroundRect() const {
    if (previews_ == nullptr || !previews_->backgroundVisible() || !previews_->backgroundNode())
        return {};
    const auto* graph = qobject_cast<const NodeGraphicsScene*>(scene());
    const auto* session = graph != nullptr ? graph->session() : nullptr;
    const auto* composition = session != nullptr ? session->composition() : nullptr;
    if (composition == nullptr)
        return {};
    const auto& format = composition->format();
    const auto extent = render::ImageExtent::create(format.width(), format.height());
    if (!extent)
        return {};
    return viewTransformedDisplayRect(QRectF(viewport()->rect()), *extent.value(),
                                      format.pixelAspect(), backgroundTransform_);
}

void NodeGraphicsView::fitBackground() {
    backgroundTransform_ = ViewTransform{};
    viewport()->update();
}

void NodeGraphicsView::backgroundActualSize() {
    backgroundTransform_ = ViewTransform{.fitToWindow = false, .zoom = 1.0, .pan = {0.0, 0.0}};
    viewport()->update();
}

void NodeGraphicsView::materializeBackgroundZoom() {
    if (!backgroundTransform_.fitToWindow)
        return;
    const auto shown = backgroundRect();
    auto actual = backgroundTransform_;
    actual.fitToWindow = false;
    actual.zoom = 1.0;
    backgroundTransform_ = actual;
    const auto unscaled = backgroundRect();
    backgroundTransform_.zoom = unscaled.width() > 0.0 && shown.width() > 0.0
                                    ? std::clamp(shown.width() / unscaled.width(),
                                                 ViewTransform::kMinZoom, ViewTransform::kMaxZoom)
                                    : 1.0;
}

void NodeGraphicsView::zoomBackgroundAbout(const QPointF viewportPoint, const double factor) {
    if (!(factor > 0.0) || backgroundRect().isEmpty())
        return;
    materializeBackgroundZoom();
    const auto before = backgroundRect();
    const double zoom = std::clamp(backgroundTransform_.zoom * factor, ViewTransform::kMinZoom,
                                   ViewTransform::kMaxZoom);
    if (before.width() <= 0.0)
        return;
    // Hold the image point under `viewportPoint` fixed: the rectangle scales about the pointer.
    const double applied = zoom / backgroundTransform_.zoom;
    const QPointF center = before.center();
    const QPointF moved = viewportPoint + (center - viewportPoint) * applied;
    backgroundTransform_.zoom = zoom;
    backgroundTransform_.pan += moved - center;
    viewport()->update();
}

void NodeGraphicsView::zoomBackgroundStep(const int notches) {
    zoomBackgroundAbout(QRectF(viewport()->rect()).center(), std::pow(kZoomStepFactor, notches));
}

void NodeGraphicsView::drawBackground(QPainter* painter, const QRectF& rect) {
    // The scene's surface and dot grid first; the background then covers the grid it lies on.
    QGraphicsView::drawBackground(painter, rect);
    const auto target = backgroundRect();
    if (target.isEmpty())
        return;
    const auto node = *previews_->backgroundNode();
    painter->save();
    painter->resetTransform();
    const qreal ratio = devicePixelRatioF();
    // Requested at the size it is drawn, never more than the composition (previewExtent caps it).
    const QSize device(static_cast<int>(std::ceil(target.width() * ratio)),
                       static_cast<int>(std::ceil(target.height() * ratio)));
    const auto preview = previews_->preview(node, device);
    if (!preview.image.isNull()) {
        paintPreviewImage(*painter, target, preview, previews_->display(node));
        if (preview.cpu)
            paintBadge(*painter, QRectF(viewport()->rect()), tr("Background: CPU"));
    } else {
        painter->setFont(kit::font(kit::TypeRole::UiSmall));
        painter->setPen(kit::color(kit::Color::Muted));
        const qreal inset = kit::px(kit::Spacing::S);
        painter->drawText(QRectF(viewport()->rect()).adjusted(inset, inset, -inset, -inset),
                          Qt::AlignLeft | Qt::AlignBottom,
                          tr("Background: %1").arg(preview.message));
    }
    painter->restore();
}

} // namespace bloom::ui

// #724 Scopes: the scope region in the Viewer.
//
// While a Scopes panel is visible, every Viewer of a composition draws that composition's scope
// region (the ScopesRegionStore's) as an overlay labelled "Scopes", in a style of its own (a
// dashed hairline in Foreground over a Canvas underlay, square handles), distinct from the region
// of interest's Accent outline and dimming. It takes input only while the Region toggle arms it
// (a drag draws a rectangle; one drag per arming) or when the press lands on the region's handles
// or body (move and resize); anywhere else the press reaches the Viewer's other gestures. A drag
// only updates the store (on release); the panel's controller does the measuring. Escape cancels a
// drag and leaves the previous region, and disarms the toggle when no drag runs.
//
// The rectangle is whole composition pixels: x and y floored, the far edge raised, clipped to the
// frame, so it is the same at every zoom, pan and preview resolution. The overlay is drawn in
// widget pixels: its hairline is one device pixel at every scale.

#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace bloom::ui {

namespace {

// The whole-pixel rectangle that holds `area` (composition pixels), clipped to the frame; nothing
// when nothing is left.
std::optional<runtime::ScopeRegion> wholePixelRegion(const QRectF& area, const double width,
                                                     const double height) {
    const auto clamp = [](const double value, const double high) {
        return std::clamp(value, 0.0, high);
    };
    const double x0 = std::floor(clamp(area.left(), width));
    const double y0 = std::floor(clamp(area.top(), height));
    const double x1 = std::ceil(clamp(area.right(), width));
    const double y1 = std::ceil(clamp(area.bottom(), height));
    if (!(x1 > x0) || !(y1 > y0))
        return std::nullopt;
    return runtime::ScopeRegion{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
                                static_cast<std::uint32_t>(x1 - x0),
                                static_cast<std::uint32_t>(y1 - y0)};
}

QRectF screenRect(const ViewerMapping& mapping, const runtime::ScopeRegion& region) {
    return QRectF(mapping.toScreen({static_cast<double>(region.x), static_cast<double>(region.y)}),
                  mapping.toScreen({static_cast<double>(region.x) + region.width,
                                    static_cast<double>(region.y) + region.height}))
        .normalized();
}

} // namespace

std::optional<ScopesRegionStore::Key> ViewerEditor::scopesRegionKey() const {
    if (shownComposition() == nullptr)
        return std::nullopt;
    return ScopesRegionStore::Key{session_.snapshot().project().id(),
                                  previewController_->targetCompositionId()};
}

void ViewerEditor::setScopesRegionStore(ScopesRegionStore* const store) {
    if (scopesRegions_ == store)
        return;
    if (scopesRegions_ != nullptr)
        disconnect(scopesRegions_, nullptr, this, nullptr);
    scopeGesture_.reset();
    scopesRegions_ = store;
    if (store != nullptr) {
        const auto refresh = [this] {
            if (!scopesRegionLive())
                scopeGesture_.reset();
            update();
        };
        connect(store, &ScopesRegionStore::regionChanged, this, refresh);
        connect(store, &ScopesRegionStore::activeChanged, this, refresh);
        connect(store, &ScopesRegionStore::armedChanged, this, refresh);
    }
    update();
}

// The overlay and its input exist only while a Scopes panel is visible.
bool ViewerEditor::scopesRegionLive() const {
    return scopesRegions_ != nullptr && scopesRegions_->active() && shownComposition() != nullptr;
}

std::optional<runtime::ScopeRegion> ViewerEditor::scopesRegionDrawnForTest() const {
    if (!scopesRegionLive())
        return std::nullopt;
    if (scopeGesture_)
        return scopesRegionCandidate(*scopeGesture_);
    const auto key = scopesRegionKey();
    return key ? scopesRegions_->region(*key) : std::nullopt;
}

// The rectangle the gesture would commit now.
std::optional<runtime::ScopeRegion>
ViewerEditor::scopesRegionCandidate(const ScopeRegionGesture& gesture) const {
    const double width = gesture.mapping.compositionFormat.width();
    const double height = gesture.mapping.compositionFormat.height();
    const auto from = gesture.mapping.toComposition(gesture.origin);
    const auto to = gesture.mapping.toComposition(gesture.pointer);
    using Kind = ScopeRegionGesture::Kind;
    if (gesture.kind == Kind::Draw)
        return wholePixelRegion(QRectF(QPointF(from.x, from.y), QPointF(to.x, to.y)).normalized(),
                                width, height);
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const auto& start = gesture.start;
    if (gesture.kind == Kind::Move) {
        // Whole pixels, and the rectangle stays whole inside the frame.
        const double maxX = std::max(0.0, width - start.width);
        const double maxY = std::max(0.0, height - start.height);
        const double x = std::clamp(start.x + std::round(dx), 0.0, maxX);
        const double y = std::clamp(start.y + std::round(dy), 0.0, maxY);
        return runtime::ScopeRegion{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                    start.width, start.height};
    }
    double left = start.x;
    double top = start.y;
    double right = static_cast<double>(start.x) + start.width;
    double bottom = static_cast<double>(start.y) + start.height;
    if ((gesture.edges & ScopeRegionGesture::kLeft) != 0)
        left += dx;
    if ((gesture.edges & ScopeRegionGesture::kRight) != 0)
        right += dx;
    if ((gesture.edges & ScopeRegionGesture::kTop) != 0)
        top += dy;
    if ((gesture.edges & ScopeRegionGesture::kBottom) != 0)
        bottom += dy;
    return wholePixelRegion(QRectF(QPointF(left, top), QPointF(right, bottom)).normalized(), width,
                            height);
}

bool ViewerEditor::scopesRegionPress(QMouseEvent* event) {
    if (!scopesRegionLive() || event->button() != Qt::LeftButton || scopeGesture_ || textEdit_ ||
        dragActive_ || panActive_ || roiGesture_ || layerMarquee_ || creation_ || motionDrag_ ||
        lineDrag_ || warpDrag_ || anchorGesture_)
        return false;
    const bool armed = scopesRegions_->armed();
    // Unarmed, the region acts only under the Select tool and never takes the region of interest
    // tool's Ctrl+Alt press.
    if (!armed &&
        (tool() != ViewerTool::Select || (event->modifiers().testFlag(Qt::ControlModifier) &&
                                          event->modifiers().testFlag(Qt::AltModifier))))
        return false;
    const auto mapping = currentMapping();
    const auto key = scopesRegionKey();
    if (!mapping || !key || !contentRect().contains(event->position()))
        return false;
    ScopeRegionGesture gesture(*mapping, *key, event->position());
    bool hit = false;
    if (const auto region = scopesRegions_->region(*key)) {
        const QRectF area = screenRect(*mapping, *region);
        const double reach = kit::px(kit::Size::GizmoHandle);
        const QPointF at = event->position();
        const bool withinX = at.x() >= area.left() - reach && at.x() <= area.right() + reach;
        const bool withinY = at.y() >= area.top() - reach && at.y() <= area.bottom() + reach;
        const double toLeft = std::abs(at.x() - area.left());
        const double toRight = std::abs(at.x() - area.right());
        const double toTop = std::abs(at.y() - area.top());
        const double toBottom = std::abs(at.y() - area.bottom());
        int edges = 0;
        if (withinY && std::min(toLeft, toRight) <= reach)
            edges |= toLeft <= toRight ? ScopeRegionGesture::kLeft : ScopeRegionGesture::kRight;
        if (withinX && std::min(toTop, toBottom) <= reach)
            edges |= toTop <= toBottom ? ScopeRegionGesture::kTop : ScopeRegionGesture::kBottom;
        if (edges != 0) {
            gesture.kind = ScopeRegionGesture::Kind::Resize;
            gesture.edges = edges;
            gesture.start = *region;
            hit = true;
        } else if (area.contains(at)) {
            gesture.kind = ScopeRegionGesture::Kind::Move;
            gesture.start = *region;
            hit = true;
        }
    }
    if (!hit && !armed)
        return false;
    scopeGesture_ = gesture;
    setFocus(Qt::MouseFocusReason);
    event->accept();
    update();
    return true;
}

bool ViewerEditor::scopesRegionMove(QMouseEvent* event) {
    if (!scopeGesture_)
        return false;
    const auto mapping = currentMapping();
    if (!scopesRegionLive() || !mapping || *mapping != scopeGesture_->mapping)
        scopeGesture_.reset();
    else
        scopeGesture_->pointer = event->position();
    update();
    event->accept();
    return true;
}

bool ViewerEditor::scopesRegionRelease(QMouseEvent* event) {
    if (!scopeGesture_ || event->button() != Qt::LeftButton)
        return false;
    const auto mapping = currentMapping();
    if (scopesRegionLive() && mapping && *mapping == scopeGesture_->mapping) {
        scopeGesture_->pointer = event->position();
        // An empty rectangle (a click, a drag outside the frame) is ignored and changes nothing.
        if (const auto region = scopesRegionCandidate(*scopeGesture_)) {
            scopesRegions_->setRegion(scopeGesture_->key, *region);
            if (scopeGesture_->kind == ScopeRegionGesture::Kind::Draw)
                scopesRegions_->setArmed(false);
        }
    }
    scopeGesture_.reset();
    update();
    event->accept();
    return true;
}

bool ViewerEditor::scopesRegionEscape(QKeyEvent* event) {
    if (event->key() != Qt::Key_Escape || scopesRegions_ == nullptr)
        return false;
    if (scopeGesture_) {
        scopeGesture_.reset();
        update();
        event->accept();
        return true;
    }
    if (scopesRegionLive() && scopesRegions_->armed()) {
        scopesRegions_->setArmed(false);
        event->accept();
        return true;
    }
    return false;
}

void ViewerEditor::paintScopesRegion(QPainter& painter) const {
    if (!scopesRegionLive())
        return;
    const auto mapping = currentMapping();
    if (!mapping)
        return;
    std::optional<runtime::ScopeRegion> region;
    if (scopeGesture_)
        region = scopesRegionCandidate(*scopeGesture_);
    else if (const auto key = scopesRegionKey())
        region = scopesRegions_->region(*key);
    if (!region)
        return;
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing, false);
    // Edges on device-pixel centers: one device pixel wide at every scale.
    const double scale = painter.device()->devicePixelRatioF();
    const auto snap = [scale](const double value) {
        return (std::floor(value * scale) + 0.5) / scale;
    };
    const QRectF raw = screenRect(*mapping, *region);
    const QRectF area(QPointF(snap(raw.left()), snap(raw.top())),
                      QPointF(snap(raw.right()), snap(raw.bottom())));
    // A Canvas underlay under a dashed Foreground line reads on any picture.
    QPen under(kit::color(kit::Color::Canvas), kit::kHairlineWidth);
    under.setCosmetic(true);
    painter.setPen(under);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area);
    QPen line(kit::color(kit::Color::Foreground), kit::kHairlineWidth, Qt::DashLine);
    line.setCosmetic(true);
    painter.setPen(line);
    painter.drawRect(area);
    // Eight square handles: Surface fill, Foreground outline (the selection's are Accent).
    const double side = kit::px(kit::Size::GizmoHandle);
    QPen outline(kit::color(kit::Color::Foreground), kit::kHairlineWidth);
    outline.setCosmetic(true);
    painter.setPen(outline);
    painter.setBrush(kit::color(kit::Color::Surface));
    const QPointF centers[] = {
        area.topLeft(),     {area.center().x(), area.top()},
        area.topRight(),    {area.right(), area.center().y()},
        area.bottomRight(), {area.center().x(), area.bottom()},
        area.bottomLeft(),  {area.left(), area.center().y()},
    };
    for (const auto& center : centers)
        painter.drawRect(
            QRectF(snap(center.x() - side / 2.0), snap(center.y() - side / 2.0), side, side));
    // The label sits above the top-left corner, or inside it when there is no room above.
    const QFont font = kit::font(kit::TypeRole::Value);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const QString label = tr("Scopes");
    const double pad = kit::px(kit::Spacing::XS);
    const QSizeF plate(metrics.horizontalAdvance(label) + 2.0 * pad, metrics.height() + pad);
    const bool above = area.top() - plate.height() >= contentRect().top();
    const QRectF plateRect(QPointF(area.left(), above ? area.top() - plate.height() : area.top()),
                           plate);
    painter.fillRect(plateRect, kit::color(kit::Color::Surface));
    painter.setPen(kit::color(kit::Color::Foreground));
    painter.drawText(plateRect.adjusted(pad, 0.0, -pad, 0.0),
                     Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, label);
    painter.restore();
}

} // namespace bloom::ui

#include <bloom/ui/kit/point_pad.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

// The share of the pad each side of the frame keeps for points just outside it.
constexpr double kOutsideShare = 0.15;
constexpr qreal kPointRadius = px(Spacing::XS);
constexpr qreal kPointReach = px(Spacing::S);
constexpr double kCoarseUnits = 10.0;

} // namespace

KPointPad::KPointPad(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kPointPad"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setAccessibleName(tr("Position"));
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void KPointPad::setFrame(const QRectF& frame) {
    if (!frame.isValid() || frame == frame_)
        return;
    frame_ = frame;
    updateGeometry();
    update();
}

void KPointPad::setPoints(std::vector<Point> points) {
    if (interactionActive_)
        return;
    points_ = std::move(points);
    if (active_ >= points_.size())
        active_ = 0;
    update();
}

QPointF KPointPad::value(const std::size_t index) const {
    return index < points_.size() ? points_[index].value : QPointF();
}

void KPointPad::setConnected(const bool connected) {
    connected_ = connected;
    update();
}

void KPointPad::setActivePoint(const std::size_t index) {
    if (index >= points_.size() || index == active_)
        return;
    active_ = index;
    update();
    Q_EMIT activePointChanged(active_);
}

QRectF KPointPad::padRect() const {
    const qreal margin = kFocusRingWidth + kHairlineWidth;
    return QRectF(rect()).adjusted(margin, margin, -margin, -margin);
}

QRectF KPointPad::frameRect() const {
    // The frame at its own aspect ratio, centred, with kOutsideShare of room on every side.
    const auto pad = padRect();
    const double share = 1.0 + 2.0 * kOutsideShare;
    const double scale = std::min(pad.width() / (frame_.width() * share),
                                  pad.height() / (frame_.height() * share));
    const double w = frame_.width() * scale;
    const double h = frame_.height() * scale;
    return {pad.center().x() - w / 2.0, pad.center().y() - h / 2.0, w, h};
}

double KPointPad::unitsPerPixel() const {
    const auto drawn = frameRect();
    return drawn.width() > 0.0 ? frame_.width() / drawn.width() : 1.0;
}

QPointF KPointPad::screenPoint(const QPointF value) const {
    const auto drawn = frameRect();
    const double scale = 1.0 / unitsPerPixel();
    return {drawn.left() + (value.x() - frame_.left()) * scale,
            drawn.top() + (value.y() - frame_.top()) * scale};
}

QPointF KPointPad::valueAt(const QPointF screen) const {
    const auto drawn = frameRect();
    const double units = unitsPerPixel();
    return {frame_.left() + (screen.x() - drawn.left()) * units,
            frame_.top() + (screen.y() - drawn.top()) * units};
}

std::optional<std::size_t> KPointPad::pointAt(const QPointF screen) const {
    // The active point wins a tie, so stacked points can still be told apart.
    const auto pad = padRect();
    std::optional<std::size_t> best;
    qreal nearest = kPointReach;
    for (std::size_t index = 0; index < points_.size(); ++index) {
        const auto at = screenPoint(points_[index].value);
        const QPointF drawn(std::clamp(at.x(), pad.left(), pad.right()),
                            std::clamp(at.y(), pad.top(), pad.bottom()));
        const qreal distance = std::hypot(screen.x() - drawn.x(), screen.y() - drawn.y());
        if (distance < nearest || (distance == nearest && index == active_)) {
            nearest = distance;
            best = index;
        }
    }
    return best;
}

State KPointPad::visualState() const {
    if (!isEnabled())
        return State::Disabled;
    if (grabbed_)
        return State::Pressed;
    if (hovered_)
        return State::Hover;
    if (hasKeyboardFocus(*this))
        return State::Focused;
    return State::Normal;
}

int KPointPad::heightForWidth(const int width) const {
    const double aspect = frame_.height() / std::max(1.0, frame_.width());
    const int height = static_cast<int>(std::lround(width * aspect));
    return std::clamp(height, px(Size::ControlRoomy) + px(Size::Control),
                      px(Size::DropdownWidthExpanded) - px(Size::ControlRoomy));
}

QSize KPointPad::sizeHint() const {
    const int width = px(Size::DropdownWidthExpanded) - px(Size::Control);
    return {width, heightForWidth(width)};
}

QSize KPointPad::minimumSizeHint() const {
    return {px(Size::ValueCellMin), px(Size::ControlRoomy) + px(Size::Control)};
}

void KPointPad::beginInteraction() {
    if (interactionActive_)
        return;
    interactionActive_ = true;
    base_.clear();
    for (const auto& point : points_)
        base_.push_back(point.value);
    Q_EMIT editStarted();
}

void KPointPad::finishInteraction(const bool keep) {
    if (!interactionActive_)
        return;
    interactionActive_ = false;
    grabbed_.reset();
    keyStepping_ = false;
    update();
    if (keep) {
        Q_EMIT editFinished();
        return;
    }
    for (std::size_t index = 0; index < points_.size() && index < base_.size(); ++index)
        points_[index].value = base_[index];
    Q_EMIT pointsChanged();
    Q_EMIT editCancelled();
}

void KPointPad::cancelEdit() { finishInteraction(false); }

void KPointPad::movePoint(const std::size_t index, const QPointF value) {
    if (index >= points_.size() || !std::isfinite(value.x()) || !std::isfinite(value.y()) ||
        points_[index].value == value)
        return;
    points_[index].value = value;
    update();
    Q_EMIT pointsChanged();
}

void KPointPad::resetToDefaults() {
    if (!isEnabled())
        return;
    beginInteraction();
    for (std::size_t index = 0; index < points_.size(); ++index)
        if (points_[index].defaultValue)
            movePoint(index, *points_[index].defaultValue);
    finishInteraction(true);
}

void KPointPad::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && grabbed_) {
        cancelEdit();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !isEnabled() || points_.empty()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    event->accept();
    auto index = pointAt(event->position());
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    if (!index && !shift)
        return;
    beginInteraction();
    if (!index) {
        // A Shift-press on bare pad jumps the active point to the pointer.
        index = active_;
        movePoint(*index, valueAt(event->position()));
    }
    if (*index != active_) {
        active_ = *index;
        Q_EMIT activePointChanged(active_);
    }
    grabbed_ = index;
    segmentPointer_ = event->position();
    segmentBase_ = points_[*index].value;
    segmentModifiers_ = event->modifiers() & Qt::ControlModifier;
    update();
}

void KPointPad::mouseMoveEvent(QMouseEvent* event) {
    if (!grabbed_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    event->accept();
    const auto modifiers = event->modifiers() & Qt::ControlModifier;
    if (modifiers != segmentModifiers_) {
        segmentPointer_ = event->position();
        segmentBase_ = points_[*grabbed_].value;
        segmentModifiers_ = modifiers;
    }
    const double scale = unitsPerPixel() * (modifiers ? KSlider::dragScale(modifiers) : 1.0);
    const auto delta = event->position() - segmentPointer_;
    movePoint(*grabbed_, segmentBase_ + delta * scale);
}

void KPointPad::mouseReleaseEvent(QMouseEvent* event) {
    if (grabbed_ && event->button() == Qt::LeftButton) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KPointPad::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled() && pointAt(event->position())) {
        if (grabbed_)
            finishInteraction(true);
        resetToDefaults();
        event->accept();
        return;
    }
    mousePressEvent(event);
}

void KPointPad::keyPressEvent(QKeyEvent* event) {
    if (points_.empty() || !isEnabled()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? kCoarseUnits : 1.0;
    QPointF delta;
    switch (event->key()) {
    case Qt::Key_Left:
        delta = {-step, 0.0};
        break;
    case Qt::Key_Right:
        delta = {step, 0.0};
        break;
    case Qt::Key_Up:
        delta = {0.0, -step};
        break;
    case Qt::Key_Down:
        delta = {0.0, step};
        break;
    case Qt::Key_Escape:
        if (grabbed_) {
            cancelEdit();
            event->accept();
            return;
        }
        if (keyStepping_) {
            finishInteraction(true);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    if (!interactionActive_) {
        beginInteraction();
        keyStepping_ = true;
    }
    movePoint(active_, points_[active_].value + delta);
    event->accept();
}

void KPointPad::keyReleaseEvent(QKeyEvent* event) {
    if (keyStepping_ && !event->isAutoRepeat() && event->key() != Qt::Key_Tab &&
        event->key() != Qt::Key_Backtab && event->key() != Qt::Key_Shift) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void KPointPad::focusInEvent(QFocusEvent* event) {
    if (!points_.empty()) {
        if (event->reason() == Qt::TabFocusReason)
            active_ = 0;
        else if (event->reason() == Qt::BacktabFocusReason)
            active_ = points_.size() - 1;
    }
    update();
    QWidget::focusInEvent(event);
}

void KPointPad::focusOutEvent(QFocusEvent* event) {
    if (keyStepping_)
        finishInteraction(true);
    QWidget::focusOutEvent(event);
}

bool KPointPad::focusNextPrevChild(const bool next) {
    if (next && active_ + 1 < points_.size()) {
        setActivePoint(active_ + 1);
        return true;
    }
    if (!next && active_ > 0) {
        setActivePoint(active_ - 1);
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

bool KPointPad::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (hasFocus() && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
                           key == Qt::Key_Down ||
                           (key == Qt::Key_Escape && (keyStepping_ || grabbed_)))) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::KeyPress: {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
            !key->modifiers().testAnyFlags(Qt::ControlModifier | Qt::AltModifier)) {
            const bool next =
                key->key() == Qt::Key_Tab && !key->modifiers().testFlag(Qt::ShiftModifier);
            if ((next && active_ + 1 < points_.size()) || (!next && active_ > 0)) {
                focusNextPrevChild(next);
                return true;
            }
        }
        break;
    }
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        if (grabbed_ || (event->type() == QEvent::WindowDeactivate && keyStepping_))
            finishInteraction(true);
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void KPointPad::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KPointPad::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KPointPad::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
        finishInteraction(true);
    }
    QWidget::changeEvent(event);
}

void KPointPad::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const State state = visualState();
    const auto pad = padRect();
    fillRoundedSurface(painter, pad, color(surfaceForState(Color::Field, state)),
                       color(borderForState(state)), Radius::Small);
    if (state == State::Disabled)
        painter.setOpacity(kDisabledOpacity);
    // The frame the values live in, and its centre lines.
    const auto drawn = frameRect();
    applyHairlinePen(painter, color(Color::Muted));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(drawn);
    applyHairlinePen(painter, color(Color::Border));
    painter.drawLine(QPointF(drawn.center().x(), drawn.top()),
                     QPointF(drawn.center().x(), drawn.bottom()));
    painter.drawLine(QPointF(drawn.left(), drawn.center().y()),
                     QPointF(drawn.right(), drawn.center().y()));
    const auto placed = [&](const std::size_t index) {
        const auto at = screenPoint(points_[index].value);
        return QPointF(std::clamp(at.x(), pad.left() + kPointRadius, pad.right() - kPointRadius),
                       std::clamp(at.y(), pad.top() + kPointRadius, pad.bottom() - kPointRadius));
    };
    if (connected_ && points_.size() >= 2) {
        QPen line(color(Color::Accent));
        line.setWidthF(kHairlineWidth);
        line.setStyle(Qt::DashLine);
        painter.setPen(line);
        painter.drawLine(placed(0), placed(1));
    }
    painter.setFont(kit::font(TypeRole::UiSmall));
    for (std::size_t index = 0; index < points_.size(); ++index) {
        const auto at = placed(index);
        const auto exact = screenPoint(points_[index].value);
        const bool pinned = std::abs(at.x() - exact.x()) > 0.5 || std::abs(at.y() - exact.y()) > 0.5;
        const bool active = index == active_;
        QPen ring(color(active ? Color::Accent : Color::Foreground));
        ring.setWidthF(active ? kEmphasisStroke : kHairlineWidth);
        painter.setPen(ring);
        // A point past the pad's edge is drawn hollow where it is pinned.
        painter.setBrush(pinned ? QBrush(Qt::NoBrush)
                                : QBrush(color(active ? Color::Accent : Color::Foreground)));
        painter.drawEllipse(at, kPointRadius, kPointRadius);
        if (active && hasKeyboardFocus(*this) && state != State::Disabled)
            drawFocusRing(painter,
                          QRectF(at.x() - kPointRadius, at.y() - kPointRadius, 2.0 * kPointRadius,
                                 2.0 * kPointRadius),
                          Radius::Full);
        if (!points_[index].label.isEmpty()) {
            painter.setPen(color(Color::Foreground));
            const QRectF label(at.x() + kPointRadius + px(Spacing::XXS),
                               at.y() - px(Size::IconMedium) / 2.0, px(Size::DropdownWidth),
                               px(Size::IconMedium));
            // Labels near the right edge sit on the point's left instead.
            const bool flip = label.right() > pad.right();
            painter.drawText(flip ? label.translated(-label.width() - 2.0 * kPointRadius -
                                                         2.0 * px(Spacing::XXS),
                                                     0.0)
                                  : label,
                             static_cast<int>(Qt::AlignVCenter |
                                              (flip ? Qt::AlignRight : Qt::AlignLeft)),
                             points_[index].label);
        }
    }
    painter.setOpacity(1.0);
}

} // namespace bloom::ui::kit

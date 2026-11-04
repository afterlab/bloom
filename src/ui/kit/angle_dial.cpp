#include <bloom/ui/kit/angle_dial.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QAccessible>
#include <QAction>
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace bloom::ui::kit {
namespace {

constexpr double kFineScale = 0.1;
// Closer to the centre than this, the pointer's angle is too unstable to turn the dial by.
constexpr qreal kDeadZone = px(Spacing::XXS);
constexpr qreal kTickLength = px(Spacing::XS);
constexpr qreal kHubRadius = px(Spacing::XXS);

[[nodiscard]] double wrapped(const double degrees) {
    double result = std::fmod(degrees, 360.0);
    if (result < 0.0)
        result += 360.0;
    return result;
}

// `delta` folded into (-180, 180].
[[nodiscard]] double shortest(const double delta) {
    double result = std::fmod(delta + 180.0, 360.0);
    if (result <= 0.0)
        result += 360.0;
    return result - 180.0;
}

} // namespace

KAngleDial::KAngleDial(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kAngleDial"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(tr("Angle"));
    resetText_ = tr("Reset");
    QSizePolicy policy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void KAngleDial::setValue(const double degrees) {
    if (dragging_)
        return;
    commitValue(degrees);
}

void KAngleDial::commitValue(const double degrees) {
    if (!std::isfinite(degrees) || degrees == value_)
        return;
    value_ = degrees;
    update();
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent change(this, value_);
        QAccessible::updateAccessibility(&change);
    }
    Q_EMIT valueChanged(value_);
}

void KAngleDial::setZero(const Zero zero) {
    zero_ = zero;
    update();
}

void KAngleDial::setCounterClockwise(const bool counterClockwise) {
    counterClockwise_ = counterClockwise;
    update();
}

void KAngleDial::setDefaultValue(const std::optional<double> degrees) { default_ = degrees; }

void KAngleDial::setResetText(const QString& text) { resetText_ = text; }

void KAngleDial::resetToDefault() {
    if (!default_ || !isEnabled())
        return;
    beginInteraction();
    commitValue(*default_);
    finishInteraction(true);
}

QRectF KAngleDial::discRect() const {
    // The focus ring is drawn outside the disc, in the margin kept for it.
    const qreal margin = kFocusRingWidth + kHairlineWidth;
    const qreal side = std::max<qreal>(0.0, std::min(width(), height()) - 2.0 * margin);
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

double KAngleDial::angleAt(const QPointF point) const {
    const auto centre = discRect().center();
    const double dx = point.x() - centre.x();
    const double dy = point.y() - centre.y();
    const double radians = zero_ == Zero::Up ? std::atan2(dx, -dy) : std::atan2(dy, dx);
    const double degrees = radians * 180.0 / std::numbers::pi;
    return wrapped(counterClockwise_ ? -degrees : degrees);
}

QPointF KAngleDial::needleTip() const {
    const auto disc = discRect();
    const double radius = disc.width() / 2.0 - kTickLength / 2.0;
    const double turned = counterClockwise_ ? -value_ : value_;
    const double radians = wrapped(turned) * std::numbers::pi / 180.0;
    const auto centre = disc.center();
    if (zero_ == Zero::Up)
        return {centre.x() + radius * std::sin(radians), centre.y() - radius * std::cos(radians)};
    return {centre.x() + radius * std::cos(radians), centre.y() + radius * std::sin(radians)};
}

State KAngleDial::visualState() const {
    if (!isEnabled())
        return State::Disabled;
    if (dragging_)
        return State::Pressed;
    if (hovered_)
        return State::Hover;
    if (hasKeyboardFocus(*this))
        return State::Focused;
    return State::Normal;
}

QSize KAngleDial::sizeHint() const {
    const int side = px(Size::Control) + px(Size::ToggleCell);
    return {side, side};
}

QSize KAngleDial::minimumSizeHint() const { return {px(Size::Control), px(Size::Control)}; }

void KAngleDial::beginInteraction() {
    if (interactionActive_)
        return;
    interactionActive_ = true;
    interactionBase_ = value_;
    Q_EMIT editStarted();
}

void KAngleDial::finishInteraction(const bool keep) {
    if (!interactionActive_)
        return;
    interactionActive_ = false;
    dragging_ = false;
    keyStepping_ = false;
    jumped_ = false;
    update();
    if (keep) {
        Q_EMIT editFinished();
        return;
    }
    {
        const QSignalBlocker blocker(this);
        commitValue(interactionBase_);
    }
    Q_EMIT valueChanged(value_);
    Q_EMIT editCancelled();
}

void KAngleDial::cancelEdit() { finishInteraction(false); }

void KAngleDial::startSegment(const QPointF point, const Qt::KeyboardModifiers modifiers) {
    lastPointer_ = angleAt(point);
    unwrapped_ = lastPointer_;
    segmentPointer_ = unwrapped_;
    segmentBase_ = value_;
    segmentModifiers_ = modifiers;
}

void KAngleDial::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && dragging_) {
        cancelEdit();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    beginInteraction();
    dragging_ = true;
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        // A Shift-press jumps the needle to the pointer by the shorter way round, so the winding
        // the value already has is kept.
        const double target = value_ + shortest(angleAt(event->position()) - wrapped(value_));
        commitValue(std::round(target / kSnapDegrees) * kSnapDegrees);
        jumped_ = true;
    }
    startSegment(event->position(), modifiers);
    update();
    event->accept();
}

void KAngleDial::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    event->accept();
    const auto centre = discRect().center();
    if (std::hypot(event->position().x() - centre.x(), event->position().y() - centre.y()) <
        kDeadZone)
        return;
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers != segmentModifiers_)
        // A modifier change continues from the value reached, so nothing jumps.
        startSegment(event->position(), modifiers);
    const double pointer = angleAt(event->position());
    unwrapped_ += shortest(pointer - lastPointer_);
    lastPointer_ = pointer;
    const double scale = modifiers.testFlag(Qt::ControlModifier) ? kFineScale : 1.0;
    double next = segmentBase_ + (unwrapped_ - segmentPointer_) * scale;
    if (modifiers.testFlag(Qt::ShiftModifier))
        next = std::round(next / kSnapDegrees) * kSnapDegrees;
    commitValue(next);
}

void KAngleDial::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KAngleDial::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && default_ && isEnabled()) {
        if (dragging_)
            finishInteraction(true);
        resetToDefault();
        event->accept();
        return;
    }
    mousePressEvent(event);
}

void KAngleDial::contextMenuEvent(QContextMenuEvent* event) {
    if (dragging_) {
        event->accept();
        return;
    }
    if (!default_ && actions().isEmpty()) {
        event->ignore();
        return;
    }
    std::unique_ptr<QMenu> menu(makeMenu(this));
    if (default_) {
        auto* reset = menu->addAction(resetText_);
        reset->setObjectName(objectName() + QStringLiteral("ResetAction"));
        reset->setEnabled(isEnabled() && value_ != *default_);
        connect(reset, &QAction::triggered, this, &KAngleDial::resetToDefault);
    }
    menu->addActions(actions());
    menu->exec(event->globalPos());
    event->accept();
}

void KAngleDial::keyPressEvent(QKeyEvent* event) {
    std::optional<double> target;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        target = value_ - 1.0;
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        target = value_ + 1.0;
        break;
    case Qt::Key_PageDown:
        target = value_ - kSnapDegrees;
        break;
    case Qt::Key_PageUp:
        target = value_ + kSnapDegrees;
        break;
    case Qt::Key_Home:
        target = default_.value_or(0.0);
        break;
    case Qt::Key_Escape:
        if (keyStepping_) {
            finishInteraction(true);
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    if (target && isEnabled()) {
        if (!interactionActive_) {
            beginInteraction();
            keyStepping_ = true;
        }
        commitValue(*target);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KAngleDial::keyReleaseEvent(QKeyEvent* event) {
    if (keyStepping_ && !event->isAutoRepeat() && event->key() != Qt::Key_Tab &&
        event->key() != Qt::Key_Backtab && event->key() != Qt::Key_Shift) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void KAngleDial::focusOutEvent(QFocusEvent* event) {
    if (keyStepping_)
        finishInteraction(true);
    QWidget::focusOutEvent(event);
}

bool KAngleDial::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (hasFocus() && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
                           key == Qt::Key_Down || key == Qt::Key_PageUp ||
                           key == Qt::Key_PageDown || key == Qt::Key_Home ||
                           (key == Qt::Key_Escape && keyStepping_))) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        if (dragging_ || (event->type() == QEvent::WindowDeactivate && keyStepping_))
            finishInteraction(true);
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void KAngleDial::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KAngleDial::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KAngleDial::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
        finishInteraction(true);
    }
    QWidget::changeEvent(event);
}

void KAngleDial::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const State state = visualState();
    const auto disc = discRect();
    const auto centre = disc.center();
    const qreal radius = disc.width() / 2.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush(color(surfaceForState(Color::Field, state)));
    painter.drawEllipse(disc);
    painter.setBrush(Qt::NoBrush);
    applyHairlinePen(painter, color(borderForState(state)));
    painter.drawEllipse(disc);
    // Zero's tick on the rim, in muted ink.
    applyHairlinePen(painter, color(Color::Muted));
    if (zero_ == Zero::Up)
        painter.drawLine(QPointF(centre.x(), disc.top()),
                         QPointF(centre.x(), disc.top() + kTickLength));
    else
        painter.drawLine(QPointF(disc.right(), centre.y()),
                         QPointF(disc.right() - kTickLength, centre.y()));
    // The needle, in accent, from the hub to the rim.
    QColor accent = color(dragging_ ? Color::AccentPressed : Color::Accent);
    if (state == State::Disabled)
        accent = withOpacity(accent, kDisabledOpacity);
    QPen needle(accent);
    needle.setWidthF(kEmphasisStroke);
    needle.setCapStyle(Qt::RoundCap);
    painter.setPen(needle);
    if (radius > kTickLength)
        painter.drawLine(centre, needleTip());
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawEllipse(centre, kHubRadius, kHubRadius);
    if (hasKeyboardFocus(*this) && state != State::Disabled)
        drawFocusRing(painter, disc, Radius::Full);
}

} // namespace bloom::ui::kit

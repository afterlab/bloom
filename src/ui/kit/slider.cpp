#include <bloom/ui/kit/slider.hpp>

#include "pointer_wrap.hpp"

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QAccessible>
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>

namespace bloom::ui::kit {
namespace {

constexpr int kTrackHeight = px(Size::SliderTrack);
// A gradient track is drawn thicker, so its colours read at a glance.
constexpr int kGradientTrackHeight = px(Size::ProgressTrack);
constexpr int kHandleDiameter = px(Size::SliderHandle);
// A full-speed Relative drag snaps to the neutral value within this many pixels of it.
constexpr double kDetentPixels = px(Spacing::XS);
// A jog strip's knurl: one ridge every this many pixels, scrolling with the value.
constexpr double kKnurlPitch = px(Spacing::XS);
// Ticks past this many along the track would read as a texture, so none are drawn.
constexpr int kMostTicks = 64;

} // namespace

KSlider::KSlider(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    setFixedHeight(px(Size::Control));
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kSlider"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    resetText_ = tr("Reset");
}

void KSlider::setRange(const double minimum, const double maximum) {
    minimum_ = minimum;
    // A collapsed range would make every position mean every value; keep the maximum strictly
    // above the minimum and let normalizedValue() stay well defined.
    maximum_ = std::max(maximum, minimum + std::numeric_limits<double>::epsilon());
    commitValue(value_);
    update();
}

double KSlider::minimum() const noexcept { return minimum_; }

double KSlider::maximum() const noexcept { return maximum_; }

double KSlider::value() const noexcept { return value_; }

void KSlider::setValue(const double value) {
    // A drag owns the handle until it ends, as a scrub owns a value field.
    if (dragging_)
        return;
    commitValue(value);
}

void KSlider::setDragMode(const DragMode mode) {
    mode_ = mode;
    // A grading drag is a horizontal scrub, and says so under the pointer.
    setCursor(mode == DragMode::Absolute ? Qt::PointingHandCursor : Qt::SizeHorCursor);
    update();
}

void KSlider::setLimits(const double minimum, const double maximum) {
    limitMinimum_ = minimum;
    limitMaximum_ = std::max(minimum, maximum);
    commitValue(value_);
}

void KSlider::setJogStep(const double step) {
    if (std::isfinite(step) && step > 0.0)
        jogStep_ = step;
}

void KSlider::setNeutral(const std::optional<double> neutral) {
    neutral_ = neutral;
    update();
}

void KSlider::setTickInterval(const double interval) {
    tickInterval_ = std::isfinite(interval) && interval > 0.0 ? interval : 0.0;
    update();
}

void KSlider::setTrackGradient(const QGradientStops& stops) {
    gradient_ = stops;
    update();
}

void KSlider::setDefaultValue(const std::optional<double> value) { default_ = value; }

void KSlider::setResetText(const QString& text) { resetText_ = text; }

void KSlider::resetToDefault() {
    if (!default_ || !isEnabled() || dragging_)
        return;
    beginInteraction();
    commitValue(*default_);
    finishInteraction(true);
}

double KSlider::lowLimit() const noexcept {
    if (mode_ == DragMode::Absolute)
        return minimum_;
    return limitMinimum_.value_or(minimum_);
}

double KSlider::highLimit() const noexcept {
    if (mode_ == DragMode::Absolute)
        return maximum_;
    return limitMaximum_.value_or(maximum_);
}

bool KSlider::overflows() const noexcept { return value_ < minimum_ || value_ > maximum_; }

double KSlider::dragScale(const Qt::KeyboardModifiers modifiers) noexcept {
    if (modifiers.testFlag(Qt::ShiftModifier))
        return kCoarseStepMultiple;
    if (modifiers.testFlag(Qt::ControlModifier))
        return 1.0 / kCoarseStepMultiple;
    return 1.0;
}

void KSlider::commitValue(const double value) {
    if (!std::isfinite(value))
        return;
    const double clamped = std::clamp(value, lowLimit(), highLimit());
    if (clamped == value_) {
        return;
    }
    value_ = clamped;
    update();
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent change(this, value_);
        QAccessible::updateAccessibility(&change);
    }
    Q_EMIT valueChanged(value_);
}

void KSlider::beginInteraction() {
    if (interactionActive_)
        return;
    interactionActive_ = true;
    interactionBase_ = value_;
    Q_EMIT editStarted();
}

void KSlider::finishInteraction(const bool keep) {
    if (!interactionActive_)
        return;
    interactionActive_ = false;
    dragging_ = false;
    keyStepping_ = false;
    update();
    if (keep) {
        Q_EMIT editFinished();
        return;
    }
    {
        const QSignalBlocker blocker(this);
        commitValue(interactionBase_);
    }
    Q_EMIT editCancelled();
}

void KSlider::cancelEdit() { finishInteraction(false); }

double KSlider::normalizedValue() const {
    return std::clamp((value_ - minimum_) / (maximum_ - minimum_), 0.0, 1.0);
}

QRectF KSlider::trackRect() const {
    if (mode_ == DragMode::Jog)
        return QRectF(rect()).adjusted(0.0, px(Spacing::XXS), 0.0, -px(Spacing::XXS));
    const auto handle = static_cast<qreal>(kHandleDiameter);
    const qreal thickness = gradient_.isEmpty() ? kTrackHeight : kGradientTrackHeight;
    return {handle / 2.0, (height() - thickness) / 2.0, std::max(0.0, width() - handle),
            thickness};
}

double KSlider::positionOf(const double value) const {
    const QRectF track = trackRect();
    const double t = std::clamp((value - minimum_) / (maximum_ - minimum_), 0.0, 1.0);
    return track.left() + track.width() * t;
}

QRectF KSlider::fillRect() const {
    const QRectF track = trackRect();
    const double from = positionOf(neutral_.value_or(minimum_));
    const double to = positionOf(value_);
    return {std::min(from, to), track.top(), std::abs(to - from), track.height()};
}

QRectF KSlider::handleRect() const {
    const QRectF track = trackRect();
    const auto handle = static_cast<qreal>(kHandleDiameter);
    const qreal centre = positionOf(value_);
    return {centre - handle / 2.0, track.center().y() - handle / 2.0, handle, handle};
}

double KSlider::valueForPosition(const double x) const {
    const QRectF track = trackRect();
    if (track.width() <= 0.0) {
        return minimum_;
    }
    const double normalized = std::clamp((x - track.left()) / track.width(), 0.0, 1.0);
    return minimum_ + normalized * (maximum_ - minimum_);
}

State KSlider::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (dragging_) {
        return State::Pressed;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (hasKeyboardFocus(*this)) {
        return State::Focused;
    }
    return State::Normal;
}

Color KSlider::borderToken() const {
    return borderForInteraction(isEnabled(), hasKeyboardFocus(*this), hovered_);
}

bool KSlider::isDragging() const noexcept { return dragging_; }

QSize KSlider::sizeHint() const { return {px(Size::ControlRoomy) * 4, px(Size::Control)}; }

QSize KSlider::minimumSizeHint() const { return {kHandleDiameter * 3, px(Size::Control)}; }

void KSlider::startSegment(const double x, const Qt::KeyboardModifiers modifiers,
                           const double scale) {
    segmentX_ = x;
    segmentBase_ = value_;
    segmentModifiers_ = modifiers & (Qt::ShiftModifier | Qt::ControlModifier);
    segmentScale_ = scale;
}

void KSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    beginInteraction();
    dragging_ = true;
    const qreal x = event->position().x();
    if (mode_ != DragMode::Absolute) {
        wrapOffset_ = 0.0;
        const auto modifiers = event->modifiers();
        // A Shift-press on a track jumps the value to the pointer, which it then follows 1:1 until
        // a modifier changes; any other press keeps the value where it is.
        if (mode_ == DragMode::Relative && modifiers.testFlag(Qt::ShiftModifier)) {
            commitValue(valueForPosition(x));
            startSegment(x, modifiers, 1.0);
        } else {
            startSegment(x, modifiers, dragScale(modifiers));
        }
        update();
        event->accept();
        return;
    }
    const QRectF handle = handleRect();
    if (x >= handle.left() && x <= handle.right()) {
        // Pressing the handle grabs it where it was pressed: nothing moves until the pointer does.
        grabOffset_ = x - handle.center().x();
    } else {
        // Pressing the bare track takes the value there immediately rather than nudging toward
        // it: the artist asked for that value by pointing at it.
        grabOffset_ = 0.0;
        commitValue(valueForPosition(x));
    }
    update();
    event->accept();
}

void KSlider::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    event->accept();
    if (mode_ == DragMode::Absolute) {
        commitValue(valueForPosition(event->position().x() - grabOffset_));
        return;
    }
    const double x = event->position().x() + wrapOffset_;
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    // A modifier change starts a new segment from the value reached, so nothing jumps.
    if (modifiers != segmentModifiers_)
        startSegment(x, modifiers, dragScale(modifiers));
    const double travel = (x - segmentX_) * segmentScale_;
    const QRectF track = trackRect();
    const double perPixel = mode_ == DragMode::Jog
                                ? jogStep_
                                : (maximum_ - minimum_) / std::max<qreal>(1.0, track.width());
    double next = segmentBase_ + travel * perPixel;
    // The neutral detent: a full-speed drag that comes close to neutral rests on it.
    if (mode_ == DragMode::Relative && neutral_ && segmentScale_ == 1.0 &&
        std::abs(positionOf(next) - positionOf(*neutral_)) < kDetentPixels &&
        next >= minimum_ && next <= maximum_)
        next = *neutral_;
    // A Relative slider stops at its track's ends; a jog runs on to its hard limits.
    if (mode_ == DragMode::Relative)
        next = std::clamp(next, std::min(minimum_, value_), std::max(maximum_, value_));
    commitValue(next);
    if (mode_ == DragMode::Jog)
        wrapPointerAtScreenEdge(*this, event->globalPosition(), wrapOffset_);
}

void KSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KSlider::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && default_ && isEnabled()) {
        // The first click of the pair was a press that moved nothing; the double-click resets.
        if (dragging_)
            finishInteraction(true);
        resetToDefault();
        event->accept();
        return;
    }
    mousePressEvent(event);
}

void KSlider::contextMenuEvent(QContextMenuEvent* event) {
    if (dragging_) {
        event->accept();
        return;
    }
    if (!default_.has_value() && actions().isEmpty()) {
        event->ignore();
        return;
    }
    std::unique_ptr<QMenu> menu(makeMenu(this));
    if (default_.has_value()) {
        auto* reset = menu->addAction(resetText_);
        reset->setObjectName(objectName() + QStringLiteral("ResetAction"));
        reset->setEnabled(isEnabled() && value_ != *default_);
        connect(reset, &QAction::triggered, this, &KSlider::resetToDefault);
    }
    menu->addActions(actions());
    menu->exec(event->globalPos());
    event->accept();
}

void KSlider::keyPressEvent(QKeyEvent* event) {
    const double span = maximum_ - minimum_;
    std::optional<double> target;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        target = value_ - span * kKeyStepFraction;
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        target = value_ + span * kKeyStepFraction;
        break;
    case Qt::Key_PageDown:
        target = value_ - span * kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_PageUp:
        target = value_ + span * kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_Home:
        target = minimum_;
        break;
    case Qt::Key_End:
        target = maximum_;
        break;
    case Qt::Key_Escape:
        // Escape confirms a key edit in progress; at rest it belongs to whatever is around.
        if (keyStepping_) {
            finishInteraction(true);
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    if (target.has_value() && isEnabled()) {
        // Held or repeated keys are one edit, ending on the release.
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

void KSlider::keyReleaseEvent(QKeyEvent* event) {
    if (keyStepping_ && !event->isAutoRepeat()) {
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            finishInteraction(true);
            event->accept();
            return;
        default:
            break;
        }
    }
    QWidget::keyReleaseEvent(event);
}

void KSlider::focusOutEvent(QFocusEvent* event) {
    if (keyStepping_)
        finishInteraction(true);
    QWidget::focusOutEvent(event);
}

bool KSlider::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // A focused slider steps with the arrows, so the frame-step shortcuts stand down.
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (hasFocus() && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
                           key == Qt::Key_Down || key == Qt::Key_PageUp ||
                           key == Qt::Key_PageDown || key == Qt::Key_Home || key == Qt::Key_End ||
                           (key == Qt::Key_Escape && keyStepping_))) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        // The drag ends where it is, as a commit, when the pointer or the window goes away.
        if (dragging_ || (event->type() == QEvent::WindowDeactivate && keyStepping_))
            finishInteraction(true);
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void KSlider::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KSlider::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KSlider::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
        finishInteraction(true);
    }
    QWidget::changeEvent(event);
}

void KSlider::paintTicks(QPainter& painter, const QRectF& track) const {
    const QColor ink = withOpacity(color(Color::Muted), isEnabled() ? 1.0 : kDisabledOpacity);
    applyHairlinePen(painter, ink);
    const qreal below = track.bottom() + px(Spacing::XXS);
    if (tickInterval_ > 0.0 && (maximum_ - minimum_) / tickInterval_ <= kMostTicks) {
        for (double tick = std::ceil(minimum_ / tickInterval_) * tickInterval_;
             tick <= maximum_ + tickInterval_ * 1e-9; tick += tickInterval_) {
            const qreal x = std::round(positionOf(tick)) + 0.5;
            painter.drawLine(QPointF(x, below), QPointF(x, below + px(Size::MinorTick)));
        }
    }
    if (neutral_ && *neutral_ >= minimum_ && *neutral_ <= maximum_) {
        // Neutral is marked across the track, taller than the other ticks.
        const qreal x = std::round(positionOf(*neutral_)) + 0.5;
        const qreal reach = px(Size::MajorTick) / 2.0;
        applyHairlinePen(painter, withOpacity(color(Color::Foreground),
                                              isEnabled() ? 0.7 : kDisabledOpacity));
        painter.drawLine(QPointF(x, track.center().y() - reach),
                         QPointF(x, track.center().y() + reach));
    }
}

void KSlider::paintOverflow(QPainter& painter, const QRectF& track) const {
    if (!overflows())
        return;
    // A chevron at the end the value lies beyond: the bar is capped, not the value.
    const bool high = value_ > maximum_;
    const qreal size = px(Spacing::XS) + px(Spacing::XXS);
    const qreal tip = high ? track.right() : track.left();
    const qreal base = high ? tip - size : tip + size;
    const qreal y = track.center().y();
    QPainterPath chevron;
    chevron.moveTo(base, y - size);
    chevron.lineTo(tip, y);
    chevron.lineTo(base, y + size);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color(dragging_ ? Color::Accent : Color::Foreground), kEmphasisStroke,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(chevron);
    painter.restore();
}

void KSlider::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();
    const QRectF track = trackRect();

    QColor accent = color(Color::Accent);
    if (state == State::Hover) {
        accent = color(Color::AccentHover);
    } else if (state == State::Pressed) {
        accent = color(Color::AccentPressed);
    } else if (state == State::Disabled) {
        accent = withOpacity(accent, kDisabledOpacity);
    }

    if (mode_ == DragMode::Jog) {
        // A jog strip: the track (dark to light when it has a gradient), a knurl that scrolls with
        // the value, neutral marked across it, and a bar from neutral to the value along its foot.
        QLinearGradient ramp(track.topLeft(), track.topRight());
        ramp.setStops(gradient_.isEmpty()
                          ? QGradientStops{{0.0, color(Color::Field)}, {1.0, color(Color::Field)}}
                          : gradient_);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath clip;
        clip.addRoundedRect(track, px(Spacing::XXS), px(Spacing::XXS));
        painter.setClipPath(clip);
        painter.fillRect(track, ramp);
        const double phase = std::fmod(value_ / jogStep_, kKnurlPitch);
        QColor ridge = color(Color::Background);
        ridge.setAlphaF(state == State::Disabled ? 0.15F : 0.35F);
        applyHairlinePen(painter, ridge);
        for (double x = track.left() - phase - kKnurlPitch; x <= track.right() + kKnurlPitch;
             x += kKnurlPitch)
            painter.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
        const qreal bar = px(Spacing::XS) - px(Spacing::XXS) / 2.0;
        const QRectF fill = fillRect();
        painter.fillRect(QRectF(fill.left(), track.bottom() - bar, fill.width(), bar),
                         dragging_ ? accent
                                   : withOpacity(color(Color::Foreground),
                                                 state == State::Disabled ? kDisabledOpacity
                                                                          : 0.8));
        painter.restore();
        fillRoundedSurface(painter, track, Qt::transparent, color(borderToken()), Radius::Small);
        if (neutral_ && *neutral_ >= minimum_ && *neutral_ <= maximum_) {
            const qreal x = std::round(positionOf(*neutral_)) + 0.5;
            applyHairlinePen(painter, withOpacity(color(Color::Foreground),
                                                  isEnabled() ? 0.9 : kDisabledOpacity));
            painter.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
        }
        paintOverflow(painter, track.adjusted(px(Spacing::XS), 0.0, -px(Spacing::XS), 0.0));
        return;
    }

    if (gradient_.isEmpty()) {
        fillRoundedSurface(painter, track, color(surfaceForState(Color::Field, state)), {},
                           Radius::Full);
        const QRectF filled = fillRect();
        if (filled.width() > 0.0) {
            fillRoundedSurface(painter, filled, accent, {}, Radius::Full);
        }
    } else {
        QLinearGradient ramp(track.topLeft(), track.topRight());
        QGradientStops stops = gradient_;
        if (state == State::Disabled)
            for (auto& stop : stops)
                stop.second = withOpacity(stop.second, kDisabledOpacity);
        ramp.setStops(stops);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ramp);
        painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
        painter.restore();
    }
    paintTicks(painter, track);
    paintOverflow(painter, track);

    QColor handleFill = color(Color::Foreground);
    if (state == State::Disabled) {
        handleFill = withOpacity(handleFill, kDisabledOpacity);
    }
    // The handle's own single border carries focus; the ring outside it is gone.
    fillRoundedSurface(painter, handleRect(), handleFill, color(borderToken()), Radius::Full);
}

} // namespace bloom::ui::kit

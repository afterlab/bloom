#include <bloom/ui/kit/gradient_editor.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>

#include <QAction>
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
#include <memory>
#include <utility>

namespace bloom::ui::kit {
namespace {

constexpr int kBarHeight = px(Size::Control);
// The band of stop handles under the bar.
constexpr int kHandleBand = px(Size::IconLarge);
constexpr qreal kHandleHalfWidth = px(Size::IconSmall) / 2.0;
// How far below the bar a dragged stop is pulled before it is removed.
constexpr qreal kRemoveDistance = px(Size::ControlRoomy);
// Samples drawn between the stops, so a gradient between two display-converted colours reads as
// the node computes it (in Reference RGB) rather than as a display-space blend.
constexpr int kGradientSamples = 48;
constexpr int kCheckerCell = px(Spacing::XS);

[[nodiscard]] float lerp(const float from, const float to, const double t) {
    return static_cast<float>(static_cast<double>(from) +
                              (static_cast<double>(to) - static_cast<double>(from)) * t);
}

} // namespace

KGradientEditor::KGradientEditor(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kGradientEditor"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setAccessibleName(tr("Gradient"));
    setToolTip(tr("Drag a stop to move it, pull it down to remove it, click the bar to add one and "
                  "double-click a stop to pick its colour"));
}

void KGradientEditor::setColorConverter(KColorConverter converter) {
    converter_ = std::move(converter);
    update();
}

void KGradientEditor::setStops(std::vector<Stop> stops) {
    if (interactionActive_)
        return;
    for (std::size_t index = 0; index < stops.size(); ++index)
        stops[index].origin = index;
    std::ranges::stable_sort(stops, {}, &Stop::position);
    if (stops == stops_)
        return;
    stops_ = std::move(stops);
    if (selected_ && *selected_ >= stops_.size()) {
        selected_ = stops_.empty() ? std::nullopt : std::optional<std::size_t>{stops_.size() - 1};
        Q_EMIT selectedStopChanged();
    }
    update();
}

void KGradientEditor::setStopLimits(const std::size_t minimum, const std::size_t maximum) {
    minimumStops_ = std::max<std::size_t>(1, minimum);
    maximumStops_ = std::max(minimumStops_, maximum);
}

void KGradientEditor::setSelectedStop(const std::optional<std::size_t> index) {
    const auto next = index && *index < stops_.size() ? index : std::nullopt;
    if (next == selected_)
        return;
    selected_ = next;
    update();
    Q_EMIT selectedStopChanged();
}

void KGradientEditor::setColorSampler(ColorSampler sampler) {
    sampler_ = std::move(sampler);
    update();
}

KColor KGradientEditor::colorAt(const double position) const {
    if (stops_.empty())
        return KColor::fromRgba(0.0F, 0.0F, 0.0F, 1.0F, ColorSpace::Reference);
    if (sampler_)
        return sampler_(stops_, position);
    if (position <= stops_.front().position)
        return stops_.front().color;
    for (std::size_t index = 1; index < stops_.size(); ++index) {
        const auto& from = stops_[index - 1];
        const auto& to = stops_[index];
        if (position < to.position) {
            const double span = to.position - from.position;
            const double t = span > 0.0 ? (position - from.position) / span : 1.0;
            return KColor::fromRgba(lerp(from.color.red, to.color.red, t),
                                    lerp(from.color.green, to.color.green, t),
                                    lerp(from.color.blue, to.color.blue, t),
                                    lerp(from.color.alpha, to.color.alpha, t),
                                    ColorSpace::Reference);
        }
    }
    return stops_.back().color;
}

QColor KGradientEditor::displayed(const KColor& color) const {
    if (color.space == ColorSpace::Reference && converter_)
        if (const auto display = color.converted(ColorSpace::Display, converter_))
            return display->toQColor();
    return color.toQColor();
}

QColor KGradientEditor::displayColorAt(const double position) const {
    return displayed(colorAt(std::clamp(position, 0.0, 1.0)));
}

QRectF KGradientEditor::barRect() const {
    // Inset by half a handle on each side, so the end stops sit fully inside the widget.
    return {kHandleHalfWidth, 0.0, std::max<qreal>(0.0, width() - 2.0 * kHandleHalfWidth),
            static_cast<qreal>(kBarHeight)};
}

QPointF KGradientEditor::stopCentre(const std::size_t index) const {
    const auto bar = barRect();
    const double position = index < stops_.size() ? stops_[index].position : 0.0;
    return {bar.left() + position * bar.width(), bar.bottom() + kHandleBand / 2.0};
}

double KGradientEditor::positionAt(const double x) const {
    const auto bar = barRect();
    if (bar.width() <= 0.0)
        return 0.0;
    return std::clamp((x - bar.left()) / bar.width(), 0.0, 1.0);
}

std::optional<std::size_t> KGradientEditor::stopAt(const QPointF point) const {
    // Only the handle band takes a stop; the bar itself adds one. The nearer handle wins, and the
    // selected one wins a tie so a stop dropped on another can be picked up again.
    const auto bar = barRect();
    if (point.y() < bar.bottom() - px(Spacing::XXS) ||
        point.y() > bar.bottom() + kHandleBand + px(Spacing::XS))
        return std::nullopt;
    std::optional<std::size_t> best;
    qreal nearest = kHandleHalfWidth + px(Spacing::XXS);
    for (std::size_t index = 0; index < stops_.size(); ++index) {
        const qreal distance = std::abs(point.x() - stopCentre(index).x());
        if (distance < nearest || (distance == nearest && selected_ == index)) {
            nearest = distance;
            best = index;
        }
    }
    return best;
}

State KGradientEditor::visualState() const {
    if (!isEnabled())
        return State::Disabled;
    if (dragged_)
        return State::Pressed;
    if (hovered_)
        return State::Hover;
    if (hasKeyboardFocus(*this))
        return State::Focused;
    return State::Normal;
}

QSize KGradientEditor::sizeHint() const {
    return {px(Size::DropdownWidthExpanded), kBarHeight + kHandleBand + px(Spacing::XXS)};
}

QSize KGradientEditor::minimumSizeHint() const {
    return {px(Size::ValueCellMin), kBarHeight + kHandleBand + px(Spacing::XXS)};
}

void KGradientEditor::beginInteraction() {
    if (interactionActive_)
        return;
    interactionActive_ = true;
    base_ = stops_;
    Q_EMIT editStarted();
}

void KGradientEditor::finishInteraction(const bool keep) {
    if (!interactionActive_)
        return;
    interactionActive_ = false;
    dragged_.reset();
    pulled_.reset();
    removalPending_ = false;
    keyStepping_ = false;
    moved_ = false;
    update();
    if (keep) {
        Q_EMIT editFinished();
        return;
    }
    stops_ = base_;
    if (selected_ && *selected_ >= stops_.size())
        selected_.reset();
    Q_EMIT stopsChanged();
    Q_EMIT editCancelled();
}

void KGradientEditor::cancelEdit() { finishInteraction(false); }

void KGradientEditor::emitChanged() {
    update();
    Q_EMIT stopsChanged();
}

std::size_t KGradientEditor::moveStop(std::size_t index, const double position) {
    stops_[index].position = std::clamp(position, 0.0, 1.0);
    // Passing a neighbour reorders the stops; the moved stop stays the one held and selected.
    while (index > 0 && stops_[index - 1].position > stops_[index].position) {
        std::swap(stops_[index - 1], stops_[index]);
        --index;
    }
    while (index + 1 < stops_.size() && stops_[index + 1].position < stops_[index].position) {
        std::swap(stops_[index + 1], stops_[index]);
        ++index;
    }
    if (selected_ != index) {
        selected_ = index;
        Q_EMIT selectedStopChanged();
    }
    return index;
}

void KGradientEditor::resort() {
    std::vector<std::size_t> order(stops_.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::ranges::stable_sort(order, {},
                             [this](const std::size_t index) { return stops_[index].position; });
    std::vector<Stop> sorted;
    sorted.reserve(stops_.size());
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (selected_ == order[index])
            selected = index;
        sorted.push_back(stops_[order[index]]);
    }
    stops_ = std::move(sorted);
    if (selected != selected_) {
        selected_ = selected;
        Q_EMIT selectedStopChanged();
    }
}

bool KGradientEditor::addStop(const double position) {
    if (!isEnabled() || stops_.size() >= maximumStops_ || !std::isfinite(position))
        return false;
    const double at = std::clamp(position, 0.0, 1.0);
    beginInteraction();
    const auto colour = colorAt(at);
    const auto insert = std::ranges::upper_bound(stops_, at, {}, &Stop::position);
    const auto index = static_cast<std::size_t>(insert - stops_.begin());
    stops_.insert(insert, Stop{at, colour});
    selected_ = index;
    Q_EMIT selectedStopChanged();
    emitChanged();
    finishInteraction(true);
    return true;
}

bool KGradientEditor::removeStop(const std::size_t index) {
    if (!isEnabled() || index >= stops_.size() || stops_.size() <= minimumStops_)
        return false;
    beginInteraction();
    stops_.erase(stops_.begin() + static_cast<std::ptrdiff_t>(index));
    selected_ = stops_.empty() ? std::nullopt
                               : std::optional<std::size_t>{std::min(index, stops_.size() - 1)};
    Q_EMIT selectedStopChanged();
    emitChanged();
    finishInteraction(true);
    return true;
}

bool KGradientEditor::setStopPosition(const std::size_t index, const double position) {
    if (!isEnabled() || index >= stops_.size() || !std::isfinite(position) ||
        stops_[index].positionLinked)
        return false;
    beginInteraction();
    moveStop(index, position);
    emitChanged();
    finishInteraction(true);
    return true;
}

bool KGradientEditor::setStopColor(const std::size_t index, const KColor& color) {
    if (index >= stops_.size() || stops_[index].colorLinked)
        return false;
    auto reference = color;
    if (reference.space != ColorSpace::Reference) {
        const auto converted =
            converter_ ? color.converted(ColorSpace::Reference, converter_) : std::nullopt;
        if (!converted)
            return false;
        reference = *converted;
    }
    if (stops_[index].color == reference)
        return true;
    stops_[index].color = reference;
    emitChanged();
    return true;
}

void KGradientEditor::distributeStops() {
    if (!isEnabled() || stops_.size() < 2)
        return;
    beginInteraction();
    // A linked position stays where its link puts it.
    for (std::size_t index = 0; index < stops_.size(); ++index)
        if (!stops_[index].positionLinked)
            stops_[index].position =
                static_cast<double>(index) / static_cast<double>(stops_.size() - 1);
    resort();
    emitChanged();
    finishInteraction(true);
}

void KGradientEditor::reverseStops() {
    if (!isEnabled() || stops_.empty())
        return;
    beginInteraction();
    std::ranges::reverse(stops_);
    for (auto& stop : stops_)
        if (!stop.positionLinked)
            stop.position = 1.0 - stop.position;
    if (selected_) {
        selected_ = stops_.size() - 1 - *selected_;
        Q_EMIT selectedStopChanged();
    }
    resort();
    emitChanged();
    finishInteraction(true);
}

void KGradientEditor::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && dragged_) {
        cancelEdit();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    event->accept();
    auto index = stopAt(event->position());
    if (!index) {
        const auto bar = barRect();
        if (!bar.adjusted(0.0, 0.0, 0.0, px(Spacing::XXS)).contains(event->position()) ||
            stops_.size() >= maximumStops_)
            return;
        // A press on the bare bar adds a stop there, which the drag then places.
        beginInteraction();
        const double at = positionAt(event->position().x());
        const auto insert = std::ranges::upper_bound(stops_, at, {}, &Stop::position);
        index = static_cast<std::size_t>(insert - stops_.begin());
        stops_.insert(insert, Stop{at, colorAt(at)});
        moved_ = true;
        emitChanged();
    } else if (stops_[*index].positionLinked) {
        // A linked position is not dragged: the press selects the stop.
        setSelectedStop(index);
        return;
    } else {
        beginInteraction();
    }
    if (selected_ != index) {
        selected_ = index;
        Q_EMIT selectedStopChanged();
    }
    dragged_ = index;
    grabOffset_ = event->position().x() - stopCentre(*index).x();
    segmentX_ = event->position().x();
    segmentBase_ = stops_[*index].position;
    segmentModifiers_ = event->modifiers() & Qt::ControlModifier;
    update();
}

void KGradientEditor::mouseMoveEvent(QMouseEvent* event) {
    if (!dragged_ && !pulled_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    event->accept();
    const auto bar = barRect();
    const auto modifiers = event->modifiers() & Qt::ControlModifier;
    const bool off = event->position().y() > bar.bottom() + kHandleBand + kRemoveDistance ||
                     event->position().y() < bar.top() - kRemoveDistance;
    if (off && !pulled_ && dragged_ && stops_.size() > minimumStops_) {
        // Pulled off the bar: the stop goes, and comes back if the pointer returns.
        pulled_ = stops_[*dragged_];
        stops_.erase(stops_.begin() + static_cast<std::ptrdiff_t>(*dragged_));
        dragged_.reset();
        selected_.reset();
        removalPending_ = true;
        moved_ = true;
        Q_EMIT selectedStopChanged();
        emitChanged();
        return;
    }
    if (pulled_) {
        if (off)
            return;
        const auto insert = std::ranges::upper_bound(stops_, pulled_->position, {}, &Stop::position);
        const auto index = static_cast<std::size_t>(insert - stops_.begin());
        stops_.insert(insert, *pulled_);
        pulled_.reset();
        removalPending_ = false;
        dragged_ = index;
        selected_ = index;
        Q_EMIT selectedStopChanged();
        segmentX_ = event->position().x();
        segmentBase_ = stops_[index].position;
        segmentModifiers_ = modifiers;
        emitChanged();
    }
    if (modifiers != segmentModifiers_) {
        // A modifier change continues from the position reached, so nothing jumps.
        segmentX_ = event->position().x();
        segmentBase_ = stops_[*dragged_].position;
        segmentModifiers_ = modifiers;
    }
    const double scale = KSlider::dragScale(modifiers);
    const double dx = event->position().x() - segmentX_;
    const double next = segmentBase_ + dx * scale / std::max<qreal>(1.0, bar.width());
    if (dx != 0.0)
        moved_ = true;
    if (next == stops_[*dragged_].position)
        return;
    dragged_ = moveStop(*dragged_, next);
    emitChanged();
}

void KGradientEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || (!dragged_ && !pulled_)) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    event->accept();
    // A press that moved nothing is still a complete (empty) edit; the owner commits nothing.
    finishInteraction(true);
}

void KGradientEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (const auto index = stopAt(event->position())) {
        finishInteraction(true);
        setSelectedStop(index);
        if (!stops_[*index].colorLinked)
            Q_EMIT stopColorRequested(*index);
        event->accept();
        return;
    }
    mousePressEvent(event);
}

void KGradientEditor::contextMenuEvent(QContextMenuEvent* event) {
    if (dragged_ || pulled_) {
        event->accept();
        return;
    }
    if (const auto index = stopAt(event->pos()))
        setSelectedStop(index);
    const double at = positionAt(event->pos().x());
    std::unique_ptr<QMenu> menu(makeMenu(this));
    auto* add = menu->addAction(tr("Add Stop"));
    add->setObjectName(QStringLiteral("kGradientEditorAddStop"));
    add->setEnabled(isEnabled() && stops_.size() < maximumStops_);
    connect(add, &QAction::triggered, this, [this, at] { static_cast<void>(addStop(at)); });
    auto* remove = menu->addAction(tr("Remove Stop"));
    remove->setObjectName(QStringLiteral("kGradientEditorRemoveStop"));
    remove->setEnabled(isEnabled() && selected_ && stops_.size() > minimumStops_);
    connect(remove, &QAction::triggered, this, [this] {
        if (selected_)
            static_cast<void>(removeStop(*selected_));
    });
    menu->addSeparator();
    auto* distribute = menu->addAction(tr("Distribute Evenly"));
    distribute->setObjectName(QStringLiteral("kGradientEditorDistribute"));
    distribute->setEnabled(isEnabled() && stops_.size() > 2);
    connect(distribute, &QAction::triggered, this, &KGradientEditor::distributeStops);
    auto* reverse = menu->addAction(tr("Reverse"));
    reverse->setObjectName(QStringLiteral("kGradientEditorReverse"));
    reverse->setEnabled(isEnabled() && stops_.size() > 1);
    connect(reverse, &QAction::triggered, this, &KGradientEditor::reverseStops);
    menu->addActions(actions());
    menu->exec(event->globalPos());
    event->accept();
}

void KGradientEditor::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && (dragged_ || pulled_)) {
        cancelEdit();
        event->accept();
        return;
    }
    if (!isEnabled() || !selected_ || *selected_ >= stops_.size()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const bool coarse = event->modifiers().testFlag(Qt::ShiftModifier);
    const double step = kKeyStepFraction * (coarse ? kCoarseStepMultiple : 1.0);
    std::optional<double> target;
    const double current = stops_[*selected_].position;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        target = current - step;
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        target = current + step;
        break;
    case Qt::Key_PageDown:
        target = current - kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_PageUp:
        target = current + kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_Home:
        target = 0.0;
        break;
    case Qt::Key_End:
        target = 1.0;
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (keyStepping_)
            finishInteraction(true);
        static_cast<void>(removeStop(*selected_));
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!stops_[*selected_].colorLinked)
            Q_EMIT stopColorRequested(*selected_);
        event->accept();
        return;
    default:
        break;
    }
    if (target && stops_[*selected_].positionLinked) {
        event->accept();
        return;
    }
    if (!target) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (!interactionActive_) {
        beginInteraction();
        keyStepping_ = true;
    }
    moveStop(*selected_, *target);
    emitChanged();
    event->accept();
}

void KGradientEditor::keyReleaseEvent(QKeyEvent* event) {
    if (keyStepping_ && !event->isAutoRepeat() && event->key() != Qt::Key_Shift &&
        event->key() != Qt::Key_Tab && event->key() != Qt::Key_Backtab) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void KGradientEditor::focusOutEvent(QFocusEvent* event) {
    if (keyStepping_)
        finishInteraction(true);
    QWidget::focusOutEvent(event);
    update();
}

bool KGradientEditor::focusNextPrevChild(const bool next) {
    // Tab walks the stops before it leaves the control.
    if (!stops_.empty()) {
        if (!selected_) {
            setSelectedStop(next ? 0 : stops_.size() - 1);
            return true;
        }
        if (next && *selected_ + 1 < stops_.size()) {
            setSelectedStop(*selected_ + 1);
            return true;
        }
        if (!next && *selected_ > 0) {
            setSelectedStop(*selected_ - 1);
            return true;
        }
    }
    return QWidget::focusNextPrevChild(next);
}

bool KGradientEditor::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (hasFocus() &&
            (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
             key == Qt::Key_Down || key == Qt::Key_PageUp || key == Qt::Key_PageDown ||
             key == Qt::Key_Home || key == Qt::Key_End || key == Qt::Key_Delete ||
             key == Qt::Key_Backspace || (key == Qt::Key_Escape && (dragged_ || pulled_)))) {
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
            const bool inside = !stops_.empty() &&
                                (!selected_ || (next && *selected_ + 1 < stops_.size()) ||
                                 (!next && *selected_ > 0));
            if (inside) {
                focusNextPrevChild(next);
                return true;
            }
        }
        break;
    }
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        if (dragged_ || pulled_ || (event->type() == QEvent::WindowDeactivate && keyStepping_))
            finishInteraction(true);
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void KGradientEditor::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KGradientEditor::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KGradientEditor::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
        finishInteraction(true);
    }
    QWidget::changeEvent(event);
}

void KGradientEditor::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const State state = visualState();
    const auto bar = barRect();
    painter.setOpacity(state == State::Disabled ? kDisabledOpacity : 1.0);
    // Partial alpha reads against the checkerboard, as on a colour chip.
    drawAlphaCheckerboard(painter, bar, kCheckerCell, Radius::Small);
    const auto radius = static_cast<qreal>(Radius::Small);
    painter.setPen(Qt::NoPen);
    if (sampler_) {
        // #546: one column per device-independent pixel, each the colour at its centre.
        QPainterPath shape;
        shape.addRoundedRect(bar, radius, radius);
        painter.save();
        painter.setClipPath(shape);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const int columns = std::max(1, static_cast<int>(std::ceil(bar.width())));
        for (int column = 0; column < columns; ++column) {
            const double t = columns == 1 ? 0.0 : (column + 0.5) / static_cast<double>(columns);
            painter.fillRect(QRectF(bar.left() + column, bar.top(), 1.0, bar.height()),
                             displayColorAt(t));
        }
        painter.restore();
    } else {
        QLinearGradient gradient(bar.topLeft(), bar.topRight());
        for (int sample = 0; sample <= kGradientSamples; ++sample) {
            const double t = static_cast<double>(sample) / kGradientSamples;
            gradient.setColorAt(t, displayColorAt(t));
        }
        for (const auto& stop : stops_)
            gradient.setColorAt(stop.position, displayed(stop.color));
        painter.setBrush(gradient);
        painter.drawRoundedRect(bar, radius, radius);
    }
    painter.setBrush(Qt::NoBrush);
    applyHairlinePen(painter, color(borderForState(state)));
    painter.drawRoundedRect(bar, radius, radius);
    // Each stop: a pointer into the bar and a swatch under it in the stop's own colour.
    const qreal half = kHandleHalfWidth;
    for (std::size_t index = 0; index < stops_.size(); ++index) {
        const auto centre = stopCentre(index);
        const bool selected = selected_ == index;
        QPainterPath handle;
        handle.moveTo(centre.x(), bar.bottom());
        handle.lineTo(centre.x() + half, centre.y() - half / 2.0);
        handle.lineTo(centre.x() + half, centre.y() + half);
        handle.lineTo(centre.x() - half, centre.y() + half);
        handle.lineTo(centre.x() - half, centre.y() - half / 2.0);
        handle.closeSubpath();
        painter.setBrush(displayed(stops_[index].color));
        const auto& stop = stops_[index];
        if (selected) {
            QPen pen(color(dragged_ ? Color::AccentPressed : Color::Accent));
            pen.setWidthF(kEmphasisStroke);
            painter.setPen(pen);
        } else {
            applyHairlinePen(painter, color(Color::Foreground));
        }
        painter.drawPath(handle);
        if (stop.colorLinked || stop.positionLinked) {
            // #543: a linked stop: a dashed ring in its link's socket colour around the handle and
            // a dot of that colour where the handle meets the bar.
            const auto linkColor =
                color(stop.colorLinked ? Color::SocketColor : Color::SocketScalar);
            QPen dashed(linkColor);
            dashed.setWidthF(kEmphasisStroke);
            dashed.setDashPattern({2.0, 1.5});
            painter.setBrush(Qt::NoBrush);
            painter.setPen(dashed);
            const qreal ring = kEmphasisStroke + 1.0;
            painter.drawRoundedRect(handle.boundingRect().adjusted(-ring, -ring, ring, ring),
                                    static_cast<qreal>(Radius::Small),
                                    static_cast<qreal>(Radius::Small));
            painter.setPen(Qt::NoPen);
            painter.setBrush(linkColor);
            const qreal dot = px(Spacing::XXS);
            painter.drawEllipse(QPointF(centre.x(), bar.bottom() - dot), dot, dot);
        }
        if (selected && hasKeyboardFocus(*this) && state != State::Disabled)
            drawFocusRing(painter, handle.boundingRect(), Radius::Small);
    }
    painter.setOpacity(1.0);
}

} // namespace bloom::ui::kit

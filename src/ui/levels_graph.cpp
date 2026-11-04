#include <bloom/ui/levels_graph.hpp>

#include "properties_value_edits.hpp"
#include "value_presentation.hpp"

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace bloom::ui {
namespace {

constexpr std::array kHandles{LevelsHandle::InputBlack, LevelsHandle::Gamma,
                              LevelsHandle::InputWhite, LevelsHandle::OutputBlack,
                              LevelsHandle::OutputWhite};
// The plot's height over its width.
constexpr double kPlotAspect = 0.45;
// The share of the range a view keeps beyond a value outside 0..1.
constexpr double kHeadroom = 0.1;

[[nodiscard]] qreal handleExtent() { return kit::px(kit::Size::IconSmall); }
[[nodiscard]] qreal barExtent() { return kit::px(kit::Spacing::S); }
[[nodiscard]] qreal small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] int control() { return kit::px(kit::Size::Control); }

[[nodiscard]] int canvasHeight(const int width) {
    return static_cast<int>(std::round(width * kPlotAspect)) +
           static_cast<int>(2.0 * handleExtent() + small() + barExtent());
}

[[nodiscard]] bool isOutput(const LevelsHandle handle) noexcept {
    return handle == LevelsHandle::OutputBlack || handle == LevelsHandle::OutputWhite;
}

[[nodiscard]] QColor channelInk(const std::size_t channel) {
    switch (channel) {
    case 1:
        return kit::color(kit::Color::ComponentX);
    case 2:
        return kit::color(kit::Color::ComponentY);
    case 3:
        return kit::color(kit::Color::ComponentZ);
    default:
        return kit::color(kit::Color::Foreground);
    }
}

[[nodiscard]] QString channelName(const std::size_t channel) {
    switch (channel) {
    case 1:
        return QObject::tr("Red");
    case 2:
        return QObject::tr("Green");
    case 3:
        return QObject::tr("Blue");
    case 4:
        return QObject::tr("Alpha");
    default:
        return QObject::tr("RGB");
    }
}

[[nodiscard]] std::vector<document::ParameterId> channelParameters(const CompositionSession& session,
                                                                   const document::NodeId node,
                                                                   const std::size_t channel) {
    std::vector<document::ParameterId> ids;
    for (std::size_t field = 0; field < document::kLevelsFieldCount; ++field)
        if (const auto id = nodeRoleParameter(session, node, document::levelsRole(channel, field)))
            ids.push_back(*id);
    return ids;
}

} // namespace

QString levelsHandleName(const LevelsHandle handle) {
    switch (handle) {
    case LevelsHandle::InputBlack:
        return QObject::tr("Input Black");
    case LevelsHandle::InputWhite:
        return QObject::tr("Input White");
    case LevelsHandle::Gamma:
        return QObject::tr("Gamma");
    case LevelsHandle::OutputBlack:
        return QObject::tr("Output Black");
    case LevelsHandle::OutputWhite:
        break;
    }
    return QObject::tr("Output White");
}

double levelsGammaHandle(const double black, const double white, const double gamma) noexcept {
    return black + (white - black) * std::pow(0.5, gamma);
}

double levelsGammaAt(const double black, const double white, const double x) noexcept {
    if (!(white != black) || !std::isfinite(x))
        return 1.0;
    const double t = std::clamp((x - black) / (white - black), 1.0e-6, 1.0 - 1.0e-6);
    return std::clamp(std::log(t) / std::log(0.5), document::kMinimumLevelsGamma,
                      document::kMaximumLevelsGamma);
}

// --- LevelsGraphCanvas --------------------------------------------------------------------------

LevelsGraphCanvas::LevelsGraphCanvas(CompositionSession& session, const document::NodeId node,
                                     QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("levelsGraph"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(tr("Levels graph"));
    setToolTip(tr("Levels: %1").arg(channelName(channel_)));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, qOverload<>(&QWidget::update));
    if (auto* previews = session_.imagePreviews())
        connect(previews, &ImagePreviewController::changed, this, qOverload<>(&QWidget::update));
}

void LevelsGraphCanvas::setChannel(const std::size_t channel) {
    if (channel >= document::kGradeChannelCount || channel == channel_)
        return;
    cancelDrag();
    channel_ = channel;
    setToolTip(tr("Levels: %1").arg(channelName(channel_)));
    update();
}

std::optional<document::ParameterId> LevelsGraphCanvas::parameter(const LevelsHandle handle) const {
    return nodeRoleParameter(session_, node_,
                             document::levelsRole(channel_, static_cast<std::size_t>(handle)));
}

std::optional<std::array<double, document::kLevelsFieldCount>> LevelsGraphCanvas::values() const {
    std::array<double, document::kLevelsFieldCount> values{};
    for (std::size_t field = 0; field < values.size(); ++field) {
        const auto id = parameter(static_cast<LevelsHandle>(field));
        const auto value = id ? liveScalar(session_, *id) : std::nullopt;
        if (!value)
            return std::nullopt;
        values[field] = *value;
    }
    return values;
}

QRectF LevelsGraphCanvas::plotRect() const {
    const qreal inset = handleExtent() / 2.0;
    const qreal bottom = height() - (2.0 * handleExtent() + small() + barExtent());
    return {inset, 0.0, std::max<qreal>(1.0, width() - inset - inset),
            std::max<qreal>(1.0, bottom)};
}

QRectF LevelsGraphCanvas::outputRect() const {
    const auto plot = plotRect();
    return {plot.left(), plot.bottom() + handleExtent() + small(), plot.width(), barExtent()};
}

LevelsGraphCanvas::Range LevelsGraphCanvas::inputRange() const {
    if (drag_)
        return drag_->input;
    Range range;
    if (const auto current = values()) {
        range.low = std::min({0.0, (*current)[0], (*current)[1]});
        range.high = std::max({1.0, (*current)[0], (*current)[1]});
    }
    const double span = range.high - range.low;
    if (range.low < 0.0)
        range.low -= kHeadroom * span;
    if (range.high > 1.0)
        range.high += kHeadroom * span;
    return range;
}

LevelsGraphCanvas::Range LevelsGraphCanvas::outputRange() const {
    if (drag_)
        return drag_->output;
    Range range;
    if (const auto current = values()) {
        range.low = std::min({0.0, (*current)[3], (*current)[4]});
        range.high = std::max({1.0, (*current)[3], (*current)[4]});
    }
    const double span = range.high - range.low;
    if (range.low < 0.0)
        range.low -= kHeadroom * span;
    if (range.high > 1.0)
        range.high += kHeadroom * span;
    return range;
}

std::optional<QPointF> LevelsGraphCanvas::handlePosition(const LevelsHandle handle) const {
    return handlePosition(handle, inputRange(), outputRange());
}

std::optional<QPointF> LevelsGraphCanvas::handlePosition(const LevelsHandle handle,
                                                         const Range& input,
                                                         const Range& output) const {
    const auto current = values();
    if (!current)
        return std::nullopt;
    const auto& v = *current;
    const auto plot = plotRect();
    const auto at = [&](const Range& range, const double value) {
        return plot.left() + plot.width() * (value - range.low) / (range.high - range.low);
    };
    const qreal inputY = plot.bottom() + handleExtent() / 2.0;
    const qreal outputY = outputRect().bottom() + handleExtent() / 2.0;
    switch (handle) {
    case LevelsHandle::InputBlack:
        return QPointF(at(input, v[0]), inputY);
    case LevelsHandle::InputWhite:
        return QPointF(at(input, v[1]), inputY);
    case LevelsHandle::Gamma:
        return QPointF(at(input, levelsGammaHandle(v[0], v[1], v[2])), inputY);
    case LevelsHandle::OutputBlack:
        return QPointF(at(output, v[3]), outputY);
    case LevelsHandle::OutputWhite:
        return QPointF(at(output, v[4]), outputY);
    }
    return std::nullopt;
}

std::optional<LevelsHandle> LevelsGraphCanvas::handleAt(const QPointF position) const {
    std::optional<LevelsHandle> nearest;
    qreal best = handleExtent() * handleExtent();
    for (const auto handle : kHandles) {
        const auto centre = handlePosition(handle);
        if (!centre)
            continue;
        const auto delta = *centre - position;
        const qreal distance = QPointF::dotProduct(delta, delta);
        if (distance <= best) {
            best = distance;
            nearest = handle;
        }
    }
    return nearest;
}

std::optional<LevelsHandle> LevelsGraphCanvas::nearestInRow(const QPointF position) const {
    const auto plot = plotRect();
    if (position.y() < plot.bottom())
        return std::nullopt;
    const bool output = position.y() >= outputRect().top() - small() / 2.0;
    std::optional<LevelsHandle> nearest;
    qreal best = std::numeric_limits<qreal>::max();
    for (const auto handle : kHandles) {
        if (isOutput(handle) != output)
            continue;
        const auto centre = handlePosition(handle);
        if (!centre)
            continue;
        const qreal distance = std::abs(centre->x() - position.x());
        if (distance < best) {
            best = distance;
            nearest = handle;
        }
    }
    return nearest;
}

double LevelsGraphCanvas::handleValueAt(const LevelsHandle handle, const double x) const {
    const auto plot = plotRect();
    const auto range = isOutput(handle) ? outputRange() : inputRange();
    const double u = std::clamp((x - plot.left()) / plot.width(), 0.0, 1.0);
    return range.low + u * (range.high - range.low);
}

void LevelsGraphCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto plot = plotRect();
    const auto input = inputRange();
    const auto output = outputRange();
    const auto x = [&](const double value) {
        return plot.left() + plot.width() * (value - input.low) / (input.high - input.low);
    };
    const auto y = [&](const double value) {
        return plot.bottom() - plot.height() * (value - output.low) / (output.high - output.low);
    };
    // #501: one uniform grey under the whole plot, as every grading graph, HDR range included.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::Field));
    painter.drawRect(plot);
    // The input histogram, requested only here, while the graph is drawn.
    if (auto* previews = session_.imagePreviews(); previews && previews->histogramsShown()) {
        const auto bins = previews->histogram(node_).bins;
        QColor ink = kit::color(kit::Color::Muted);
        ink.setAlphaF(0.45F);
        painter.setBrush(ink);
        const auto count = static_cast<qreal>(bins.size());
        for (std::size_t bin = 0; bin < bins.size(); ++bin) {
            const qreal x0 = x(static_cast<qreal>(bin) / count);
            const qreal x1 = x(static_cast<qreal>(bin + 1) / count);
            const qreal tall = plot.height() * 0.9 * static_cast<qreal>(bins[bin]);
            painter.drawRect(QRectF(x0, plot.bottom() - tall, x1 - x0, tall));
        }
    }
    // The grid: a line every quarter across the whole plot, the same past 1 as below it.
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    for (double t = std::ceil(input.low * 4.0) / 4.0; t < input.high; t += 0.25)
        if (t > input.low)
            painter.drawLine(QPointF(x(t), plot.top()), QPointF(x(t), plot.bottom()));
    for (double t = std::ceil(output.low * 4.0) / 4.0; t < output.high; t += 0.25)
        if (t > output.low)
            painter.drawLine(QPointF(plot.left(), y(t)), QPointF(plot.right(), y(t)));
    const auto current = values();
    if (current) {
        // The channel's transfer, input along the plot, output up it over the output range.
        const auto& v = *current;
        QPainterPath path;
        const int steps = std::max(16, static_cast<int>(plot.width() / 2.0));
        for (int step = 0; step <= steps; ++step) {
            const double value =
                input.low + (input.high - input.low) * static_cast<double>(step) / steps;
            double t = v[1] != v[0] ? (value - v[0]) / (v[1] - v[0]) : (value >= v[0] ? 1.0 : 0.0);
            if (v[2] != 1.0 && t > 0.0)
                t = std::pow(t, 1.0 / v[2]);
            const double out = v[3] + t * (v[4] - v[3]);
            const double u = (out - output.low) / (output.high - output.low);
            const QPointF point(x(value), plot.bottom() - std::clamp(u, -0.1, 1.1) * plot.height());
            if (step == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        painter.save();
        painter.setClipRect(plot);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(channelInk(channel_), kit::kChartLineStroke));
        painter.drawPath(path);
        painter.restore();
    }
    // 0 and 1 on the plot's foot, where the input reaches them.
    painter.setFont(kit::font(kit::TypeRole::UiSmall));
    painter.setPen(kit::color(kit::Color::Muted));
    const QFontMetricsF metrics(painter.font());
    const qreal inset = kit::px(kit::Spacing::XXS);
    const qreal baseline = plot.bottom() - inset - metrics.descent();
    painter.drawText(QPointF(x(0.0) + inset, baseline), QStringLiteral("0"));
    painter.drawText(QPointF(x(1.0) - inset - metrics.horizontalAdvance(QStringLiteral("1")),
                             baseline),
                     QStringLiteral("1"));
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);
    // The output bar, dark to the channel's ink over its range.
    const auto bar = outputRect();
    QLinearGradient ramp(bar.topLeft(), bar.topRight());
    const auto stopAt = [&](const double value) {
        return std::clamp((value - output.low) / (output.high - output.low), 0.0, 1.0);
    };
    ramp.setColorAt(0.0, kit::color(kit::Color::Background));
    ramp.setColorAt(stopAt(0.0), kit::color(kit::Color::Background));
    ramp.setColorAt(stopAt(1.0), channelInk(channel_));
    ramp.setColorAt(1.0, channelInk(channel_));
    painter.setPen(Qt::NoPen);
    painter.setBrush(ramp);
    painter.drawRect(bar);
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bar);
    // The handles: triangles pointing up at their value.
    const qreal half = handleExtent() / 2.0;
    for (const auto handle : kHandles) {
        const auto centre = handlePosition(handle);
        if (!centre)
            continue;
        const qreal tip = centre->y() - half;
        QPainterPath triangle;
        triangle.moveTo(centre->x(), tip);
        triangle.lineTo(centre->x() + half, tip + handleExtent());
        triangle.lineTo(centre->x() - half, tip + handleExtent());
        triangle.closeSubpath();
        const bool active = drag_ && drag_->handle == handle;
        const bool hovered = !drag_ && hovered_ == handle;
        const auto fill =
            handle == LevelsHandle::Gamma ? kit::Color::Muted
            : handle == LevelsHandle::InputBlack || handle == LevelsHandle::OutputBlack
                ? kit::Color::Background
                : kit::Color::Foreground;
        painter.setBrush(kit::color(fill));
        painter.setPen(QPen(kit::color(active    ? kit::Color::Accent
                                       : hovered ? kit::Color::Foreground
                                                 : kit::Color::BorderHover),
                            active || hovered ? kit::kEmphasisStroke : kit::kHairlineWidth));
        painter.drawPath(triangle);
    }
    if (hasFocus()) {
        kit::applyHairlinePen(painter, kit::color(kit::Color::Accent));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plot);
    }
}

void LevelsGraphCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    event->accept();
    if (event->modifiers().testFlag(Qt::AltModifier)) {
        emit alternateClicked();
        return;
    }
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    const bool jump = modifiers.testFlag(Qt::ShiftModifier);
    auto handle = handleAt(event->position());
    // A Shift-press on a handle's track jumps the nearest handle of that row to the pointer.
    if (jump)
        if (const auto nearest = nearestInRow(event->position()))
            handle = nearest;
    const auto id = handle ? parameter(*handle) : std::nullopt;
    const auto current = values();
    if (!handle || !id || !current)
        return;
    const auto input = inputRange();
    const auto output = outputRange();
    if (!session_.beginValueEdit(*id))
        return;
    Drag drag;
    drag.handle = *handle;
    drag.parameter = *id;
    drag.input = input;
    drag.output = output;
    drag.base = *current;
    // Gamma's handle stands at its position, not at its value.
    if (*handle == LevelsHandle::Gamma)
        drag.origin = levelsGammaHandle((*current)[0], (*current)[1], (*current)[2]);
    else
        drag.origin = (*current)[static_cast<std::size_t>(*handle)];
    drag.fromX = event->position().x();
    drag.modifiers = modifiers;
    drag.scale = kit::KSlider::dragScale(modifiers);
    drag_ = drag;
    if (jump) {
        drag_->scale = 1.0;
        drag_->origin = handleValueAt(*handle, event->position().x());
        const double next =
            *handle == LevelsHandle::Gamma
                ? levelsGammaAt(drag_->base[0], drag_->base[1], drag_->origin)
                : std::clamp(drag_->origin, -document::kMaximumGradeValue,
                             document::kMaximumGradeValue);
        if (!session_.updateValueEdit(next))
            cancelDrag();
    }
    drag_->at = drag_->origin;
    update();
}

void LevelsGraphCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_) {
        const auto over = handleAt(event->position());
        if (over != hovered_) {
            hovered_ = over;
            setCursor(over ? Qt::SizeHorCursor : Qt::ArrowCursor);
            update();
        }
        return;
    }
    if (!session_.isValueEditing(drag_->parameter)) {
        drag_.reset();
        update();
        return;
    }
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers != drag_->modifiers) {
        // A modifier change continues from where the handle is, so it never jumps.
        drag_->origin = drag_->at;
        drag_->fromX = event->position().x();
        drag_->modifiers = modifiers;
        drag_->scale = kit::KSlider::dragScale(modifiers);
    }
    const auto plot = plotRect();
    const auto& range = isOutput(drag_->handle) ? drag_->output : drag_->input;
    // Relative: the handle moves by the pointer's travel, from where it was pressed.
    const double travel = (event->position().x() - drag_->fromX) * drag_->scale *
                          (range.high - range.low) / std::max<qreal>(1.0, plot.width());
    drag_->at = std::clamp(drag_->origin + travel, range.low, range.high);
    const double next =
        drag_->handle == LevelsHandle::Gamma
            ? levelsGammaAt(drag_->base[0], drag_->base[1], drag_->at)
            : std::clamp(drag_->at, -document::kMaximumGradeValue, document::kMaximumGradeValue);
    if (!session_.updateValueEdit(next))
        cancelDrag();
    event->accept();
}

void LevelsGraphCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool owned = session_.isValueEditing(drag_->parameter);
    drag_.reset();
    // One command for the gesture; a press that moved nothing commits nothing.
    if (owned)
        static_cast<void>(session_.commitValueEdit());
    update();
    event->accept();
}

void LevelsGraphCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    // A double-click on a handle resets it; on the plot, the channel.
    if (const auto handle = handleAt(event->position()))
        static_cast<void>(resetHandle(*handle));
    else if (plotRect().contains(event->position()))
        static_cast<void>(resetChannel());
    event->accept();
}

QMenu* LevelsGraphCanvas::makeContextMenu(const QPointF position) {
    auto* menu = kit::makeMenu(this);
    const auto current = values();
    if (const auto handle = handleAt(position)) {
        const auto index = static_cast<std::size_t>(*handle);
        auto* reset = menu->addAction(tr("Reset %1").arg(levelsHandleName(*handle)));
        reset->setObjectName(QStringLiteral("levelsGraphResetHandle"));
        reset->setEnabled(isEnabled() && current &&
                          (*current)[index] != document::kLevelsFieldDefaults[index]);
        connect(reset, &QAction::triggered, this,
                [this, handle = *handle] { static_cast<void>(resetHandle(handle)); });
    }
    auto* channel = menu->addAction(tr("Reset %1").arg(channelName(channel_)));
    channel->setObjectName(QStringLiteral("levelsGraphResetChannel"));
    bool changed = false;
    if (current)
        for (std::size_t field = 0; field < document::kLevelsFieldCount; ++field)
            changed = changed || (*current)[field] != document::kLevelsFieldDefaults[field];
    channel->setEnabled(isEnabled() && changed);
    connect(channel, &QAction::triggered, this, [this] { static_cast<void>(resetChannel()); });
    const auto ids = channelParameters(session_, node_, channel_);
    std::size_t keyable = 0;
    std::size_t keyed = 0;
    for (const auto id : ids) {
        const auto state = session_.keyframeDiamondStateForParameter(id);
        keyable += state != KeyframeDiamondState::Unsupported ? 1U : 0U;
        keyed += state == KeyframeDiamondState::AnimatedWithKey ? 1U : 0U;
    }
    if (keyable != 0) {
        menu->addSeparator();
        auto* key = menu->addAction(keyed == keyable
                                        ? tr("Remove %1 Keys").arg(channelName(channel_))
                                        : tr("Key %1").arg(channelName(channel_)));
        key->setObjectName(QStringLiteral("levelsGraphKey"));
        connect(key, &QAction::triggered, this, &LevelsGraphCanvas::keyRequested);
    }
    auto* show = menu->addAction(tr("Show Values"));
    show->setObjectName(QStringLiteral("levelsGraphShowValues"));
    connect(show, &QAction::triggered, this, &LevelsGraphCanvas::alternateClicked);
    return menu;
}

void LevelsGraphCanvas::contextMenuEvent(QContextMenuEvent* event) {
    if (drag_) {
        event->accept();
        return;
    }
    const std::unique_ptr<QMenu> menu(makeContextMenu(event->pos()));
    menu->exec(event->globalPos());
    event->accept();
}

void LevelsGraphCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void LevelsGraphCanvas::focusOutEvent(QFocusEvent* event) {
    cancelDrag();
    QWidget::focusOutEvent(event);
    update();
}

void LevelsGraphCanvas::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void LevelsGraphCanvas::leaveEvent(QEvent* event) {
    if (hovered_) {
        hovered_.reset();
        update();
    }
    QWidget::leaveEvent(event);
}

void LevelsGraphCanvas::cancelDrag() {
    if (!drag_)
        return;
    const auto id = drag_->parameter;
    drag_.reset();
    if (session_.isValueEditing(id))
        session_.cancelValueEdit();
    update();
}

bool LevelsGraphCanvas::resetHandle(const LevelsHandle handle) {
    const auto id = parameter(handle);
    const auto current = values();
    if (!id || !current)
        return false;
    const double neutral = document::kLevelsFieldDefaults[static_cast<std::size_t>(handle)];
    if ((*current)[static_cast<std::size_t>(handle)] == neutral)
        return true;
    return session_.setParameterValue(*id, neutral, tr("Reset %1").arg(levelsHandleName(handle)));
}

bool LevelsGraphCanvas::resetChannel() {
    const auto current = values();
    if (!current)
        return false;
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> changes;
    for (std::size_t field = 0; field < document::kLevelsFieldCount; ++field)
        if (const auto id = parameter(static_cast<LevelsHandle>(field));
            id && (*current)[field] != document::kLevelsFieldDefaults[field])
            changes.emplace_back(*id, document::kLevelsFieldDefaults[field]);
    if (changes.empty())
        return true;
    return session_.setParameterValues(std::move(changes),
                                       tr("Reset Levels %1").arg(channelName(channel_)));
}

// --- LevelsCardWidget ---------------------------------------------------------------------------

LevelsCardWidget::LevelsCardWidget(CompositionSession& session, const document::NodeId node,
                                   const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeLevelsGraph")
                       : QStringLiteral("propertiesLevelsGraph"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    tabs_ = new kit::KTabBar(this);
    tabs_->setObjectName(QStringLiteral("levelsGraphTabs"));
    for (const auto& text : {tr("RGB"), tr("R"), tr("G"), tr("B"), tr("A")})
        tabs_->addTab(text);
    for (int index = 0; index < tabs_->count(); ++index)
        tabs_->setTabToolTip(index, channelName(static_cast<std::size_t>(index)));
    tabs_->setCurrentIndex(0);
    diamond_ = new KeyframeGroupDiamond(session_, tr("Levels"), this);
    diamond_->setObjectName(QStringLiteral("levelsGraphDiamond"));
    // The Values disclosure: its caret turns down while the channel's rows are shown.
    auto* disclosure = new kit::KButton(kit::IconId::CaretRight, tr("Values"), this);
    disclosure->setVariant(kit::KButton::Variant::Ghost);
    disclosure->setControlSize(kit::KButton::ControlSize::Compact);
    disclosure->setCheckable(true);
    disclosure->setObjectName(QStringLiteral("levelsGraphValues"));
    disclosure->setAccessibleName(tr("Levels values"));
    disclosure->setToolTip(tr("Show this channel's rows, with their sockets, to link them"));
    values_ = disclosure;
    histogram_ = new kit::KCheckBox(tr("Histogram"), this);
    histogram_->setObjectName(QStringLiteral("levelsGraphHistogram"));
    histogram_->setCheckable(true);
    histogram_->setAccessibleName(tr("Histogram"));
    histogram_->setToolTip(tr("The input's luminance histogram behind the levels"));
    reset_ = new kit::KButton(kit::IconId::Reset, QString{}, this);
    reset_->setObjectName(QStringLiteral("levelsGraphReset"));
    reset_->setControlSize(kit::KButton::ControlSize::Compact);
    reset_->setAccessibleName(tr("Reset channel"));
    reset_->setToolTip(tr("Reset this channel's five values"));
    canvas_ = new LevelsGraphCanvas(session_, node_, this);
    inputLabel_ = new kit::KLabel(tr("Input"), this, kit::TypeRole::UiSmall);
    inputLabel_->setObjectName(QStringLiteral("levelsGraphInputLabel"));
    inputLabel_->setTone(kit::Color::Muted);
    outputLabel_ = new kit::KLabel(tr("Output"), this, kit::TypeRole::UiSmall);
    outputLabel_->setObjectName(QStringLiteral("levelsGraphOutputLabel"));
    outputLabel_->setTone(kit::Color::Muted);
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        const auto handle = static_cast<LevelsHandle>(index);
        auto* field = new kit::KValueField(this);
        field->setObjectName(QStringLiteral("levelsGraphField.%1").arg(index));
        field->setCompact(true);
        field->setOutlinedAtRest(true);
        field->setMaximumWidth(QWIDGETSIZE_MAX);
        configureValueField(*field, document::levelsSchemaKey(0, index),
                            document::ParameterValueKind::Float64,
                            document::levelsRole(0, index));
        field->setDefaultValue(document::kLevelsFieldDefaults[index]);
        field->setAccessibleName(levelsHandleName(handle));
        field->setToolTip(levelsHandleName(handle));
        field->setResetText(tr("Reset %1").arg(levelsHandleName(handle)));
        auto* key = new QAction(tr("Add Key"), field);
        key->setObjectName(QStringLiteral("levelsGraphFieldKey"));
        connect(key, &QAction::triggered, this, [this, handle] {
            if (const auto id = canvas_->parameter(handle))
                static_cast<void>(session_.toggleKeyframeForParameter(*id));
        });
        field->addAction(key);
        properties::bindValueEdit(session_, *field, [this, handle] {
            return canvas_->parameter(handle).value_or(document::ParameterId{});
        });
        connect(field, &kit::KValueField::valueChanged, this, [this, handle](const double value) {
            if (const auto id = canvas_->parameter(handle))
                static_cast<void>(session_.setParameterValue(
                    *id, value, tr("Set %1").arg(levelsHandleName(handle))));
        });
        fields_[index] = field;
    }
    connect(tabs_, &kit::KTabBar::currentChanged, this, [this](const int index) {
        if (index < 0)
            return;
        canvas_->setChannel(static_cast<std::size_t>(index));
        refresh();
        if (expanded_)
            emit rowsChanged();
    });
    connect(values_, &QAbstractButton::clicked, this, [this] { setValuesExpanded(!expanded_); });
    connect(canvas_, &LevelsGraphCanvas::alternateClicked, this,
            [this] { setValuesExpanded(!expanded_); });
    connect(canvas_, &LevelsGraphCanvas::keyRequested, diamond_,
            [this] { static_cast<void>(diamond_->toggle()); });
    connect(reset_, &QAbstractButton::clicked, canvas_,
            [this] { static_cast<void>(canvas_->resetChannel()); });
    connect(histogram_, &kit::KSwitch::toggled, this, [this](const bool on) {
        if (auto* previews = session_.imagePreviews())
            previews->setHistogramsShown(on);
    });
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &LevelsCardWidget::refresh);
    refresh();
}

kit::KValueField* LevelsCardWidget::field(const LevelsHandle handle) const {
    return fields_[static_cast<std::size_t>(handle)];
}

bool LevelsCardWidget::showsRole(const std::string_view role) const {
    if (role == "clamp")
        return true;
    if (!expanded_)
        return false;
    for (std::size_t field = 0; field < document::kLevelsFieldCount; ++field)
        if (role == document::levelsRole(canvas_->channel(), field))
            return true;
    return false;
}

void LevelsCardWidget::setValuesExpanded(const bool expanded) {
    {
        const QSignalBlocker blocker(values_);
        values_->setChecked(expanded);
    }
    values_->setIconId(expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;
    emit rowsChanged();
}

int LevelsCardWidget::heightAt(const int width) const {
    // Tabs, the histogram switch, the graph, then the input and output values.
    return 2 * (control() + static_cast<int>(small())) + canvasHeight(width) +
           static_cast<int>(small()) + 2 * control() + static_cast<int>(small());
}

int LevelsCardWidget::minimumWidthHint() const {
    // #499: every channel tab fits beside the diamond and the Values button, so none is clipped;
    // three values fit beside their caption.
    // The gaps either side of the diamond.
    const int gaps = 2 * static_cast<int>(small());
    const int header = tabs_->sizeHint().width() + gaps + kit::px(kit::Size::IconSmall) +
                       values_->sizeHint().width();
    const int label =
        std::max(inputLabel_->sizeHint().width(), outputLabel_->sizeHint().width());
    const int values =
        label + static_cast<int>(small()) +
        3 * (kit::px(kit::Size::PropertiesFieldWidth) + static_cast<int>(small()));
    return std::max({kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                     header, values});
}

QSize LevelsCardWidget::sizeHint() const {
    const int width = std::max(minimumWidthHint(), kit::px(kit::Size::DropdownWidthExpanded));
    return {width, heightAt(width)};
}

void LevelsCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void LevelsCardWidget::place() {
    const int w = width();
    const int s = static_cast<int>(small());
    // The tabs, then the channel's diamond and its Values button at the right of their row.
    const int values = values_->sizeHint().width();
    values_->setGeometry(std::max(0, w - values), 0, values, control());
    const int diamondX = std::max(0, w - values - s - diamond_->width());
    diamond_->move(diamondX, (control() - diamond_->height()) / 2);
    tabs_->setGeometry(0, 0, std::max(0, diamondX - s), control());
    int y = control() + s;
    histogram_->setGeometry(0, y, std::min(w - control() - s, histogram_->sizeHint().width()),
                            control());
    reset_->setGeometry(std::max(0, w - control()), y, control(), control());
    y += control() + s;
    canvas_->setGeometry(0, y, w, canvasHeight(w));
    y += canvasHeight(w) + s;
    // The values under the graph: black at the left, gamma in the middle, white at the right.
    const int label = std::max(inputLabel_->sizeHint().width(), outputLabel_->sizeHint().width());
    const int left = label + s;
    const int field = std::max(0, (w - left - 2 * s) / 3);
    const auto column = [&](const int index) { return left + index * (field + s); };
    inputLabel_->setGeometry(0, y, label, control());
    fields_[0]->setGeometry(column(0), y, field, control());
    fields_[2]->setGeometry(column(1), y, field, control());
    fields_[1]->setGeometry(column(2), y, w - column(2), control());
    y += control() + s;
    outputLabel_->setGeometry(0, y, label, control());
    fields_[3]->setGeometry(column(0), y, field, control());
    fields_[4]->setGeometry(column(2), y, w - column(2), control());
}

void LevelsCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    canvas_->setEnabled(!locked);
    reset_->setEnabled(!locked);
    if (auto* previews = session_.imagePreviews()) {
        const QSignalBlocker blocker(histogram_);
        histogram_->setChecked(previews->histogramsShown());
    }
    const auto channel = canvas_->channel();
    diamond_->setParameters(channelParameters(session_, node_, channel));
    diamond_->refresh();
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        auto* field = fields_[index];
        const auto id = canvas_->parameter(static_cast<LevelsHandle>(index));
        field->setEnabled(id && !locked && scalarEditable(session_, *id));
        if (id && !field->hasLiveEdit())
            if (const auto value = liveScalar(session_, *id)) {
                const QSignalBlocker blocker(field);
                field->setValue(*value);
            }
        const auto state =
            id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
        for (auto* action : field->actions()) {
            action->setVisible(state != KeyframeDiamondState::Unsupported);
            action->setText(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                            : tr("Add Key"));
        }
    }
    // A dot on each channel's tab that is not at its defaults.
    for (std::size_t tab = 0; tab < document::kGradeChannelCount; ++tab) {
        bool modified = false;
        for (std::size_t index = 0; index < document::kLevelsFieldCount; ++index)
            if (const auto id = nodeRoleParameter(session_, node_, document::levelsRole(tab, index)))
                if (const auto value = liveScalar(session_, *id))
                    modified = modified || *value != document::kLevelsFieldDefaults[index];
        tabs_->setTabModified(static_cast<int>(tab), modified);
    }
    canvas_->update();
}

} // namespace bloom::ui

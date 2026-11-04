#include <bloom/ui/levels_graph.hpp>

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

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

[[nodiscard]] QString handleName(const LevelsHandle handle) {
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

} // namespace

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
    setToolTip(tr("Drag the black, gamma and white handles; double-click a handle to reset it; "
                  "Alt-click for the values"));
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
    update();
}

std::optional<document::ParameterId> LevelsGraphCanvas::parameter(const LevelsHandle handle) const {
    const auto* composition = session_.composition();
    const auto* record = composition ? composition->graph().findNode(node_) : nullptr;
    if (record == nullptr)
        return std::nullopt;
    const auto role = document::levelsRole(channel_, static_cast<std::size_t>(handle));
    const auto binding =
        std::ranges::find(record->parameters, role, &document::ParameterBinding::role);
    if (binding == record->parameters.end())
        return std::nullopt;
    return binding->parameterId;
}

std::optional<std::array<double, document::kLevelsFieldCount>> LevelsGraphCanvas::values() const {
    std::array<double, document::kLevelsFieldCount> values{};
    for (std::size_t field = 0; field < values.size(); ++field) {
        const auto id = parameter(static_cast<LevelsHandle>(field));
        const auto value = id ? session_.liveValue(*id) : std::nullopt;
        const auto* scalar = value ? std::get_if<double>(&*value) : nullptr;
        if (scalar == nullptr)
            return std::nullopt;
        values[field] = *scalar;
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

void LevelsGraphCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto plot = plotRect();
    const auto input = inputRange();
    const auto output = outputRange();
    const auto x = [&](const double value) {
        return plot.left() + plot.width() * (value - input.low) / (input.high - input.low);
    };
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::SurfaceSunken));
    painter.drawRect(plot);
    // The 0..1 span, lighter than the HDR range around it.
    painter.setBrush(kit::color(kit::Color::Field));
    painter.drawRect(QRectF(QPointF(x(0.0), plot.top()), QPointF(x(1.0), plot.bottom())));
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
        const auto fill =
            handle == LevelsHandle::Gamma ? kit::Color::Muted
            : handle == LevelsHandle::InputBlack || handle == LevelsHandle::OutputBlack
                ? kit::Color::Background
                : kit::Color::Foreground;
        painter.setBrush(kit::color(fill));
        painter.setPen(QPen(kit::color(active ? kit::Color::Accent : kit::Color::BorderHover),
                            active ? kit::kEmphasisStroke : kit::kHairlineWidth));
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
    const auto handle = handleAt(event->position());
    const auto id = handle ? parameter(*handle) : std::nullopt;
    const auto current = values();
    if (!handle || !id || !current)
        return;
    const auto input = inputRange();
    const auto output = outputRange();
    if (!session_.beginValueEdit(*id))
        return;
    drag_ = Drag{*handle, *id, input, output, *current};
    update();
}

void LevelsGraphCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_)
        return;
    if (!session_.isValueEditing(drag_->parameter)) {
        drag_.reset();
        update();
        return;
    }
    const auto plot = plotRect();
    const auto& range = isOutput(drag_->handle) ? drag_->output : drag_->input;
    const double u = std::clamp((event->position().x() - plot.left()) / plot.width(), 0.0, 1.0);
    const double value = range.low + u * (range.high - range.low);
    const double next =
        drag_->handle == LevelsHandle::Gamma
            ? levelsGammaAt(drag_->base[0], drag_->base[1], value)
            : std::clamp(value, -document::kMaximumGradeValue, document::kMaximumGradeValue);
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
    if (const auto handle = handleAt(event->position()))
        static_cast<void>(resetHandle(*handle));
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
    return session_.setParameterValue(*id, neutral, tr("Reset %1").arg(handleName(handle)));
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
    tabs_->setCurrentIndex(0);
    auto* disclosure = new kit::KButton(kit::IconId::CaretRight, QString{}, this);
    disclosure->setVariant(kit::KButton::Variant::Ghost);
    disclosure->setControlSize(kit::KButton::ControlSize::Compact);
    disclosure->setObjectName(QStringLiteral("levelsGraphValues"));
    disclosure->setAccessibleName(tr("Levels values"));
    disclosure->setToolTip(tr("Show this channel's values"));
    values_ = disclosure;
    canvas_ = new LevelsGraphCanvas(session_, node_, this);
    connect(tabs_, &kit::KTabBar::currentChanged, this, [this](const int index) {
        if (index < 0)
            return;
        canvas_->setChannel(static_cast<std::size_t>(index));
        if (expanded_)
            emit rowsChanged();
    });
    connect(values_, &QAbstractButton::clicked, this, [this] { setValuesExpanded(!expanded_); });
    connect(canvas_, &LevelsGraphCanvas::alternateClicked, this,
            [this] { setValuesExpanded(!expanded_); });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        const auto* composition = session_.composition();
        canvas_->setEnabled(composition != nullptr && !composition->nodeLocked(node_));
    });
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
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;
    values_->setIconId(expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    emit rowsChanged();
}

int LevelsCardWidget::heightAt(const int width) const {
    return control() + static_cast<int>(small()) + canvasHeight(width);
}

int LevelsCardWidget::minimumWidthHint() const {
    return kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
}

QSize LevelsCardWidget::sizeHint() const {
    const int width = kit::px(kit::Size::DropdownWidthExpanded);
    return {width, heightAt(width)};
}

void LevelsCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    const int w = width();
    const int tabsWidth = std::max(0, w - control() - static_cast<int>(small()));
    tabs_->setGeometry(0, 0, tabsWidth, control());
    values_->setGeometry(std::max(0, w - control()), 0, control(), control());
    const int top = control() + static_cast<int>(small());
    canvas_->setGeometry(0, top, w, canvasHeight(w));
}

} // namespace bloom::ui

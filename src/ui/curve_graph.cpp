#include <bloom/ui/curve_graph.hpp>

#include <bloom/document/grade_curves.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/grade_curve.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

using document::GradeCurve;
using document::Vec2d;

// A point is drawn as large as a node socket and can be picked a little outside it.
constexpr qreal kPointRadius = kit::px(kit::Size::NodeSocketDot) / 2.0;
constexpr qreal kHitRadius = kPointRadius + kit::px(kit::Spacing::XS);
// The share of the range a channel curve's view keeps above its highest point (and above 1).
constexpr double kHeadroom = 0.25;

[[nodiscard]] QString tabText(const GradeCurve curve) {
    switch (curve) {
    case GradeCurve::Master:
        return QObject::tr("RGB");
    case GradeCurve::Red:
        return QObject::tr("R");
    case GradeCurve::Green:
        return QObject::tr("G");
    case GradeCurve::Blue:
        return QObject::tr("B");
    case GradeCurve::Alpha:
        return QObject::tr("A");
    case GradeCurve::HueVsHue:
        return QObject::tr("Hue/Hue");
    case GradeCurve::HueVsSaturation:
        return QObject::tr("Hue/Sat");
    case GradeCurve::HueVsLuma:
        return QObject::tr("Hue/Luma");
    case GradeCurve::LumaVsSaturation:
        return QObject::tr("Luma/Sat");
    }
    return {};
}

[[nodiscard]] QColor curveInk(const GradeCurve curve) {
    switch (curve) {
    case GradeCurve::Red:
        return kit::color(kit::Color::ComponentX);
    case GradeCurve::Green:
        return kit::color(kit::Color::ComponentY);
    case GradeCurve::Blue:
        return kit::color(kit::Color::ComponentZ);
    case GradeCurve::Alpha:
        return kit::color(kit::Color::Muted);
    default:
        return kit::color(kit::Color::Foreground);
    }
}

[[nodiscard]] bool showsHistogram(const GradeCurve curve) noexcept {
    return document::isChannelGradeCurve(curve) || curve == GradeCurve::LumaVsSaturation;
}

[[nodiscard]] std::optional<document::ParameterId>
curveParameter(const CompositionSession& session, const document::NodeId node,
               const GradeCurve curve) {
    const auto* composition = session.composition();
    const auto* record = composition ? composition->graph().findNode(node) : nullptr;
    if (record == nullptr)
        return std::nullopt;
    const auto binding = std::ranges::find(record->parameters, document::gradeCurveRole(curve),
                                           &document::ParameterBinding::role);
    if (binding == record->parameters.end())
        return std::nullopt;
    return binding->parameterId;
}

void paintCurve(QPainter& painter, const QRectF& rect, const CurveGraphRange& range,
                const GradeCurve curve, const document::PathValue& value, const QColor& ink,
                const qreal width) {
    const auto shape = runtime::prepareGradeCurveShape(curve, value);
    if (!shape)
        return;
    QPainterPath path;
    const int steps = std::max(16, static_cast<int>(rect.width() / 2.0));
    for (int step = 0; step <= steps; ++step) {
        const double x =
            range.left + (range.right - range.left) * static_cast<double>(step) / steps;
        const auto point = curveGraphPoint(rect, range, {x, runtime::sampleGradeCurve(*shape, x)});
        if (step == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    painter.save();
    painter.setClipRect(rect);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, width));
    painter.drawPath(path);
    painter.restore();
}

} // namespace

CurveGraphRange curveGraphRange(const GradeCurve curve,
                                const std::span<const document::PathValue> values) {
    CurveGraphRange range;
    if (document::isHueGradeCurve(curve)) {
        range = curve == GradeCurve::HueVsHue
                    ? CurveGraphRange{0.0, 1.0, -document::kMaximumHueShift,
                                      document::kMaximumHueShift}
                    : CurveGraphRange{0.0, 1.0, 0.0, 2.0};
        for (const auto& value : values)
            for (const auto& anchor : value.anchors)
                range.top = std::max(range.top, anchor.point.y);
        return range;
    }
    double low = 0.0;
    double high = 1.0;
    double lowY = 0.0;
    double highY = curve == GradeCurve::LumaVsSaturation ? 2.0 : 1.0;
    for (const auto& value : values)
        for (const auto& anchor : value.anchors) {
            low = std::min(low, anchor.point.x);
            high = std::max(high, anchor.point.x);
            lowY = std::min(lowY, anchor.point.y);
            highY = std::max(highY, anchor.point.y);
        }
    if (curve == GradeCurve::LumaVsSaturation)
        return {low, high + kHeadroom * (high - low), 0.0, highY};
    // A channel curve's view is square in value, so the identity is the diagonal.
    low = std::min(low, lowY);
    high = std::max(high, highY);
    high += kHeadroom * (high - low);
    return {low, high, low, high};
}

QPointF curveGraphPoint(const QRectF& rect, const CurveGraphRange& range, const Vec2d value) {
    const double u = (value.x - range.left) / (range.right - range.left);
    const double v = (value.y - range.bottom) / (range.top - range.bottom);
    return {rect.left() + u * rect.width(), rect.bottom() - v * rect.height()};
}

Vec2d curveGraphValue(const QRectF& rect, const CurveGraphRange& range, const QPointF point) {
    const double u = (point.x() - rect.left()) / std::max(1.0, rect.width());
    const double v = (rect.bottom() - point.y()) / std::max(1.0, rect.height());
    return {range.left + u * (range.right - range.left),
            range.bottom + v * (range.top - range.bottom)};
}

void paintCurveGraph(QPainter& painter, const QRectF& rect, const CurveGraphPicture& picture) {
    const auto& range = picture.range;
    const auto active = picture.active;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::SurfaceSunken));
    painter.drawRect(rect);
    const auto at = [&](const double x, const double y) {
        return curveGraphPoint(rect, range, {x, y});
    };
    const bool hue = !picture.overview && document::isHueGradeCurve(active);
    if (hue) {
        // The hue strip behind the curve, wrapping at both edges.
        QLinearGradient strip(rect.topLeft(), rect.topRight());
        constexpr int stops = 48;
        for (int stop = 0; stop <= stops; ++stop) {
            const double h = static_cast<double>(stop) / stops;
            const auto rgb = runtime::gradeHueColor(h);
            QColor color = QColor::fromRgbF(static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                                            static_cast<float>(rgb[2]));
            color.setAlphaF(0.35F);
            strip.setColorAt(h, color);
        }
        painter.setBrush(strip);
        painter.drawRect(rect);
        QRectF band = rect;
        band.setTop(rect.bottom() - kit::px(kit::Spacing::XS));
        strip.setStart(band.topLeft());
        strip.setFinalStop(band.topRight());
        for (int stop = 0; stop <= stops; ++stop) {
            const double h = static_cast<double>(stop) / stops;
            const auto rgb = runtime::gradeHueColor(h);
            strip.setColorAt(h, QColor::fromRgbF(static_cast<float>(rgb[0]),
                                                 static_cast<float>(rgb[1]),
                                                 static_cast<float>(rgb[2])));
        }
        painter.setBrush(strip);
        painter.drawRect(band);
    } else {
        // The 0..1 box, lighter than the HDR range around it.
        const QRectF box = QRectF(at(0.0, 1.0), at(1.0, 0.0)).normalized().intersected(rect);
        painter.setBrush(kit::color(kit::Color::Field));
        painter.drawRect(box);
    }
    if (!picture.histogram.empty()) {
        const auto bins = picture.histogram.size();
        QColor ink = kit::color(kit::Color::Muted);
        ink.setAlphaF(0.35F);
        painter.setBrush(ink);
        const double base = hue ? range.bottom : std::max(range.bottom, 0.0);
        const QRectF box = QRectF(at(0.0, base), at(1.0, range.top)).normalized();
        const auto count = static_cast<qreal>(bins);
        for (std::size_t bin = 0; bin < bins; ++bin) {
            const qreal x0 = box.left() + box.width() * static_cast<qreal>(bin) / count;
            const qreal x1 = box.left() + box.width() * static_cast<qreal>(bin + 1) / count;
            const qreal height = box.height() * 0.9 * static_cast<qreal>(picture.histogram[bin]);
            painter.drawRect(QRectF(x0, box.bottom() - height, x1 - x0, height));
        }
    }
    // Quarter lines, then the 0..1 box outline.
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.setBrush(Qt::NoBrush);
    for (int line = 1; line < 4; ++line) {
        const double t = line / 4.0;
        if (hue) {
            painter.drawLine(at(t, range.bottom), at(t, range.top));
        } else {
            painter.drawLine(at(t, std::max(range.bottom, 0.0)), at(t, 1.0));
            painter.drawLine(at(std::max(range.left, 0.0), t), at(1.0, t));
        }
    }
    if (!hue) {
        kit::applyHairlinePen(painter, kit::color(kit::Color::BorderHover));
        painter.drawRect(QRectF(at(0.0, 1.0), at(1.0, 0.0)).normalized());
    }
    // The identity: the diagonal for a channel curve, the neutral line otherwise.
    QPen identity(kit::color(kit::Color::Faint), kit::kHairlineWidth, Qt::DashLine);
    painter.setPen(identity);
    if (picture.overview || document::isChannelGradeCurve(active)) {
        painter.drawLine(at(range.left, range.left), at(range.right, range.right));
    } else {
        const double neutral = document::neutralGradeCurveOutput(active);
        painter.drawLine(at(range.left, neutral), at(range.right, neutral));
    }
    if (picture.overview) {
        for (const auto curve : {GradeCurve::Alpha, GradeCurve::Red, GradeCurve::Green,
                                 GradeCurve::Blue, GradeCurve::Master})
            if (const auto& value = picture.curves[document::gradeCurveIndex(curve)])
                paintCurve(painter, rect, range, curve, *value, curveInk(curve), 1.5);
        painter.restore();
        return;
    }
    if (document::isChannelGradeCurve(active))
        for (const auto curve : {GradeCurve::Master, GradeCurve::Red, GradeCurve::Green,
                                 GradeCurve::Blue}) {
            const auto& value = picture.curves[document::gradeCurveIndex(curve)];
            if (curve == active || !value || active == GradeCurve::Alpha)
                continue;
            QColor faint = curveInk(curve);
            faint.setAlphaF(0.3F);
            paintCurve(painter, rect, range, curve, *value, faint, 1.0);
        }
    const auto& value = picture.curves[document::gradeCurveIndex(active)];
    if (value) {
        paintCurve(painter, rect, range, active, *value, curveInk(active), 2.0);
        for (std::size_t index = 0; index < value->anchors.size(); ++index) {
            const auto centre = curveGraphPoint(rect, range, value->anchors[index].point);
            const bool selected = picture.selected == index;
            painter.setPen(QPen(selected ? kit::color(kit::Color::Accent) : curveInk(active),
                                picture.hovered == index ? 2.0 : 1.5));
            painter.setBrush(selected ? kit::color(kit::Color::Accent)
                                      : kit::color(kit::Color::SurfaceSunken));
            painter.drawEllipse(centre, kPointRadius, kPointRadius);
        }
    }
    painter.restore();
}

std::array<std::optional<document::PathValue>, document::kGradeCurveCount>
curveGraphValues(const CompositionSession& session, const document::NodeId node) {
    std::array<std::optional<document::PathValue>, document::kGradeCurveCount> values{};
    for (std::size_t index = 0; index < document::kGradeCurveCount; ++index) {
        const auto parameter = curveParameter(session, node, document::kGradeCurves[index]);
        const auto value = parameter ? session.liveValue(*parameter) : std::nullopt;
        if (value)
            if (const auto* path = std::get_if<document::PathValue>(&*value))
                values[index] = *path;
    }
    return values;
}

// --- CurveGraphCanvas ---------------------------------------------------------------------------

CurveGraphCanvas::CurveGraphCanvas(CompositionSession& session, const document::NodeId node,
                                   QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("curveGraphCanvas"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(kit::px(kit::Size::PropertyRow) * 8);
    setAccessibleName(tr("Curve graph"));
    setToolTip(tr("Click to add a point, drag to move it, double-click or Delete to remove it"));
    for (const auto signal : {&CompositionSession::snapshotChanged,
                              &CompositionSession::liveValueChanged,
                              &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, qOverload<>(&QWidget::update));
    if (auto* previews = session_.imagePreviews())
        connect(previews, &ImagePreviewController::changed, this, qOverload<>(&QWidget::update));
}

QSize CurveGraphCanvas::sizeHint() const { return {240, height()}; }
QSize CurveGraphCanvas::minimumSizeHint() const { return {120, height()}; }

void CurveGraphCanvas::setCurve(const GradeCurve curve) {
    if (curve_ == curve)
        return;
    cancelDrag();
    curve_ = curve;
    hovered_.reset();
    setSelected(std::nullopt);
    update();
}

void CurveGraphCanvas::setSelected(const std::optional<std::size_t> point) {
    if (selected_ == point)
        return;
    selected_ = point;
    emit selectionChanged();
    update();
}

std::optional<document::ParameterId> CurveGraphCanvas::parameter() const {
    return curveParameter(session_, node_, curve_);
}

std::optional<document::PathValue> CurveGraphCanvas::value() const {
    const auto id = parameter();
    const auto value = id ? session_.liveValue(*id) : std::nullopt;
    const auto* path = value ? std::get_if<document::PathValue>(&*value) : nullptr;
    return path ? std::optional(*path) : std::nullopt;
}

QRectF CurveGraphCanvas::plotRect() const {
    const qreal inset = kPointRadius + kit::px(kit::Spacing::XXS);
    return QRectF(rect()).adjusted(inset, inset, -inset, -inset);
}

CurveGraphRange CurveGraphCanvas::range() const {
    if (drag_)
        return drag_->range;
    const auto current = value();
    if (!current)
        return curveGraphRange(curve_, {});
    return curveGraphRange(curve_, std::span(&*current, 1));
}

std::optional<std::size_t> CurveGraphCanvas::pointAt(const QPointF position) const {
    const auto current = value();
    if (!current)
        return std::nullopt;
    const auto plot = plotRect();
    const auto shown = range();
    std::optional<std::size_t> nearest;
    qreal best = kHitRadius * kHitRadius;
    for (std::size_t index = 0; index < current->anchors.size(); ++index) {
        const auto delta = curveGraphPoint(plot, shown, current->anchors[index].point) - position;
        const qreal distance = QPointF::dotProduct(delta, delta);
        if (distance <= best) {
            best = distance;
            nearest = index;
        }
    }
    return nearest;
}

Vec2d CurveGraphCanvas::clamped(const Vec2d value, const CurveGraphRange& shown) const {
    const auto input = document::gradeCurveInputDomain(curve_);
    const auto output = document::gradeCurveOutputDomain(curve_);
    return {std::clamp(value.x, std::max(input.minimum, shown.left),
                       std::min(input.maximum, shown.right)),
            std::clamp(value.y, std::max(output.minimum, shown.bottom),
                       std::min(output.maximum, shown.top))};
}

void CurveGraphCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    CurveGraphPicture picture;
    picture.active = curve_;
    picture.curves = curveGraphValues(session_, node_);
    picture.selected = selected_;
    picture.hovered = hovered_;
    picture.range = range();
    auto* previews = session_.imagePreviews();
    // Requested only here, while the graph is drawn: a hidden graph asks for nothing.
    if (previews != nullptr && previews->histogramsShown() && showsHistogram(curve_))
        picture.histogram = previews->histogram(node_).bins;
    paintCurveGraph(painter, plotRect(), picture);
}

void CurveGraphCanvas::mousePressEvent(QMouseEvent* event) {
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
    const auto id = parameter();
    auto current = value();
    if (!id || !current)
        return;
    const auto shown = range();
    auto hit = pointAt(event->position());
    if (!hit && current->anchors.size() >= document::kMaximumCurvePoints)
        return;
    if (!session_.beginValueEdit(*id))
        return;
    if (!hit) {
        // A click on the graph adds a point there; the drag that follows moves it, and the release
        // commits the two as one command.
        const auto point = clamped(curveGraphValue(plotRect(), shown, event->position()), shown);
        current->anchors.push_back({point, std::nullopt, std::nullopt,
                                    document::PathAnchorMode::Corner});
        hit = current->anchors.size() - 1;
        if (!session_.updateValueEdit(*current)) {
            session_.cancelValueEdit();
            return;
        }
    }
    drag_ = Drag{*hit, shown, false, event->position()};
    setSelected(hit);
    event->accept();
}

void CurveGraphCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_) {
        const auto hover = pointAt(event->position());
        if (hover != hovered_) {
            hovered_ = hover;
            update();
        }
        return;
    }
    const auto id = parameter();
    if (!id || !session_.isValueEditing(*id)) {
        drag_.reset();
        return;
    }
    auto current = value();
    if (!current || drag_->point >= current->anchors.size())
        return;
    current->anchors[drag_->point].point =
        clamped(curveGraphValue(plotRect(), drag_->range, event->position()), drag_->range);
    drag_->moved = true;
    if (!session_.updateValueEdit(*current))
        cancelDrag();
    event->accept();
}

void CurveGraphCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    drag_.reset();
    // One command for the whole gesture; a press that changed nothing commits nothing.
    static_cast<void>(session_.commitValueEdit());
    update();
    event->accept();
}

void CurveGraphCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (const auto hit = pointAt(event->position())) {
        cancelDrag();
        static_cast<void>(removePoint(*hit));
        event->accept();
        return;
    }
    mousePressEvent(event);
}

void CurveGraphCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_ &&
        !drag_) {
        static_cast<void>(removeSelected());
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CurveGraphCanvas::focusOutEvent(QFocusEvent* event) {
    cancelDrag();
    QWidget::focusOutEvent(event);
}

void CurveGraphCanvas::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void CurveGraphCanvas::leaveEvent(QEvent* event) {
    if (hovered_) {
        hovered_.reset();
        update();
    }
    QWidget::leaveEvent(event);
}

void CurveGraphCanvas::cancelDrag() {
    if (!drag_)
        return;
    drag_.reset();
    const auto id = parameter();
    if (id && session_.isValueEditing(*id))
        session_.cancelValueEdit();
    update();
}

bool CurveGraphCanvas::removeSelected() { return selected_ && removePoint(*selected_); }

bool CurveGraphCanvas::removePoint(const std::size_t point) {
    const auto id = parameter();
    auto current = value();
    // A channel curve keeps its two end points' worth of shape; the others may become empty.
    const std::size_t minimum = document::isChannelGradeCurve(curve_) ? 2 : 0;
    if (!id || !current || point >= current->anchors.size() ||
        current->anchors.size() <= minimum)
        return false;
    current->anchors.erase(current->anchors.begin() + static_cast<std::ptrdiff_t>(point));
    setSelected(std::nullopt);
    return session_.setParameterValue(*id, std::move(*current), tr("Remove Curve Point"));
}

// --- CurveGraphField ----------------------------------------------------------------------------

CurveGraphField::CurveGraphField(CompositionSession& session, const document::NodeId node,
                                 std::vector<GradeCurve> curves, QWidget* parent)
    : QWidget(parent), session_(session), node_(node), curves_(std::move(curves)) {
    setObjectName(QStringLiteral("curveGraphField"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    tabs_ = new kit::KTabBar(this);
    tabs_->setObjectName(QStringLiteral("curveGraphTabs"));
    for (const auto curve : curves_)
        tabs_->addTab(tabText(curve));
    tabs_->setCurrentIndex(0);
    header->addWidget(tabs_, 1);
    diamond_ = new KeyframeDiamond(
        session_, std::string(document::gradeCurveRole(curves_.empty() ? GradeCurve::Master
                                                                       : curves_.front())),
        this);
    diamond_->setObjectName(QStringLiteral("curveGraphDiamond"));
    diamond_->setToolTip(tr("Add or remove a key for the whole curve at the playhead"));
    header->addWidget(diamond_);
    layout->addLayout(header);
    canvas_ = new CurveGraphCanvas(session_, node_, this);
    if (!curves_.empty())
        canvas_->setCurve(curves_.front());
    layout->addWidget(canvas_);
    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(kit::px(kit::Spacing::XS));
    input_ = new kit::KValueField(this);
    input_->setObjectName(QStringLiteral("curveGraphInput"));
    input_->setLabel(tr("In"));
    input_->setAccessibleName(tr("Selected point input"));
    output_ = new kit::KValueField(this);
    output_->setObjectName(QStringLiteral("curveGraphOutput"));
    output_->setLabel(tr("Out"));
    output_->setAccessibleName(tr("Selected point output"));
    for (auto* field : {input_, output_}) {
        field->setCompact(true);
        footer->addWidget(field, 1);
        connect(field, &kit::KValueField::editStarted, this, &CurveGraphField::beginPointEdit);
        connect(field, &kit::KValueField::editFinished, this, [this] {
            if (pointEdit_)
                static_cast<void>(session_.commitValueEdit());
            pointEdit_ = false;
        });
        connect(field, &kit::KValueField::editCancelled, this, [this] {
            if (pointEdit_)
                session_.cancelValueEdit();
            pointEdit_ = false;
        });
    }
    connect(input_, &kit::KValueField::valueChanged, this,
            [this](const double value) { updatePointEdit(true, value); });
    connect(output_, &kit::KValueField::valueChanged, this,
            [this](const double value) { updatePointEdit(false, value); });
    histogram_ = new kit::KCheckBox(tr("Histogram"), this);
    histogram_->setObjectName(QStringLiteral("curveGraphHistogram"));
    histogram_->setCheckable(true);
    histogram_->setAccessibleName(tr("Histogram"));
    histogram_->setToolTip(tr("Draw the input's luminance histogram behind the curve"));
    footer->addWidget(histogram_);
    layout->addLayout(footer);
    connect(histogram_, &kit::KSwitch::toggled, this, [this](const bool on) {
        if (auto* previews = session_.imagePreviews())
            previews->setHistogramsShown(on);
    });
    connect(tabs_, &kit::KTabBar::currentChanged, this, [this](const int index) {
        if (index >= 0 && static_cast<std::size_t>(index) < curves_.size())
            setCurve(curves_[static_cast<std::size_t>(index)]);
    });
    connect(canvas_, &CurveGraphCanvas::selectionChanged, this, &CurveGraphField::refresh);
    for (const auto signal : {&CompositionSession::snapshotChanged,
                              &CompositionSession::liveValueChanged,
                              &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &CurveGraphField::refresh);
    refresh();
}

GradeCurve CurveGraphField::curve() const noexcept { return canvas_->curve(); }

void CurveGraphField::setCurve(const GradeCurve curve) {
    const auto found = std::ranges::find(curves_, curve);
    if (found == curves_.end())
        return;
    if (pointEdit_) {
        session_.cancelValueEdit();
        pointEdit_ = false;
    }
    canvas_->setCurve(curve);
    const QSignalBlocker blocker(tabs_);
    tabs_->setCurrentIndex(static_cast<int>(found - curves_.begin()));
    diamond_->setRole(std::string(document::gradeCurveRole(curve)));
    refresh();
}

void CurveGraphField::refresh() {
    const auto parameter = canvas_->parameter();
    diamond_->setParameterId(parameter.value_or(document::ParameterId{}));
    diamond_->refresh();
    const auto curve = canvas_->curve();
    histogram_->setVisible(showsHistogram(curve));
    if (auto* previews = session_.imagePreviews()) {
        const QSignalBlocker blocker(histogram_);
        histogram_->setChecked(previews->histogramsShown());
    }
    const auto value = canvas_->value();
    const auto selected = canvas_->selected();
    const bool point = value && selected && *selected < value->anchors.size();
    input_->setEnabled(point);
    output_->setEnabled(point);
    const auto input = document::gradeCurveInputDomain(curve);
    const auto output = document::gradeCurveOutputDomain(curve);
    input_->setRange(input.minimum, input.maximum);
    output_->setRange(output.minimum, output.maximum);
    if (point && !pointEdit_) {
        input_->setValue(value->anchors[*selected].point.x);
        output_->setValue(value->anchors[*selected].point.y);
    }
    canvas_->update();
}

void CurveGraphField::beginPointEdit() {
    const auto parameter = canvas_->parameter();
    pointEdit_ = parameter && canvas_->selected() && session_.beginValueEdit(*parameter);
}

void CurveGraphField::updatePointEdit(const bool input, const double value) {
    if (!pointEdit_)
        return;
    auto current = canvas_->value();
    const auto selected = canvas_->selected();
    if (!current || !selected || *selected >= current->anchors.size())
        return;
    auto& point = current->anchors[*selected].point;
    (input ? point.x : point.y) = value;
    static_cast<void>(session_.updateValueEdit(std::move(*current)));
}

} // namespace bloom::ui

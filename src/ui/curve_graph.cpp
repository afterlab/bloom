#include <bloom/ui/curve_graph.hpp>

#include <bloom/document/grade_curves.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/grade_curve.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>
#include <bloom/ui/kit/value_field.hpp>

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
#include <memory>

namespace bloom::ui {
namespace {

using document::GradeCurve;
using document::GradeCurvePreset;
using document::Vec2d;

// A point is drawn as large as a node socket and can be picked well outside it, so it is easy to
// hit at card size.
constexpr qreal kPointRadius = kit::px(kit::Size::NodeSocketDot) / 2.0;
constexpr qreal kHitRadius = kPointRadius + kit::px(kit::Spacing::S);
// How far past the graph's edge a dragged point leaves it.
constexpr qreal kOffDistance = kit::px(kit::Spacing::M);
// The share of the range a channel curve's view keeps above its highest point (and above 1).
constexpr double kHeadroom = 0.25;
// Ctrl (Cmd) drags a point at this share of the pointer's speed.
constexpr double kFineScale = 0.1;
// The graph's height over its width.
constexpr double kCanvasAspect = 0.75;
// The hue tab follows the five channel tabs.
constexpr int kHueTab = static_cast<int>(document::kGradeChannelCount);
constexpr std::array kHueCurves{GradeCurve::HueVsHue, GradeCurve::HueVsSaturation,
                                GradeCurve::HueVsLuma, GradeCurve::LumaVsSaturation};

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] qreal band() { return kit::px(kit::Spacing::XS) + kit::px(kit::Spacing::XXS); }
[[nodiscard]] int canvasHeight(const int width) {
    return static_cast<int>(std::round(width * kCanvasAspect));
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

[[nodiscard]] QColor rgb(const std::array<double, 3>& value, const float alpha = 1.0F) {
    QColor color = QColor::fromRgbF(static_cast<float>(std::clamp(value[0], 0.0, 1.0)),
                                    static_cast<float>(std::clamp(value[1], 0.0, 1.0)),
                                    static_cast<float>(std::clamp(value[2], 0.0, 1.0)));
    color.setAlphaF(alpha);
    return color;
}

// The colour an output axis shows at `value`: the hue turned by a Hue vs Hue shift, grey to colour
// for a saturation gain (1 is the colour as it is), black to white for a luma gain.
[[nodiscard]] QColor outputAxisColor(const GradeCurve curve, const double value) {
    if (curve == GradeCurve::HueVsHue)
        return rgb(runtime::gradeHueColor(value - std::floor(value)));
    const double amount = std::clamp(value / 2.0, 0.0, 1.0);
    if (curve == GradeCurve::HueVsLuma)
        return rgb({amount, amount, amount});
    const auto vivid = runtime::gradeHueColor(0.05);
    const double grey = 0.5;
    return rgb({grey + (vivid[0] - grey) * amount, grey + (vivid[1] - grey) * amount,
                grey + (vivid[2] - grey) * amount});
}

[[nodiscard]] std::optional<document::ParameterId> curveParameter(const CompositionSession& session,
                                                                  const document::NodeId node,
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

// A value field's presentation for a curve's input or output, in displayed units: a hue in
// degrees, a gain as a multiplier, a channel value as it is.
struct AxisFormat final {
    double scale = 1.0;
    int decimals = 3;
    QString unit;
};

[[nodiscard]] AxisFormat inputFormat(const GradeCurve curve) {
    if (document::isHueGradeCurve(curve))
        return {360.0, 1, QStringLiteral("°")};
    return {};
}

[[nodiscard]] AxisFormat outputFormat(const GradeCurve curve) {
    if (curve == GradeCurve::HueVsHue)
        return {360.0, 1, QStringLiteral("°")};
    if (!document::isChannelGradeCurve(curve))
        return {1.0, 2, QStringLiteral("×")};
    return {};
}

void configureAxisField(kit::KValueField& field, const AxisFormat& format,
                        const document::GradeCurveRange& domain) {
    kit::ValueFieldPresentation presentation;
    presentation.minimum = domain.minimum * format.scale;
    presentation.maximum = domain.maximum * format.scale;
    presentation.softMinimum = 0.0;
    presentation.softMaximum = format.scale;
    presentation.step = format.scale / 1000.0;
    presentation.decimals = format.decimals;
    presentation.unit = format.unit;
    presentation.displayScale = format.scale;
    field.setPresentation(presentation);
}

} // namespace

QString gradeCurveName(const GradeCurve curve) {
    switch (curve) {
    case GradeCurve::Master:
        return QObject::tr("RGB");
    case GradeCurve::Red:
        return QObject::tr("Red");
    case GradeCurve::Green:
        return QObject::tr("Green");
    case GradeCurve::Blue:
        return QObject::tr("Blue");
    case GradeCurve::Alpha:
        return QObject::tr("Alpha");
    case GradeCurve::HueVsHue:
        return QObject::tr("Hue vs Hue");
    case GradeCurve::HueVsSaturation:
        return QObject::tr("Hue vs Saturation");
    case GradeCurve::HueVsLuma:
        return QObject::tr("Hue vs Luma");
    case GradeCurve::LumaVsSaturation:
        return QObject::tr("Luma vs Saturation");
    }
    return {};
}

QString gradeCurvePresetName(const GradeCurve curve, const GradeCurvePreset preset) {
    if (!document::isChannelGradeCurve(curve))
        return QObject::tr("Reset");
    switch (preset) {
    case GradeCurvePreset::Linear:
        return QObject::tr("Linear");
    case GradeCurvePreset::MediumContrast:
        return QObject::tr("Medium Contrast");
    case GradeCurvePreset::StrongContrast:
        return QObject::tr("Strong Contrast");
    case GradeCurvePreset::LowContrast:
        return QObject::tr("Low Contrast");
    case GradeCurvePreset::Lighten:
        return QObject::tr("Lighten");
    case GradeCurvePreset::Darken:
        return QObject::tr("Darken");
    case GradeCurvePreset::Fade:
        return QObject::tr("Fade");
    case GradeCurvePreset::CrushBlacks:
        return QObject::tr("Crush Blacks");
    case GradeCurvePreset::Invert:
        return QObject::tr("Invert");
    }
    return {};
}

QString curveGraphInputName(const GradeCurve curve) {
    if (document::isHueGradeCurve(curve))
        return QObject::tr("Hue");
    if (curve == GradeCurve::LumaVsSaturation)
        return QObject::tr("Luma");
    return QObject::tr("In");
}

QString curveGraphOutputName(const GradeCurve curve) {
    switch (curve) {
    case GradeCurve::HueVsHue:
        return QObject::tr("Hue shift");
    case GradeCurve::HueVsSaturation:
    case GradeCurve::LumaVsSaturation:
        return QObject::tr("Saturation");
    case GradeCurve::HueVsLuma:
        return QObject::tr("Luma");
    default:
        return QObject::tr("Out");
    }
}

CurveGraphRange curveGraphRange(const GradeCurve curve,
                                const std::span<const document::PathValue> values) {
    CurveGraphRange range;
    if (document::isHueGradeCurve(curve)) {
        range =
            curve == GradeCurve::HueVsHue
                ? CurveGraphRange{0.0, 1.0, -document::kMaximumHueShift, document::kMaximumHueShift}
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
    // #501: one uniform grey under the whole graph, HDR range included, as every grading graph.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::Field));
    painter.drawRect(rect);
    const auto at = [&](const double x, const double y) {
        return curveGraphPoint(rect, range, {x, y});
    };
    const bool hue = document::isHueGradeCurve(active);
    // The hue curves and Luma vs Saturation draw what their axes mean.
    const bool axes = !document::isChannelGradeCurve(active);
    constexpr int stops = 48;
    if (hue) {
        // The hue strip behind the curve, wrapping at both edges.
        QLinearGradient strip(rect.topLeft(), rect.topRight());
        for (int stop = 0; stop <= stops; ++stop) {
            const double h = static_cast<double>(stop) / stops;
            strip.setColorAt(h, rgb(runtime::gradeHueColor(h), 0.35F));
        }
        painter.setBrush(strip);
        painter.drawRect(rect);
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
    // The grid: a line every quarter across the whole graph, the same past 1 as below it.
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.setBrush(Qt::NoBrush);
    if (hue) {
        for (int line = 1; line < 4; ++line)
            painter.drawLine(at(line / 4.0, range.bottom), at(line / 4.0, range.top));
    } else {
        for (double t = std::ceil(range.left * 4.0) / 4.0; t <= range.right; t += 0.25)
            if (t > range.left)
                painter.drawLine(at(t, range.bottom), at(t, range.top));
        for (double t = std::ceil(range.bottom * 4.0) / 4.0; t <= range.top; t += 0.25)
            if (t > range.bottom)
                painter.drawLine(at(range.left, t), at(range.right, t));
    }
    // The identity: the diagonal for a channel curve, the neutral line otherwise.
    QPen identity(kit::color(kit::Color::Faint), kit::kHairlineWidth, Qt::DashLine);
    painter.setPen(identity);
    if (document::isChannelGradeCurve(active)) {
        painter.drawLine(at(range.left, range.left), at(range.right, range.right));
    } else {
        const double neutral = document::neutralGradeCurveOutput(active);
        painter.drawLine(at(range.left, neutral), at(range.right, neutral));
    }
    if (axes) {
        // A band along each axis: the input's hue strip or luma ramp along the bottom, the
        // output's colour up the left.
        painter.setPen(Qt::NoPen);
        const QRectF bottom(rect.left(), rect.bottom() - band(), rect.width(), band());
        QLinearGradient across(at(0.0, range.bottom), at(1.0, range.bottom));
        for (int stop = 0; stop <= stops; ++stop) {
            const double t = static_cast<double>(stop) / stops;
            across.setColorAt(t, hue ? rgb(runtime::gradeHueColor(t)) : rgb({t, t, t}));
        }
        painter.setBrush(across);
        painter.drawRect(bottom);
        const QRectF left(rect.left(), rect.top(), band(), rect.height() - band());
        QLinearGradient up(rect.bottomLeft(), rect.topLeft());
        for (int stop = 0; stop <= stops; ++stop) {
            const double t = static_cast<double>(stop) / stops;
            up.setColorAt(t,
                          outputAxisColor(active, range.bottom + t * (range.top - range.bottom)));
        }
        painter.setBrush(up);
        painter.drawRect(left);
        // The axis names, inside the plot beside their bands.
        painter.setFont(kit::font(kit::TypeRole::UiSmall));
        painter.setPen(kit::color(kit::Color::Muted));
        const qreal inset = band() + kit::px(kit::Spacing::XS);
        const QRectF text =
            rect.adjusted(inset, kit::px(kit::Spacing::XXS), -kit::px(kit::Spacing::XS), -inset);
        painter.drawText(text, Qt::AlignRight | Qt::AlignBottom, curveGraphInputName(active));
        painter.drawText(text, Qt::AlignLeft | Qt::AlignTop, curveGraphOutputName(active));
    }
    if (document::isChannelGradeCurve(active))
        for (const auto curve :
             {GradeCurve::Master, GradeCurve::Red, GradeCurve::Green, GradeCurve::Blue}) {
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
    setAccessibleName(tr("Curve graph"));
    setToolTip(gradeCurveName(curve_));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, qOverload<>(&QWidget::update));
    if (auto* previews = session_.imagePreviews())
        connect(previews, &ImagePreviewController::changed, this, qOverload<>(&QWidget::update));
}

QSize CurveGraphCanvas::sizeHint() const { return {240, canvasHeight(240)}; }
QSize CurveGraphCanvas::minimumSizeHint() const { return {120, canvasHeight(120)}; }

void CurveGraphCanvas::setCurve(const GradeCurve curve) {
    if (curve_ == curve)
        return;
    cancelDrag();
    curve_ = curve;
    hovered_.reset();
    addedRevision_.reset();
    setToolTip(gradeCurveName(curve_));
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

std::size_t CurveGraphCanvas::minimumPoints() const noexcept {
    // A channel curve keeps two points' worth of shape; the others may become empty.
    return document::isChannelGradeCurve(curve_) ? 2 : 0;
}

bool CurveGraphCanvas::offGraph(const QPointF position) const {
    return !QRectF(rect())
                .adjusted(-kOffDistance, -kOffDistance, kOffDistance, kOffDistance)
                .contains(position);
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
    if (hasFocus()) {
        kit::applyHairlinePen(painter, kit::color(kit::Color::Accent));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plotRect());
    }
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
    addedRevision_.reset();
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
    Drag drag;
    drag.range = shown;
    drag.from = event->position();
    drag.fine = event->modifiers().testFlag(Qt::ControlModifier);
    if (!hit) {
        // A click on the graph adds a point there; the drag that follows moves it, and the release
        // commits the two as one command.
        const auto point = clamped(curveGraphValue(plotRect(), shown, event->position()), shown);
        current->anchors.push_back(
            {point, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
        hit = current->anchors.size() - 1;
        drag.added = true;
        if (!session_.updateValueEdit(*current)) {
            session_.cancelValueEdit();
            return;
        }
    }
    drag.point = *hit;
    drag.base = *current;
    drag.origin = current->anchors[*hit].point;
    drag.at = drag.origin;
    drag_ = std::move(drag);
    setSelected(hit);
    event->accept();
}

void CurveGraphCanvas::dragTo(const QPointF position, const bool fine) {
    auto& drag = *drag_;
    if (fine != drag.fine) {
        // A modifier change continues from where the point is now, so it never jumps.
        drag.origin = drag.at;
        drag.from = position;
        drag.fine = fine;
    }
    const auto plot = plotRect();
    const auto from = curveGraphValue(plot, drag.range, drag.from);
    const auto to = curveGraphValue(plot, drag.range, position);
    const double scale = drag.fine ? kFineScale : 1.0;
    // Relative: the point moves by the pointer's travel, from where it was pressed.
    drag.at =
        clamped({drag.origin.x + (to.x - from.x) * scale, drag.origin.y + (to.y - from.y) * scale},
                drag.range);
    auto next = drag.base;
    drag.off = offGraph(position) && next.anchors.size() > minimumPoints();
    if (drag.off)
        next.anchors.erase(next.anchors.begin() + static_cast<std::ptrdiff_t>(drag.point));
    else
        next.anchors[drag.point].point = drag.at;
    if (!session_.updateValueEdit(std::move(next)))
        cancelDrag();
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
    dragTo(event->position(), event->modifiers().testFlag(Qt::ControlModifier));
    event->accept();
}

void CurveGraphCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool added = drag_->added;
    const bool off = drag_->off;
    drag_.reset();
    if (off)
        setSelected(std::nullopt);
    // One command for the whole gesture; a press that changed nothing commits nothing.
    const auto id = parameter();
    if (id && session_.isValueEditing(*id) && session_.commitValueEdit() && added && !off)
        addedRevision_ = session_.snapshot().revision();
    update();
    event->accept();
}

void CurveGraphCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    // A double-click resets the curve. When its first click added a point, that point is taken
    // back first, so the reset is the one step the double-click leaves in the history.
    if (addedRevision_ && *addedRevision_ == session_.snapshot().revision())
        static_cast<void>(session_.undo());
    addedRevision_.reset();
    static_cast<void>(resetCurve());
    event->accept();
}

QMenu* CurveGraphCanvas::makeContextMenu(const QPointF position) {
    auto* menu = kit::makeMenu(this);
    const auto id = parameter();
    const auto current = value();
    const auto hit = pointAt(position);
    auto* remove = menu->addAction(tr("Delete Point"));
    remove->setObjectName(QStringLiteral("curveGraphDeletePoint"));
    remove->setEnabled(hit.has_value() && current && current->anchors.size() > minimumPoints());
    if (hit)
        connect(remove, &QAction::triggered, this,
                [this, point = *hit] { static_cast<void>(removePoint(point)); });
    auto* reset = menu->addAction(tr("Reset Curve"));
    reset->setObjectName(QStringLiteral("curveGraphReset"));
    reset->setEnabled(current && *current != document::defaultGradeCurve(curve_));
    connect(reset, &QAction::triggered, this, [this] { static_cast<void>(resetCurve()); });
    const auto state =
        id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    if (state != KeyframeDiamondState::Unsupported) {
        menu->addSeparator();
        auto* key = menu->addAction(
            state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key") : tr("Add Key"));
        key->setObjectName(QStringLiteral("curveGraphKey"));
        connect(key, &QAction::triggered, this, [this, parameter = *id] {
            static_cast<void>(session_.toggleKeyframeForParameter(parameter));
        });
    }
    return menu;
}

void CurveGraphCanvas::contextMenuEvent(QContextMenuEvent* event) {
    if (drag_) {
        event->accept();
        return;
    }
    const std::unique_ptr<QMenu> menu(makeContextMenu(event->pos()));
    menu->exec(event->globalPos());
    event->accept();
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
    update();
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
    if (!id || !current || point >= current->anchors.size() ||
        current->anchors.size() <= minimumPoints())
        return false;
    current->anchors.erase(current->anchors.begin() + static_cast<std::ptrdiff_t>(point));
    setSelected(std::nullopt);
    return session_.setParameterValue(*id, std::move(*current), tr("Remove Curve Point"));
}

bool CurveGraphCanvas::applyPreset(const GradeCurvePreset preset) {
    const auto id = parameter();
    const auto current = value();
    if (!id || !current)
        return false;
    auto next = document::gradeCurvePreset(curve_, preset);
    if (*current == next)
        return true;
    setSelected(std::nullopt);
    const auto label = preset == GradeCurvePreset::Linear
                           ? tr("Reset Curve")
                           : tr("Curve Preset %1").arg(gradeCurvePresetName(curve_, preset));
    return session_.setParameterValue(*id, std::move(next), label);
}

bool CurveGraphCanvas::resetCurve() { return applyPreset(GradeCurvePreset::Linear); }

// --- CurvesCardWidget ---------------------------------------------------------------------------

CurvesCardWidget::CurvesCardWidget(CompositionSession& session, const document::NodeId node,
                                   const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeCurvesGraph")
                       : QStringLiteral("propertiesCurvesGraph"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    tabs_ = new kit::KTabBar(this);
    tabs_->setObjectName(QStringLiteral("curvesGraphTabs"));
    for (const auto& text : {tr("RGB"), tr("R"), tr("G"), tr("B"), tr("A"), tr("Hue")})
        tabs_->addTab(text);
    tabs_->setCurrentIndex(0);
    hueCurves_ = new kit::KDropdown(this);
    hueCurves_->setObjectName(QStringLiteral("curvesGraphHueCurve"));
    hueCurves_->setAccessibleName(tr("Hue curve"));
    hueCurves_->setControlSize(kit::KDropdown::ControlSize::Dense);
    for (const auto curve : kHueCurves)
        hueCurves_->addItem(gradeCurveName(curve), static_cast<int>(curve));
    hueCurves_->setCurrentIndex(0);
    histogram_ = new kit::KCheckBox(tr("Histogram"), this);
    histogram_->setObjectName(QStringLiteral("curvesGraphHistogram"));
    histogram_->setCheckable(true);
    histogram_->setAccessibleName(tr("Histogram"));
    histogram_->setToolTip(tr("The input's luminance histogram behind the curve"));
    presets_ = new kit::KButton(kit::IconId::CaretDown, tr("Preset"), this);
    presets_->setObjectName(QStringLiteral("curvesGraphPresets"));
    presets_->setControlSize(kit::KButton::ControlSize::Compact);
    presets_->setAccessibleName(tr("Curve preset"));
    diamond_ = new KeyframeDiamond(session_,
                                   std::string(document::gradeCurveRole(GradeCurve::Master)), this);
    diamond_->setObjectName(QStringLiteral("curvesGraphDiamond"));
    canvas_ = new CurveGraphCanvas(session_, node_, this);
    for (auto** field : {&input_, &output_}) {
        *field = new kit::KValueField(this);
        (*field)->setCompact(true);
        (*field)->setMaximumWidth(QWIDGETSIZE_MAX);
        (*field)->setOutlinedAtRest(true);
    }
    input_->setObjectName(QStringLiteral("curvesGraphInput"));
    output_->setObjectName(QStringLiteral("curvesGraphOutput"));
    inputLabel_ = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
    inputLabel_->setTone(kit::Color::Muted);
    outputLabel_ = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
    outputLabel_->setTone(kit::Color::Muted);
    for (auto* field : {input_, output_}) {
        connect(field, &kit::KValueField::editStarted, this, &CurvesCardWidget::beginPointEdit);
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
    connect(tabs_, &kit::KTabBar::currentChanged, this, [this](const int index) {
        if (index >= 0 && index < kHueTab)
            setCurve(document::kGradeCurves[static_cast<std::size_t>(index)]);
        else if (index == kHueTab)
            setCurve(hueCurve_);
    });
    connect(hueCurves_, &kit::KDropdown::activated, this, [this](const int index) {
        if (index >= 0 && static_cast<std::size_t>(index) < kHueCurves.size())
            setCurve(kHueCurves[static_cast<std::size_t>(index)]);
    });
    connect(histogram_, &kit::KSwitch::toggled, this, [this](const bool on) {
        if (auto* previews = session_.imagePreviews())
            previews->setHistogramsShown(on);
    });
    connect(presets_, &QAbstractButton::clicked, this, [this] {
        const std::unique_ptr<QMenu> menu(makePresetMenu());
        menu->exec(presets_->mapToGlobal(QPoint(0, presets_->height())));
    });
    connect(canvas_, &CurveGraphCanvas::selectionChanged, this, &CurvesCardWidget::refresh);
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &CurvesCardWidget::refresh);
    configureCurve();
    refresh();
}

GradeCurve CurvesCardWidget::curve() const noexcept { return canvas_->curve(); }

void CurvesCardWidget::setCurve(const GradeCurve curve) {
    if (pointEdit_) {
        session_.cancelValueEdit();
        pointEdit_ = false;
    }
    if (!document::isChannelGradeCurve(curve))
        hueCurve_ = curve;
    canvas_->setCurve(curve);
    configureCurve();
    refresh();
}

void CurvesCardWidget::configureCurve() {
    const auto active = curve();
    const bool channel = document::isChannelGradeCurve(active);
    {
        const QSignalBlocker tabs(tabs_);
        tabs_->setCurrentIndex(channel ? static_cast<int>(document::gradeCurveIndex(active))
                                       : kHueTab);
        const QSignalBlocker hue(hueCurves_);
        const auto found = std::ranges::find(kHueCurves, hueCurve_);
        hueCurves_->setCurrentIndex(static_cast<int>(found - kHueCurves.begin()));
    }
    hueCurves_->setVisible(!channel);
    histogram_->setVisible(channel);
    diamond_->setRole(std::string(document::gradeCurveRole(active)));
    inputLabel_->setText(curveGraphInputName(active));
    outputLabel_->setText(curveGraphOutputName(active));
    configureAxisField(*input_, inputFormat(active), document::gradeCurveInputDomain(active));
    configureAxisField(*output_, outputFormat(active), document::gradeCurveOutputDomain(active));
    input_->setAccessibleName(tr("Selected point %1").arg(curveGraphInputName(active)));
    output_->setAccessibleName(tr("Selected point %1").arg(curveGraphOutputName(active)));
    place();
}

QMenu* CurvesCardWidget::makePresetMenu() {
    auto* menu = kit::makeMenu(this);
    const auto active = curve();
    const bool channel = document::isChannelGradeCurve(active);
    for (const auto preset : document::kGradeCurvePresets) {
        if (!channel && preset != GradeCurvePreset::Linear)
            continue;
        auto* action = menu->addAction(gradeCurvePresetName(active, preset));
        action->setObjectName(QStringLiteral("curvesGraphPreset.%1").arg(static_cast<int>(preset)));
        connect(action, &QAction::triggered, this,
                [this, preset] { static_cast<void>(canvas_->applyPreset(preset)); });
    }
    return menu;
}

bool CurvesCardWidget::showsRole(const std::string_view role) const {
    // The curves have no rows: the graph is their editor. Clamp is a plain switch row.
    return role == "clamp";
}

int CurvesCardWidget::heightAt(const int width) const {
    return 2 * (control() + small()) + canvasHeight(width) + small() + control();
}

int CurvesCardWidget::minimumWidthHint() const {
    const int header = tabs_->sizeHint().width() + small() + control();
    const int choices = std::max(hueCurves_->sizeHint().width(), histogram_->sizeHint().width()) +
                        small() + presets_->sizeHint().width();
    // The two halves of the footer: each a label column for the longest axis name and a field.
    int label = 0;
    for (const auto curve : document::kGradeCurves) {
        const QFontMetrics metrics(inputLabel_->font());
        label = std::max({label, metrics.horizontalAdvance(curveGraphInputName(curve)),
                          metrics.horizontalAdvance(curveGraphOutputName(curve))});
    }
    const int half = label + small() + kit::px(kit::Size::PropertiesFieldWidth);
    return std::max({header, choices, 2 * half + small(),
                     kit::px(kit::Size::DropdownWidthExpanded) - control()});
}

QSize CurvesCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void CurvesCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void CurvesCardWidget::place() {
    const int w = width();
    // The tabs, and the curve's keyframe diamond at the right of their row.
    tabs_->setGeometry(0, 0, std::max(0, w - control() - small()), control());
    diamond_->setGeometry(std::max(0, w - control()), 0, control(), control());
    // The hue curve's name (or the histogram switch for a channel), and the Preset menu.
    int y = control() + small();
    const int presets = presets_->sizeHint().width();
    presets_->setGeometry(std::max(0, w - presets), y, presets, control());
    const int left = std::max(0, w - presets - small());
    hueCurves_->setGeometry(0, y, std::min(left, hueCurves_->sizeHint().width()), control());
    histogram_->setGeometry(0, y, std::min(left, histogram_->sizeHint().width()), control());
    y += control() + small();
    canvas_->setGeometry(0, y, w, canvasHeight(w));
    y += canvasHeight(w) + small();
    // The selected point's input and output, each with its axis name.
    const int half = std::max(0, (w - small()) / 2);
    int label = 0;
    for (auto* text : {inputLabel_, outputLabel_})
        label = std::max(label, text->sizeHint().width());
    for (int side = 0; side < 2; ++side) {
        const int x = side * (half + small());
        auto* text = side == 0 ? inputLabel_ : outputLabel_;
        auto* field = side == 0 ? input_ : output_;
        text->setGeometry(x, y, label, control());
        field->setGeometry(x + label + small(), y, std::max(0, half - label - small()), control());
    }
}

void CurvesCardWidget::refresh() {
    const auto parameter = canvas_->parameter();
    diamond_->setParameterId(parameter.value_or(document::ParameterId{}));
    diamond_->refresh();
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    canvas_->setEnabled(!locked);
    presets_->setEnabled(!locked);
    if (auto* previews = session_.imagePreviews()) {
        const QSignalBlocker blocker(histogram_);
        histogram_->setChecked(previews->histogramsShown());
    }
    const auto value = canvas_->value();
    const auto selected = canvas_->selected();
    const bool point = !locked && value && selected && *selected < value->anchors.size();
    const auto active = curve();
    for (const bool input : {true, false}) {
        auto* field = input ? input_ : output_;
        field->setEnabled(point);
        if (pointEdit_ || field->hasLiveEdit())
            continue;
        const QSignalBlocker blocker(field);
        const auto format = input ? inputFormat(active) : outputFormat(active);
        if (point) {
            const auto at = value->anchors[*selected].point;
            field->setSpecialValueText({});
            field->setValue((input ? at.x : at.y) * format.scale);
        } else {
            // No point is selected: the field says so rather than show a stale number.
            field->setSpecialValueText(QStringLiteral("–"));
            field->setValue(field->minimum());
        }
    }
    canvas_->update();
}

void CurvesCardWidget::beginPointEdit() {
    const auto parameter = canvas_->parameter();
    pointEdit_ = parameter && canvas_->selected() && session_.beginValueEdit(*parameter);
}

void CurvesCardWidget::updatePointEdit(const bool input, const double shown) {
    if (!pointEdit_)
        return;
    auto current = canvas_->value();
    const auto selected = canvas_->selected();
    if (!current || !selected || *selected >= current->anchors.size())
        return;
    const auto active = curve();
    const double scale = (input ? inputFormat(active) : outputFormat(active)).scale;
    auto& point = current->anchors[*selected].point;
    (input ? point.x : point.y) = shown / scale;
    static_cast<void>(session_.updateValueEdit(std::move(*current)));
}

} // namespace bloom::ui

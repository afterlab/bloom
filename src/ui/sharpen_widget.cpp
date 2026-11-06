#include <bloom/ui/sharpen_widget.hpp>

#include <bloom/document/project.hpp>
#include <bloom/document/sharpen_definition.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QCoreApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace bloom::ui {
namespace {

using document::SharpenParameterSpec;
using namespace std::string_view_literals;

constexpr double kDisplayGamma = 2.2;
// The edge drawn: its two sides in linear light.
constexpr double kDarkSide = 0.2;
constexpr double kBrightSide = 0.6;
// The brightness the graph's height spans, enough for the Amount slider's end on this edge.
constexpr double kGraphMaximum = 1.4;
// The pixels each side of the edge the graph spans, laid out by log(1 + pixels) so that a radius of a
// pixel or two is as easy to see and to drag as a wide one. The Radius slider's end.
constexpr double kGraphSpan = 32.0;
constexpr int kMaximumTaps = 256;
constexpr std::array kHandleRoles{"amount"sv, "radius"sv};

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] qreal handleRadius() { return kit::px(kit::Size::NodeSocketDot) / 2.0 + 1.0; }

[[nodiscard]] const SharpenParameterSpec& specOf(const std::string_view role) {
    for (const auto& spec : document::sharpenParameters())
        if (spec.role == role)
            return spec;
    return document::sharpenParameters().front();
}

[[nodiscard]] QString description(const std::string_view role) {
    if (role == "amount")
        return QCoreApplication::translate("Sharpen",
                                           "How much of the detail is added back: at 100% it is "
                                           "added once.");
    if (role == "radius")
        return QCoreApplication::translate("Sharpen",
                                           "How far from an edge the sharpening reaches, in "
                                           "full-resolution pixels, the same radius as Gaussian Blur.");
    if (role == "threshold")
        return QCoreApplication::translate("Sharpen",
                                           "Detail smaller than this, in linear light, is left alone, "
                                           "so grain and flat areas stay clean.");
    return {};
}

// A graph position from 0 at the edge to 1 at the span, for `pixels` from the edge.
[[nodiscard]] double positionOf(const double pixels) {
    return std::log1p(std::max(pixels, 0.0)) / std::log1p(kGraphSpan);
}
[[nodiscard]] double pixelsOf(const double position) {
    return std::expm1(std::max(position, 0.0) * std::log1p(kGraphSpan));
}

// The share of a Gaussian of `radius` that lies past the edge for the pixel `offset` past it, the
// blur's own kernel: sigma radius / 3 and whole taps out to the radius.
[[nodiscard]] double brightShare(const int offset, const double radius) {
    const int support = std::min(static_cast<int>(std::ceil(radius)), kMaximumTaps);
    if (support <= 0)
        return offset >= 0 ? 1.0 : 0.0;
    double total = 0.0;
    double bright = 0.0;
    for (int tap = -support; tap <= support; ++tap) {
        const double distance = static_cast<double>(tap) / (radius / 3.0);
        const double weight = std::exp(-0.5 * distance * distance);
        total += weight;
        if (offset + tap >= 0)
            bright += weight;
    }
    return bright / total;
}

// The node's pixel (render::unsharpImagePixel) on the drawn edge.
[[nodiscard]] double sharpenedEdge(const int offset, const double amount, const double radius,
                                   const double threshold) {
    const double value = offset >= 0 ? kBrightSide : kDarkSide;
    const double blurred = kDarkSide + (kBrightSide - kDarkSide) * brightShare(offset, radius);
    const double detail = value - blurred;
    const double cored = detail > threshold    ? detail - threshold
                         : detail < -threshold ? detail + threshold
                                               : 0.0;
    return std::max(value + amount * cored, 0.0);
}

} // namespace

// --- The edge graph -----------------------------------------------------------------------------

SharpenProfileGraph::SharpenProfileGraph(CompositionSession& session, const document::NodeId node,
                                         QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("sharpenProfileGraph"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAccessibleName(tr("Sharpening across an edge"));
    setToolTip(tr("An edge as this node sharpens it: drag the handle on the overshoot up or down to "
                  "change the amount, and the handle where the halo ends sideways to change the "
                  "radius"));
}

int SharpenProfileGraph::fixedHeight() {
    // commands::estimatedNodeCardSize holds the same height.
    return 120;
}

QRectF SharpenProfileGraph::plotRect() const {
    const qreal inset = handleRadius() + 1.0;
    return QRectF(rect()).adjusted(inset, inset, -inset, -inset);
}

double SharpenProfileGraph::scalar(const std::string_view role) const {
    const auto fallback = specOf(role).value;
    const auto id = nodeRoleParameter(session_, node_, role);
    return id ? liveScalar(session_, *id).value_or(fallback) : fallback;
}

double SharpenProfileGraph::sharpenedAt(const int offset) const {
    return sharpenedEdge(offset, scalar("amount"), scalar("radius"), scalar("threshold"));
}

QPointF SharpenProfileGraph::handlePosition(const std::size_t handle) const {
    const auto plot = plotRect();
    const double half = plot.width() / 2.0;
    const auto heightOf = [&](const double value) {
        return plot.bottom() - std::clamp(value / kGraphMaximum, 0.0, 1.0) * plot.height();
    };
    if (handle == 0)
        return {plot.center().x() + positionOf(0.5) * half, heightOf(sharpenedAt(0))};
    return {plot.center().x() + std::min(positionOf(scalar("radius")), 1.0) * half,
            heightOf(kBrightSide)};
}

std::optional<std::size_t> SharpenProfileGraph::handleAt(const QPointF position) const {
    // The nearest handle within reach of the pointer.
    std::optional<std::size_t> nearest;
    qreal best = handleRadius() + kit::px(kit::Spacing::S);
    for (std::size_t handle = 0; handle < kHandles; ++handle) {
        const auto at = handlePosition(handle);
        const qreal distance = std::hypot(at.x() - position.x(), at.y() - position.y());
        if (distance <= best) {
            best = distance;
            nearest = handle;
        }
    }
    return nearest;
}

QImage SharpenProfileGraph::preview(const int width, const int height) const {
    const int w = std::max(1, width);
    const int h = std::max(1, height);
    const double amount = scalar("amount");
    const double radius = scalar("radius");
    const double threshold = scalar("threshold");
    QImage image(w, h, QImage::Format_RGB32);
    auto* first = reinterpret_cast<QRgb*>(image.scanLine(0));
    for (int x = 0; x < w; ++x) {
        const double position = 2.0 * (x + 0.5) / w - 1.0;
        const double pixels = pixelsOf(std::abs(position));
        const int offset =
            position >= 0.0 ? static_cast<int>(pixels) : -1 - static_cast<int>(pixels);
        const double value = sharpenedEdge(offset, amount, radius, threshold);
        const int shade = static_cast<int>(
            std::lround(255.0 * std::pow(std::clamp(value, 0.0, 1.0), 1.0 / kDisplayGamma)));
        first[x] = qRgb(shade, shade, shade);
    }
    for (int y = 1; y < h; ++y)
        std::copy_n(first, w, reinterpret_cast<QRgb*>(image.scanLine(y)));
    return image;
}

void SharpenProfileGraph::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    QPainter painter(this);
    const auto plot = plotRect();
    const auto ratio = devicePixelRatioF();
    auto picture = preview(static_cast<int>(std::lround(plot.width() * ratio)),
                           static_cast<int>(std::lround(plot.height() * ratio)));
    picture.setDevicePixelRatio(ratio);
    painter.drawImage(plot.topLeft(), picture);
    kit::applyHairlinePen(painter,
                          kit::color(hasFocus() ? kit::Color::Accent : kit::Color::Border));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);

    painter.setRenderHint(QPainter::Antialiasing, true);
    const double amount = scalar("amount");
    const double radius = scalar("radius");
    const double threshold = scalar("threshold");
    const auto heightOf = [&](const double value) {
        return plot.bottom() - std::clamp(value / kGraphMaximum, 0.0, 1.0) * plot.height();
    };
    const double half = plot.width() / 2.0;
    // The input, the plain edge.
    QPainterPath input;
    input.moveTo(plot.left(), heightOf(kDarkSide));
    input.lineTo(plot.center().x(), heightOf(kDarkSide));
    input.lineTo(plot.center().x(), heightOf(kBrightSide));
    input.lineTo(plot.right(), heightOf(kBrightSide));
    // The result, one point for each pixel each side of the edge.
    QPainterPath curve;
    const int reach = static_cast<int>(kGraphSpan);
    for (int offset = -reach; offset < reach; ++offset) {
        const double pixels = offset >= 0 ? offset + 0.5 : -offset - 0.5;
        const double position = std::min(positionOf(pixels), 1.0);
        const QPointF point(plot.center().x() + (offset >= 0 ? position : -position) * half,
                            heightOf(sharpenedEdge(offset, amount, radius, threshold)));
        if (offset == -reach) {
            curve.moveTo(plot.left(), point.y());
            curve.lineTo(point);
        } else {
            curve.lineTo(point);
        }
    }
    curve.lineTo(plot.right(), heightOf(sharpenedEdge(reach - 1, amount, radius, threshold)));
    painter.setBrush(Qt::NoBrush);
    // A dark edge under each line keeps it readable over the bright side.
    painter.setPen(QPen(kit::color(kit::Color::Background), kit::kHairlineWidth + 2.0));
    painter.drawPath(input);
    painter.setPen(QPen(kit::color(kit::Color::Foreground), kit::kHairlineWidth));
    painter.drawPath(input);
    painter.setPen(QPen(kit::color(kit::Color::Background), kit::kEmphasisStroke + 2.0));
    painter.drawPath(curve);
    painter.setPen(QPen(kit::color(kit::Color::Accent), kit::kEmphasisStroke));
    painter.drawPath(curve);
    for (std::size_t handle = 0; handle < kHandles; ++handle) {
        const bool active = drag_ && drag_->handle == handle;
        const bool hovered = !drag_ && hovered_ == handle;
        painter.setBrush(kit::color(active || hovered ? kit::Color::Accent : kit::Color::Foreground));
        painter.setPen(QPen(kit::color(kit::Color::Background), kit::kHairlineWidth));
        painter.drawEllipse(handlePosition(handle), handleRadius(), handleRadius());
    }
}

void SharpenProfileGraph::mousePressEvent(QMouseEvent* event) {
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
    const auto handle = handleAt(event->position());
    const auto id = handle ? nodeRoleParameter(session_, node_, kHandleRoles[*handle]) : std::nullopt;
    if (!handle || !id || !session_.beginValueEdit(*id))
        return;
    drag_ = Drag{*handle, *id, scalar(kHandleRoles[*handle]), event->position()};
    update();
}

void SharpenProfileGraph::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_) {
        const auto hovered = handleAt(event->position());
        if (hovered != hovered_) {
            hovered_ = hovered;
            update();
        }
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (!session_.isValueEditing(drag_->parameter)) {
        drag_.reset();
        update();
        return;
    }
    const auto plot = plotRect();
    const double scale = kit::KSlider::dragScale(event->modifiers());
    const auto delta = event->position() - drag_->last;
    if (drag_->handle == 0) {
        // The overshoot follows the pointer: the amount moves it by the edge's first pixel's detail.
        const double detail = (kBrightSide - kDarkSide) * (1.0 - brightShare(0, scalar("radius")));
        const double brightness = -delta.y() * scale * kGraphMaximum / std::max(1.0, plot.height());
        if (detail > 0.0)
            drag_->value += brightness / detail;
    } else {
        // The end of the halo follows the pointer along the graph's log(1 + pixels) layout.
        const double position = positionOf(drag_->value) +
                                delta.x() * scale / std::max(1.0, plot.width() / 2.0);
        drag_->value = pixelsOf(position);
    }
    drag_->last = event->position();
    const auto& spec = specOf(kHandleRoles[drag_->handle]);
    drag_->value = std::clamp(drag_->value, spec.minimum, spec.maximum);
    if (!session_.updateValueEdit(drag_->value))
        cancelDrag();
    update();
    event->accept();
}

void SharpenProfileGraph::mouseReleaseEvent(QMouseEvent* event) {
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

void SharpenProfileGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    if (const auto handle = handleAt(event->position()))
        if (const auto id = nodeRoleParameter(session_, node_, kHandleRoles[*handle])) {
            const auto& spec = specOf(kHandleRoles[*handle]);
            static_cast<void>(session_.setParameterValue(
                *id, spec.value,
                tr("Reset %1").arg(QString::fromUtf8(spec.label.data(),
                                                     static_cast<qsizetype>(spec.label.size())))));
        }
    event->accept();
}

void SharpenProfileGraph::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SharpenProfileGraph::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void SharpenProfileGraph::leaveEvent(QEvent* event) {
    if (hovered_) {
        hovered_.reset();
        update();
    }
    QWidget::leaveEvent(event);
}

void SharpenProfileGraph::cancelDrag() {
    if (!drag_)
        return;
    const auto id = drag_->parameter;
    drag_.reset();
    if (session_.isValueEditing(id))
        session_.cancelValueEdit();
    update();
}

// --- The card -----------------------------------------------------------------------------------

SharpenCardWidget::SharpenCardWidget(CompositionSession& session, const document::NodeId node,
                                     const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeSharpen") : QStringLiteral("propertiesSharpen"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    graph_ = new SharpenProfileGraph(session_, node_, this);
    for (const auto& spec : document::sharpenParameters()) {
        auto* row = new GradingValueRow(
            session_, node_, std::string(spec.role), document::sharpenSchemaKey(spec.role),
            QCoreApplication::translate("Sharpen", std::string(spec.label).c_str()), spec.value, this);
        row->setDescription(description(spec.role));
        GradingSliderSpec slider;
        slider.minimum = spec.minimum;
        slider.maximum = spec.softMaximum;
        slider.neutral = spec.value;
        slider.ticks = spec.softMaximum / 4.0;
        slider.gradient = jogStops();
        row->setSlider(slider);
        values_.push_back(row);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &SharpenCardWidget::refresh);
    refresh();
}

GradingValueRow* SharpenCardWidget::valueRow(const std::string_view role) const {
    const auto found =
        std::ranges::find_if(values_, [&](const auto* row) { return row->role() == role; });
    return found == values_.end() ? nullptr : *found;
}

bool SharpenCardWidget::showsRole(const std::string_view role) const {
    // Every value is here.
    return document::sharpenParameterSpec(document::sharpenSchemaKey(role)) == nullptr;
}

std::optional<int> SharpenCardWidget::socketY(const std::string_view role) const {
    if (const auto* row = valueRow(role))
        return row->y() + row->socketY();
    return std::nullopt;
}

int SharpenCardWidget::heightAt(const int width) const {
    static_cast<void>(width);
    int height = SharpenProfileGraph::fixedHeight() + small();
    for (const auto* row : values_)
        height += row->heightHint() + small();
    return std::max(0, height - small());
}

int SharpenCardWidget::minimumWidthHint() const {
    int width = kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
    for (const auto* row : values_)
        width = std::max(width, row->minimumWidthHint());
    return width;
}

QSize SharpenCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void SharpenCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void SharpenCardWidget::place() {
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* row : values_)
        labels = std::max(labels, row->labelWidthHint());
    graph_->setGeometry(0, 0, width(), SharpenProfileGraph::fixedHeight());
    int y = graph_->height() + small();
    for (auto* row : values_) {
        row->setLabelWidth(labels);
        row->setGeometry(0, y, width(), row->heightHint());
        y += row->heightHint() + small();
    }
}

void SharpenCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    for (auto* row : values_)
        row->refresh(locked);
    graph_->setEnabled(!locked);
    graph_->update();
}

} // namespace bloom::ui

#include <bloom/ui/film_grain_widget.hpp>

#include <bloom/core/film_grain_noise.hpp>
#include <bloom/document/film_grain_definition.hpp>
#include <bloom/document/project.hpp>
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

using document::FilmGrainParameterKind;
using document::FilmGrainParameterSpec;
using namespace std::string_view_literals;

// The display curve the ramp is laid out in: equal steps look like equal steps of brightness.
constexpr double kDisplayGamma = 2.2;
// The response the graph's height spans, as the rows' sliders do. A larger value holds at the top.
constexpr double kGraphMaximum = 2.0;
constexpr std::array kHandleRoles{"shadows"sv, "midtones"sv, "highlights"sv};
constexpr std::array kHandleLumas{0.0, document::kFilmGrainMidGrey, document::kFilmGrainWhite};

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] qreal handleRadius() { return kit::px(kit::Size::NodeSocketDot) / 2.0 + 1.0; }

[[nodiscard]] const FilmGrainParameterSpec& specOf(const std::string_view role) {
    for (const auto& spec : document::filmGrainParameters())
        if (spec.role == role)
            return spec;
    return document::filmGrainParameters().front();
}

[[nodiscard]] QString description(const std::string_view role) {
    static const std::array<std::pair<std::string_view, const char*>, 8> kText{{
        {"intensity",
         QT_TRANSLATE_NOOP("FilmGrain", "How strong the grain is, in linear light, where the "
                                        "response is 100%.")},
        {"size", QT_TRANSLATE_NOOP("FilmGrain", "How wide a grain is, in full-resolution pixels.")},
        {"redAmount", QT_TRANSLATE_NOOP("FilmGrain", "How strong the red channel's grain is.")},
        {"greenAmount", QT_TRANSLATE_NOOP("FilmGrain", "How strong the green channel's grain is.")},
        {"blueAmount", QT_TRANSLATE_NOOP("FilmGrain", "How strong the blue channel's grain is.")},
        {"shadows", QT_TRANSLATE_NOOP("FilmGrain", "The share of the grain on black.")},
        {"midtones", QT_TRANSLATE_NOOP("FilmGrain", "The share of the grain at mid grey.")},
        {"highlights",
         QT_TRANSLATE_NOOP("FilmGrain", "The share of the grain at white. Above white, the grain "
                                        "grows with the brightness.")},
    }};
    for (const auto& [name, text] : kText)
        if (name == role)
            return QCoreApplication::translate("FilmGrain", text);
    return {};
}

} // namespace

// --- The response graph -------------------------------------------------------------------------

FilmGrainResponseGraph::FilmGrainResponseGraph(CompositionSession& session,
                                               const document::NodeId node, QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("filmGrainResponseGraph"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAccessibleName(tr("Grain response to brightness"));
    setToolTip(tr("The grain on a ramp from black to white, and how strong it is at each brightness: "
                  "drag a handle up or down to change the shadows, the midtones or the highlights"));
}

int FilmGrainResponseGraph::fixedHeight() {
    // commands::estimatedNodeCardSize holds the same height.
    return 120;
}

QRectF FilmGrainResponseGraph::plotRect() const {
    const qreal inset = handleRadius() + 1.0;
    return QRectF(rect()).adjusted(inset, inset, -inset, -inset);
}

double FilmGrainResponseGraph::scalar(const std::string_view role) const {
    const auto fallback = specOf(role).value;
    const auto id = nodeRoleParameter(session_, node_, role);
    return id ? liveScalar(session_, *id).value_or(fallback) : fallback;
}

QPointF FilmGrainResponseGraph::handlePosition(const std::size_t handle) const {
    const auto plot = plotRect();
    const double x = std::pow(kHandleLumas[handle], 1.0 / kDisplayGamma);
    const double y = std::clamp(scalar(kHandleRoles[handle]) / kGraphMaximum, 0.0, 1.0);
    return {plot.left() + x * plot.width(), plot.bottom() - y * plot.height()};
}

std::optional<std::size_t> FilmGrainResponseGraph::handleAt(const QPointF position) const {
    // The nearest handle within reach of the pointer.
    std::optional<std::size_t> nearest;
    qreal best = (handleRadius() + kit::px(kit::Spacing::S));
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

QImage FilmGrainResponseGraph::preview(const int width, const int height) const {
    const int w = std::max(1, width);
    const int h = std::max(1, height);
    const double intensity = scalar("intensity") * core::film_grain::kNormalise;
    const std::array amounts{intensity * scalar("redAmount"), intensity * scalar("greenAmount"),
                             intensity * scalar("blueAmount")};
    const double shadows = scalar("shadows");
    const double midtones = scalar("midtones");
    const double highlights = scalar("highlights");
    const double size = std::max(scalar("size"), 0.25);
    std::int64_t seed = 1;
    if (const auto id = nodeRoleParameter(session_, node_, "seed"))
        if (const auto live = session_.liveValue(*id))
            if (const auto* stored = std::get_if<std::int64_t>(&*live))
                seed = *stored;
    const auto stream = core::film_grain::stream(seed, 0);
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        const auto py = static_cast<float>((y + 0.5) / size);
        for (int x = 0; x < w; ++x) {
            // The ramp, one preview pixel for one image pixel, so the grain shows at its size.
            const double shown = (x + 0.5) / w;
            const double luma = std::pow(shown, kDisplayGamma);
            const double response = document::filmGrainResponse(luma, shadows, midtones, highlights);
            const auto grain =
                core::film_grain::noise(static_cast<float>((x + 0.5) / size), py, stream);
            std::array<int, 3> rgb{};
            for (std::size_t c = 0; c < 3; ++c) {
                const double value =
                    luma + response * amounts[c] * static_cast<double>(grain[c]);
                rgb[c] = static_cast<int>(std::lround(
                    255.0 * std::pow(std::clamp(value, 0.0, 1.0), 1.0 / kDisplayGamma)));
            }
            line[x] = qRgb(rgb[0], rgb[1], rgb[2]);
        }
    }
    return image;
}

void FilmGrainResponseGraph::paintEvent(QPaintEvent* event) {
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
    const double shadows = scalar("shadows");
    const double midtones = scalar("midtones");
    const double highlights = scalar("highlights");
    QPainterPath curve;
    const int steps = std::max(2, static_cast<int>(plot.width()) / 2);
    for (int step = 0; step <= steps; ++step) {
        const double x = static_cast<double>(step) / steps;
        const double response = document::filmGrainResponse(std::pow(x, kDisplayGamma), shadows,
                                                            midtones, highlights);
        const QPointF point(plot.left() + x * plot.width(),
                            plot.bottom() -
                                std::clamp(response / kGraphMaximum, 0.0, 1.0) * plot.height());
        if (step == 0)
            curve.moveTo(point);
        else
            curve.lineTo(point);
    }
    painter.setBrush(Qt::NoBrush);
    // A dark edge under the line keeps it readable over the white end of the ramp.
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

void FilmGrainResponseGraph::mousePressEvent(QMouseEvent* event) {
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
    drag_ = Drag{*handle, *id, scalar(kHandleRoles[*handle]), event->position().y()};
    update();
}

void FilmGrainResponseGraph::mouseMoveEvent(QMouseEvent* event) {
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
    const double perPixel = kGraphMaximum / std::max(1.0, plot.height());
    drag_->value += (drag_->lastY - event->position().y()) * perPixel *
                    kit::KSlider::dragScale(event->modifiers());
    drag_->lastY = event->position().y();
    const auto& spec = specOf(kHandleRoles[drag_->handle]);
    drag_->value = std::clamp(drag_->value, spec.minimum, spec.maximum);
    if (!session_.updateValueEdit(drag_->value))
        cancelDrag();
    update();
    event->accept();
}

void FilmGrainResponseGraph::mouseReleaseEvent(QMouseEvent* event) {
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

void FilmGrainResponseGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    if (const auto handle = handleAt(event->position()))
        if (const auto id = nodeRoleParameter(session_, node_, kHandleRoles[*handle]))
            static_cast<void>(session_.setParameterValue(
                *id, specOf(kHandleRoles[*handle]).value,
                tr("Reset %1").arg(QString::fromUtf8(specOf(kHandleRoles[*handle]).label.data(),
                                                     static_cast<qsizetype>(
                                                         specOf(kHandleRoles[*handle]).label.size())))));
    event->accept();
}

void FilmGrainResponseGraph::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void FilmGrainResponseGraph::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void FilmGrainResponseGraph::leaveEvent(QEvent* event) {
    if (hovered_) {
        hovered_.reset();
        update();
    }
    QWidget::leaveEvent(event);
}

void FilmGrainResponseGraph::cancelDrag() {
    if (!drag_)
        return;
    const auto id = drag_->parameter;
    drag_.reset();
    if (session_.isValueEditing(id))
        session_.cancelValueEdit();
    update();
}

// --- The card -----------------------------------------------------------------------------------

FilmGrainCardWidget::FilmGrainCardWidget(CompositionSession& session, const document::NodeId node,
                                         const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeFilmGrain") : QStringLiteral("propertiesFilmGrain"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    graph_ = new FilmGrainResponseGraph(session_, node_, this);
    for (const auto& spec : document::filmGrainParameters()) {
        if (spec.kind != FilmGrainParameterKind::Scalar)
            continue;
        const auto schemaKey = document::filmGrainSchemaKey(spec.role);
        auto* row = new GradingValueRow(
            session_, node_, std::string(spec.role), schemaKey,
            QCoreApplication::translate("FilmGrain", std::string(spec.label).c_str()), spec.value,
            this);
        row->setDescription(description(spec.role));
        GradingSliderSpec slider;
        slider.minimum = spec.softMinimum;
        slider.maximum = spec.softMaximum;
        slider.neutral = spec.value;
        slider.ticks = (spec.softMaximum - spec.softMinimum) / 4.0;
        slider.gradient = jogStops();
        row->setSlider(slider);
        values_.push_back(row);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &FilmGrainCardWidget::refresh);
    refresh();
}

GradingValueRow* FilmGrainCardWidget::valueRow(const std::string_view role) const {
    const auto found =
        std::ranges::find_if(values_, [&](const auto* row) { return row->role() == role; });
    return found == values_.end() ? nullptr : *found;
}

bool FilmGrainCardWidget::showsRole(const std::string_view role) const {
    // The animation and the seed are the node's own rows; every value is here.
    return !document::isFilmGrainScalarSchemaKey(document::filmGrainSchemaKey(role));
}

std::optional<int> FilmGrainCardWidget::socketY(const std::string_view role) const {
    if (const auto* row = valueRow(role))
        return row->y() + row->socketY();
    return std::nullopt;
}

int FilmGrainCardWidget::heightAt(const int width) const {
    static_cast<void>(width);
    int height = FilmGrainResponseGraph::fixedHeight() + small();
    for (const auto* row : values_)
        height += row->heightHint() + small();
    return std::max(0, height - small());
}

int FilmGrainCardWidget::minimumWidthHint() const {
    int width = kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
    for (const auto* row : values_)
        width = std::max(width, row->minimumWidthHint());
    return width;
}

QSize FilmGrainCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void FilmGrainCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void FilmGrainCardWidget::place() {
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* row : values_)
        labels = std::max(labels, row->labelWidthHint());
    graph_->setGeometry(0, 0, width(), FilmGrainResponseGraph::fixedHeight());
    int y = graph_->height() + small();
    for (auto* row : values_) {
        row->setLabelWidth(labels);
        row->setGeometry(0, y, width(), row->heightHint());
        y += row->heightHint() + small();
    }
}

void FilmGrainCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    for (auto* row : values_)
        row->refresh(locked);
    graph_->setEnabled(!locked);
    graph_->update();
}

} // namespace bloom::ui

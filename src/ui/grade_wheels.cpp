#include <bloom/ui/grade_wheels.hpp>

#include "properties_value_edits.hpp"
#include "value_presentation.hpp"

#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QConicalGradient>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bloom::ui {
namespace {

using document::GradePrimary;
using document::GradeWheel;

// The vectorscope places red at 103 degrees; the wheels follow it so a puck pushed toward red
// points where a scope shows red.
constexpr double kRedDegrees = 103.0;
constexpr double kRadians = std::numbers::pi / 180.0;
// An orthonormal basis of the chroma plane: e1 the chroma of red, e2 green minus blue.
const double kE1Scale = 1.0 / std::sqrt(6.0);
const double kE2Scale = 1.0 / std::sqrt(2.0);

[[nodiscard]] std::array<double, 3> e1() { return {2.0 * kE1Scale, -kE1Scale, -kE1Scale}; }
[[nodiscard]] std::array<double, 3> e2() { return {0.0, kE2Scale, -kE2Scale}; }

[[nodiscard]] std::optional<document::ParameterId> roleParameter(const CompositionSession& session,
                                                                 const document::NodeId node,
                                                                 const std::string_view role) {
    const auto* composition = session.composition();
    const auto* record = composition ? composition->graph().findNode(node) : nullptr;
    if (record == nullptr)
        return std::nullopt;
    const auto binding =
        std::ranges::find(record->parameters, role, &document::ParameterBinding::role);
    if (binding == record->parameters.end())
        return std::nullopt;
    return binding->parameterId;
}

[[nodiscard]] std::optional<double> scalarNow(const CompositionSession& session,
                                              const document::ParameterId parameter) {
    const auto value = session.liveValue(parameter);
    const auto* scalar = value ? std::get_if<double>(&*value) : nullptr;
    return scalar ? std::optional(*scalar) : std::nullopt;
}

[[nodiscard]] bool editable(const CompositionSession& session, const document::ParameterId id) {
    const auto* composition = session.composition();
    const auto* parameter = composition ? composition->parameters().find(id) : nullptr;
    return parameter != nullptr && !composition->parameterLocked(id) &&
           !std::holds_alternative<document::DriverBindingSource>(parameter->source);
}

[[nodiscard]] QString primaryName(const GradePrimary primary) {
    switch (primary) {
    case GradePrimary::Temperature:
        return QObject::tr("Temperature");
    case GradePrimary::Tint:
        return QObject::tr("Tint");
    case GradePrimary::Contrast:
        return QObject::tr("Contrast");
    case GradePrimary::Pivot:
        return QObject::tr("Pivot");
    case GradePrimary::Saturation:
        return QObject::tr("Saturation");
    case GradePrimary::Exposure:
        return QObject::tr("Exposure");
    }
    return {};
}

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int gap() { return kit::px(kit::Spacing::S); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

} // namespace

double gradeWheelRange(const GradeWheel wheel) noexcept {
    switch (wheel) {
    case GradeWheel::Lift:
        return 0.25;
    case GradeWheel::Gamma:
    case GradeWheel::Gain:
        return 0.5;
    case GradeWheel::Offset:
        break;
    }
    return 0.1;
}

QString gradeWheelName(const GradeWheel wheel) {
    switch (wheel) {
    case GradeWheel::Lift:
        return QObject::tr("Lift");
    case GradeWheel::Gamma:
        return QObject::tr("Gamma");
    case GradeWheel::Gain:
        return QObject::tr("Gain");
    case GradeWheel::Offset:
        break;
    }
    return QObject::tr("Offset");
}

QPointF gradeWheelPuck(const GradeWheel wheel, const std::array<double, 3>& rgb) {
    const double neutral = document::gradeWheelDomain(wheel).neutral;
    std::array<double, 3> d{rgb[0] - neutral, rgb[1] - neutral, rgb[2] - neutral};
    const double mean = (d[0] + d[1] + d[2]) / 3.0;
    for (auto& value : d)
        value -= mean;
    const auto a = e1();
    const auto b = e2();
    const double u = d[0] * a[0] + d[1] * a[1] + d[2] * a[2];
    const double v = d[0] * b[0] + d[1] * b[1] + d[2] * b[2];
    const double radius = std::hypot(u, v) / gradeWheelRange(wheel);
    if (radius == 0.0)
        return {};
    const double angle = std::atan2(v, u) + kRedDegrees * kRadians;
    return {radius * std::cos(angle), radius * std::sin(angle)};
}

std::array<double, 3> gradeWheelColour(const GradeWheel wheel, const QPointF puck,
                                       const std::array<double, 3>& current) {
    const auto domain = document::gradeWheelDomain(wheel);
    const double grey = (current[0] + current[1] + current[2]) / 3.0;
    const double radius = std::hypot(puck.x(), puck.y()) * gradeWheelRange(wheel);
    const double angle = std::atan2(puck.y(), puck.x()) - kRedDegrees * kRadians;
    const double u = radius * std::cos(angle);
    const double v = radius * std::sin(angle);
    const auto a = e1();
    const auto b = e2();
    std::array<double, 3> rgb{};
    for (std::size_t channel = 0; channel < 3; ++channel)
        rgb[channel] =
            std::clamp(grey + u * a[channel] + v * b[channel], domain.minimum, domain.maximum);
    return rgb;
}

std::array<double, 3> gradeWheelHueColour(const double degrees) {
    const double angle = (degrees - kRedDegrees) * kRadians;
    const auto a = e1();
    const auto b = e2();
    std::array<double, 3> chroma{};
    double peak = 0.0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        chroma[channel] = std::cos(angle) * a[channel] + std::sin(angle) * b[channel];
        peak = std::max(peak, std::abs(chroma[channel]));
    }
    std::array<double, 3> rgb{};
    for (std::size_t channel = 0; channel < 3; ++channel)
        rgb[channel] = 0.5 + 0.45 * chroma[channel] / peak;
    return rgb;
}

// --- GradeWheelCanvas ---------------------------------------------------------------------------

GradeWheelCanvas::GradeWheelCanvas(CompositionSession& session, const document::NodeId node,
                                   const GradeWheel wheel, QWidget* parent)
    : QWidget(parent), session_(session), node_(node), wheel_(wheel) {
    setObjectName(QStringLiteral("gradeWheel.%1")
                      .arg(QString::fromUtf8(
                          document::kGradeWheelNames[static_cast<std::size_t>(wheel)].data())));
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("%1 wheel").arg(gradeWheelName(wheel)));
    setToolTip(tr("%1: drag to shift the colour (Shift for fine), double-click to reset, "
                  "Alt-click for its values")
                   .arg(gradeWheelName(wheel)));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, qOverload<>(&QWidget::update));
}

std::optional<std::array<document::ParameterId, 4>> GradeWheelCanvas::parameters() const {
    std::array<document::ParameterId, 4> ids{};
    for (std::size_t component = 0; component < ids.size(); ++component) {
        const auto id = roleParameter(session_, node_, document::gradeWheelRole(wheel_, component));
        if (!id)
            return std::nullopt;
        ids[component] = *id;
    }
    return ids;
}

std::optional<std::array<double, 4>> GradeWheelCanvas::values() const {
    const auto ids = parameters();
    if (!ids)
        return std::nullopt;
    std::array<double, 4> values{};
    for (std::size_t component = 0; component < values.size(); ++component) {
        const auto value = scalarNow(session_, (*ids)[component]);
        if (!value)
            return std::nullopt;
        values[component] = *value;
    }
    return values;
}

QRectF GradeWheelCanvas::discRect() const {
    const QFontMetricsF metrics(kit::font(kit::TypeRole::UiSmall));
    const qreal title = metrics.height() + kit::px(kit::Spacing::XXS);
    const qreal side = std::max<qreal>(0.0, std::min<qreal>(width(), height() - title)) -
                       kit::px(kit::Spacing::XS);
    return {(width() - side) / 2.0, title + (height() - title - side) / 2.0, side, side};
}

QPointF GradeWheelCanvas::puckPosition() const {
    const auto disc = discRect();
    const auto current = values();
    QPointF puck;
    if (current)
        puck = gradeWheelPuck(wheel_, {(*current)[1], (*current)[2], (*current)[3]});
    const qreal length = std::hypot(puck.x(), puck.y());
    if (length > 1.0)
        puck /= length;
    const qreal radius = disc.width() / 2.0;
    return {disc.center().x() + puck.x() * radius, disc.center().y() - puck.y() * radius};
}

void GradeWheelCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(kit::font(kit::TypeRole::UiSmall));
    painter.setPen(kit::color(hasFocus() ? kit::Color::Foreground : kit::Color::Muted));
    painter.drawText(QRectF(0.0, 0.0, width(), discRect().top()), Qt::AlignLeft | Qt::AlignTop,
                     gradeWheelName(wheel_));
    const auto disc = discRect();
    if (disc.width() <= 0.0)
        return;
    // The hue ring, faded toward the grey centre.
    QConicalGradient hues(disc.center(), 0.0);
    constexpr int stops = 36;
    for (int stop = 0; stop <= stops; ++stop) {
        const double t = static_cast<double>(stop) / stops;
        const auto rgb = gradeWheelHueColour(t * 360.0);
        hues.setColorAt(t, QColor::fromRgbF(static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                                            static_cast<float>(rgb[2])));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(hues);
    painter.drawEllipse(disc);
    QRadialGradient fade(disc.center(), disc.width() / 2.0);
    QColor centre = kit::color(kit::Color::SurfaceSunken);
    fade.setColorAt(0.0, centre);
    centre.setAlphaF(0.15F);
    fade.setColorAt(1.0, centre);
    painter.setBrush(fade);
    painter.drawEllipse(disc);
    painter.setBrush(Qt::NoBrush);
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawEllipse(disc);
    painter.drawLine(QPointF(disc.left(), disc.center().y()),
                     QPointF(disc.right(), disc.center().y()));
    painter.drawLine(QPointF(disc.center().x(), disc.top()),
                     QPointF(disc.center().x(), disc.bottom()));
    // The puck, joined to the centre.
    const auto puck = puckPosition();
    painter.setPen(QPen(kit::color(kit::Color::Foreground), kit::kHairlineWidth));
    painter.drawLine(disc.center(), puck);
    const qreal dot = kit::px(kit::Size::NodeSocketDot) / 2.0;
    painter.setPen(QPen(kit::color(drag_ ? kit::Color::Accent : kit::Color::Foreground),
                        kit::kEmphasisStroke));
    painter.setBrush(kit::color(kit::Color::SurfaceSunken));
    painter.drawEllipse(puck, dot, dot);
}

void GradeWheelCanvas::mousePressEvent(QMouseEvent* event) {
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
    const auto ids = parameters();
    const auto current = values();
    if (!ids || !current ||
        !session_.beginValueEdits({(*ids)[1], (*ids)[2], (*ids)[3]},
                                  tr("Set %1 Colour").arg(gradeWheelName(wheel_))))
        return;
    const std::array<double, 3> base{(*current)[1], (*current)[2], (*current)[3]};
    drag_ = Drag{event->position(), gradeWheelPuck(wheel_, base), base, (*ids)[1]};
    update();
}

void GradeWheelCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_)
        return;
    if (!session_.isValueEditing(drag_->primary)) {
        drag_.reset();
        update();
        return;
    }
    const qreal radius = std::max<qreal>(1.0, discRect().width() / 2.0);
    const qreal speed = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 1.0;
    const QPointF delta = (event->position() - drag_->pressed) * (speed / radius);
    QPointF puck(drag_->puck.x() + delta.x(), drag_->puck.y() - delta.y());
    // The rim stops a drag, unless the values already sat past it.
    const qreal limit = std::max<qreal>(1.0, std::hypot(drag_->puck.x(), drag_->puck.y()));
    if (const qreal length = std::hypot(puck.x(), puck.y()); length > limit)
        puck *= limit / length;
    const auto rgb = gradeWheelColour(wheel_, puck, drag_->base);
    if (!session_.updateValueEdits({rgb[0], rgb[1], rgb[2]}))
        cancelDrag();
    event->accept();
}

void GradeWheelCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool owned = session_.isValueEditing(drag_->primary);
    drag_.reset();
    // One command for the gesture; a press that moved nothing commits nothing.
    if (owned)
        static_cast<void>(session_.commitValueEdit());
    update();
    event->accept();
}

void GradeWheelCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    static_cast<void>(reset());
    event->accept();
}

void GradeWheelCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void GradeWheelCanvas::focusOutEvent(QFocusEvent* event) {
    cancelDrag();
    QWidget::focusOutEvent(event);
}

void GradeWheelCanvas::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void GradeWheelCanvas::cancelDrag() {
    if (!drag_)
        return;
    const auto primary = drag_->primary;
    drag_.reset();
    if (session_.isValueEditing(primary))
        session_.cancelValueEdit();
    update();
}

bool GradeWheelCanvas::reset() {
    const auto ids = parameters();
    const auto current = values();
    if (!ids || !current)
        return false;
    const double neutral = document::gradeWheelDomain(wheel_).neutral;
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> changes;
    for (std::size_t component = 0; component < ids->size(); ++component)
        if ((*current)[component] != neutral)
            changes.emplace_back((*ids)[component], neutral);
    if (changes.empty())
        return true;
    return session_.setParameterValues(std::move(changes),
                                       tr("Reset %1").arg(gradeWheelName(wheel_)));
}

// --- GradeCardWidget ----------------------------------------------------------------------------

GradeCardWidget::GradeCardWidget(CompositionSession& session, const document::NodeId node,
                                 const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeGradeWheels")
                       : QStringLiteral("propertiesGradeWheels"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        const auto wheel = document::kGradeWheels[index];
        auto& cell = cells_[index];
        cell.canvas = new GradeWheelCanvas(session_, node_, wheel, this);
        connect(cell.canvas, &GradeWheelCanvas::alternateClicked, this,
                [this, index] { setExpanded(index, !expanded_[index]); });
        cell.master = new kit::KSlider(this);
        cell.master->setObjectName(QStringLiteral("gradeWheelMaster"));
        cell.master->setAccessibleName(tr("%1 master").arg(gradeWheelName(wheel)));
        cell.master->setToolTip(tr("%1 master").arg(gradeWheelName(wheel)));
        const auto masterKey = document::gradeWheelSchemaKey(wheel, 0);
        configureSlider(*cell.master, document::parameterPresentation(
                                          masterKey, document::ParameterValueKind::Float64,
                                          document::gradeWheelRole(wheel, 0)));
        const auto masterRole = document::gradeWheelRole(wheel, 0);
        properties::bindValueEdit(session_, *cell.master, [this, masterRole] {
            return parameter(masterRole).value_or(document::ParameterId{});
        });
        connect(cell.master, &kit::KSlider::valueChanged, this,
                [this, masterRole, wheel](const double value) {
                    if (const auto id = parameter(masterRole))
                        static_cast<void>(session_.setParameterValue(
                            *id, value, tr("Set %1 Master").arg(gradeWheelName(wheel))));
                });
        cell.reset = new kit::KButton(kit::IconId::Reset, QString{}, this);
        cell.reset->setObjectName(QStringLiteral("gradeWheelReset"));
        cell.reset->setControlSize(kit::KButton::ControlSize::Compact);
        cell.reset->setAccessibleName(tr("Reset %1").arg(gradeWheelName(wheel)));
        cell.reset->setToolTip(tr("Reset %1").arg(gradeWheelName(wheel)));
        connect(cell.reset, &QAbstractButton::clicked, cell.canvas,
                [canvas = cell.canvas] { static_cast<void>(canvas->reset()); });
        auto* disclosure = new kit::KButton(kit::IconId::CaretRight, QString{}, this);
        disclosure->setVariant(kit::KButton::Variant::Ghost);
        disclosure->setControlSize(kit::KButton::ControlSize::Compact);
        disclosure->setObjectName(QStringLiteral("gradeWheelValues"));
        disclosure->setAccessibleName(tr("%1 values").arg(gradeWheelName(wheel)));
        disclosure->setToolTip(tr("Show the %1 values").arg(gradeWheelName(wheel)));
        connect(disclosure, &QAbstractButton::clicked, this,
                [this, index] { setExpanded(index, !expanded_[index]); });
        cell.values = disclosure;
    }
    primariesTitle_ = new kit::KLabel(tr("Primaries"), this, kit::TypeRole::UiSmall);
    primariesTitle_->setTone(kit::Color::Muted);
    auto* disclosure = new kit::KButton(kit::IconId::CaretRight, QString{}, this);
    disclosure->setVariant(kit::KButton::Variant::Ghost);
    disclosure->setControlSize(kit::KButton::ControlSize::Compact);
    disclosure->setObjectName(QStringLiteral("gradePrimariesValues"));
    disclosure->setAccessibleName(tr("Primaries values"));
    disclosure->setToolTip(tr("Show the primaries' rows, to key or link them"));
    connect(disclosure, &QAbstractButton::clicked, this,
            [this] { setExpanded(expanded_.size() - 1, !expanded_.back()); });
    primariesValues_ = disclosure;
    for (std::size_t index = 0; index < primaries_.size(); ++index) {
        const auto which = document::kGradePrimaries[index];
        const auto role = std::string(document::gradePrimaryRole(which));
        auto* field = new kit::KValueField(this);
        field->setObjectName(QStringLiteral("gradePrimary.%1").arg(QString::fromStdString(role)));
        field->setCompact(true);
        // #499: the field takes the rest of its row, so a long value is not cut short either.
        field->setMaximumWidth(QWIDGETSIZE_MAX);
        field->setOutlinedAtRest(true);
        configureValueField(*field, document::gradePrimarySchemaKey(which),
                            document::ParameterValueKind::Float64, role);
        auto* label = new kit::KLabel(primaryName(which), this, kit::TypeRole::UiSmall);
        label->setObjectName(
            QStringLiteral("gradePrimaryLabel.%1").arg(QString::fromStdString(role)));
        label->setTone(kit::Color::Muted);
        primaryLabels_[index] = label;
        field->setAccessibleName(primaryName(which));
        field->setToolTip(primaryName(which));
        field->setDefaultValue(document::gradePrimaryDomain(which).neutral);
        properties::bindValueEdit(session_, *field, [this, role] {
            return parameter(role).value_or(document::ParameterId{});
        });
        connect(field, &kit::KValueField::valueChanged, this, [this, role, which](double value) {
            if (const auto id = parameter(role))
                static_cast<void>(
                    session_.setParameterValue(*id, value, tr("Set %1").arg(primaryName(which))));
        });
        primaries_[index] = field;
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &GradeCardWidget::refresh);
    refresh();
}

std::optional<document::ParameterId> GradeCardWidget::parameter(const std::string_view role) const {
    return roleParameter(session_, node_, role);
}

GradeWheelCanvas* GradeCardWidget::canvas(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].canvas;
}
kit::KSlider* GradeCardWidget::master(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].master;
}
kit::KButton* GradeCardWidget::resetButton(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].reset;
}
kit::KValueField* GradeCardWidget::primary(const GradePrimary which) const {
    return primaries_[static_cast<std::size_t>(which)];
}

bool GradeCardWidget::showsRole(const std::string_view role) const {
    for (std::size_t index = 0; index < cells_.size(); ++index)
        for (std::size_t component = 0; component < document::kGradeWheelComponentCount;
             ++component)
            if (role == document::gradeWheelRole(document::kGradeWheels[index], component))
                return expanded_[index];
    return expanded_.back();
}

void GradeCardWidget::setValuesExpanded(const GradeWheel wheel, const bool expanded) {
    setExpanded(static_cast<std::size_t>(wheel), expanded);
}
bool GradeCardWidget::valuesExpanded(const GradeWheel wheel) const {
    return expanded_[static_cast<std::size_t>(wheel)];
}
void GradeCardWidget::setPrimariesExpanded(const bool expanded) {
    setExpanded(expanded_.size() - 1, expanded);
}

void GradeCardWidget::setExpanded(const std::size_t group, const bool expanded) {
    if (expanded_[group] == expanded)
        return;
    expanded_[group] = expanded;
    auto* button = group < cells_.size() ? cells_[group].values : primariesValues_;
    button->setIconId(expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    emit rowsChanged();
}

int GradeCardWidget::minimumWidthHint() const {
    return 2 * (kit::px(kit::Size::ImageThumbnail) + control()) + gap();
}

int GradeCardWidget::heightAt(const int width) const {
    const int cell = std::max(0, (width - gap()) / 2);
    const int wheels = 2 * (cell + small() + control()) + gap();
    const int primaries = (1 + static_cast<int>(document::kGradePrimaryCount)) * control() +
                          static_cast<int>(document::kGradePrimaryCount) * small();
    return wheels + gap() + primaries;
}

QSize GradeCardWidget::sizeHint() const {
    const int width = kit::px(kit::Size::DropdownWidthExpanded) + gap();
    return {width, heightAt(width)};
}

void GradeCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void GradeCardWidget::place() {
    const int w = width();
    const int cell = std::max(0, (w - gap()) / 2);
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        const int x = static_cast<int>(index % 2) * (cell + gap());
        const int y = static_cast<int>(index / 2) * (cell + small() + control() + gap());
        auto& c = cells_[index];
        c.canvas->setGeometry(x, y, cell, cell);
        const int row = y + cell + small();
        const int slider = std::max(0, cell - 2 * (control() + small()));
        c.master->setGeometry(x, row, slider, control());
        c.reset->setGeometry(x + slider + small(), row, control(), control());
        c.values->setGeometry(x + slider + control() + 2 * small(), row, control(), control());
    }
    int y = 2 * (cell + small() + control()) + gap();
    primariesTitle_->setGeometry(0, y, std::max(0, w - control()), control());
    primariesValues_->setGeometry(std::max(0, w - control()), y, control(), control());
    y += control() + small();
    // #499: one primary per row, its whole name in a label column measured from the widest name
    // and the field taking the rest of the row, so no name or value is cut short.
    int labels = 0;
    for (const auto* label : primaryLabels_)
        labels = std::max(labels, label->sizeHint().width());
    labels += small();
    for (std::size_t index = 0; index < primaries_.size(); ++index) {
        const int top = y + static_cast<int>(index) * (control() + small());
        primaryLabels_[index]->setGeometry(0, top, labels, control());
        primaries_[index]->setGeometry(labels + small(), top, std::max(0, w - labels - small()),
                                       control());
    }
}

void GradeCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool nodeLocked = composition == nullptr || composition->nodeLocked(node_);
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        const auto wheel = document::kGradeWheels[index];
        auto& cell = cells_[index];
        const auto ids = cell.canvas->parameters();
        bool colour = ids.has_value() && !nodeLocked;
        if (ids)
            for (std::size_t component = 1; component < ids->size(); ++component)
                colour = colour && editable(session_, (*ids)[component]);
        cell.canvas->setEnabled(colour);
        const auto master = parameter(document::gradeWheelRole(wheel, 0));
        cell.master->setEnabled(master && !nodeLocked && editable(session_, *master));
        cell.reset->setEnabled(!nodeLocked && ids.has_value());
        if (master && !cell.master->hasLiveEdit())
            if (const auto value = scalarNow(session_, *master)) {
                const QSignalBlocker blocker(cell.master);
                cell.master->setValue(*value);
            }
        cell.canvas->update();
    }
    for (std::size_t index = 0; index < primaries_.size(); ++index) {
        auto* field = primaries_[index];
        const auto id = parameter(document::gradePrimaryRole(document::kGradePrimaries[index]));
        field->setEnabled(id && !nodeLocked && editable(session_, *id));
        if (id && !field->hasLiveEdit())
            if (const auto value = scalarNow(session_, *id)) {
                const QSignalBlocker blocker(field);
                field->setValue(*value);
            }
    }
}

} // namespace bloom::ui

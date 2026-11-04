#include <bloom/ui/grade_wheels.hpp>

#include "properties_value_edits.hpp"
#include "value_presentation.hpp"

#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QConicalGradient>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <memory>
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
// The donut's hole, as a share of its radius.
constexpr double kHoleShare = 0.5;
// The disc's diameter as a share of its cell, and the jog strip's width as a share of the disc.
constexpr double kDiscShare = 0.8;
constexpr double kJogShare = 0.88;
// An arrow key nudges the puck this share of the radius (Ctrl a tenth, Shift ten times).
constexpr double kNudge = 0.02;
// Ctrl (Cmd) drags at this share of the pointer's speed.
constexpr double kFineScale = 0.1;

[[nodiscard]] std::array<double, 3> e1() { return {2.0 * kE1Scale, -kE1Scale, -kE1Scale}; }
[[nodiscard]] std::array<double, 3> e2() { return {0.0, kE2Scale, -kE2Scale}; }

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

[[nodiscard]] QString primaryDescription(const GradePrimary primary) {
    switch (primary) {
    case GradePrimary::Temperature:
        return QObject::tr("Positive is warmer (amber), negative cooler (blue).");
    case GradePrimary::Tint:
        return QObject::tr("Positive is more magenta, negative more green.");
    case GradePrimary::Contrast:
        return QObject::tr("1 leaves the image as it is; above 1 spreads values away from the "
                           "pivot, below 1 draws them toward it.");
    case GradePrimary::Pivot:
        return QObject::tr("The value contrast turns around; 0.18 is middle grey.");
    case GradePrimary::Saturation:
        return QObject::tr("0 is grey, 1 leaves the colour as it is, above 1 is more vivid.");
    case GradePrimary::Exposure:
        return QObject::tr("In stops: +1 doubles the light, -1 halves it.");
    }
    return {};
}

[[nodiscard]] QString channelName(const std::size_t channel) {
    switch (channel) {
    case 0:
        return QObject::tr("red");
    case 1:
        return QObject::tr("green");
    default:
        return QObject::tr("blue");
    }
}

[[nodiscard]] kit::Color channelInk(const std::size_t channel) {
    switch (channel) {
    case 0:
        return kit::Color::ComponentX;
    case 1:
        return kit::Color::ComponentY;
    default:
        return kit::Color::ComponentZ;
    }
}

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int gap() { return kit::px(kit::Spacing::S); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] qreal puckRadius() { return kit::px(kit::Size::NodeSocketDot) / 2.0; }

[[nodiscard]] double modifierScale(const Qt::KeyboardModifiers modifiers) {
    return modifiers.testFlag(Qt::ControlModifier) ? kFineScale : 1.0;
}

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

QString gradeWheelLevelDescription(const GradeWheel wheel) {
    switch (wheel) {
    case GradeWheel::Lift:
        return QObject::tr("Raises or lowers black; white stays.");
    case GradeWheel::Gamma:
        return QObject::tr("Brightens or darkens midtones; black and white stay.");
    case GradeWheel::Gain:
        return QObject::tr("Scales the image; black stays.");
    case GradeWheel::Offset:
        break;
    }
    return QObject::tr("Adds to every value.");
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
    setMouseTracking(true);
    setAccessibleName(tr("%1 colour").arg(gradeWheelName(wheel)));
    setToolTip(tr("%1 colour").arg(gradeWheelName(wheel)));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, qOverload<>(&QWidget::update));
}

QSize GradeWheelCanvas::sizeHint() const {
    const int side = kit::px(kit::Size::ImageThumbnail) + control();
    return {side, side};
}

std::optional<std::array<document::ParameterId, 4>> GradeWheelCanvas::parameters() const {
    std::array<document::ParameterId, 4> ids{};
    for (std::size_t component = 0; component < ids.size(); ++component) {
        const auto id =
            nodeRoleParameter(session_, node_, document::gradeWheelRole(wheel_, component));
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
        const auto value = liveScalar(session_, (*ids)[component]);
        if (!value)
            return std::nullopt;
        values[component] = *value;
    }
    return values;
}

QRectF GradeWheelCanvas::discRect() const {
    // The puck at the rim, and its emphasis stroke, stay inside the widget.
    const qreal margin = puckRadius() + kit::kEmphasisStroke;
    const qreal side = std::max<qreal>(0.0, std::min<qreal>(width(), height()) - 2.0 * margin);
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

bool GradeWheelCanvas::onDisc(const QPointF position) const {
    const auto disc = discRect();
    const QPointF delta = position - disc.center();
    return std::hypot(delta.x(), delta.y()) <= disc.width() / 2.0 + puckRadius();
}

bool GradeWheelCanvas::neutral() const {
    const auto current = values();
    if (!current)
        return true;
    const auto puck = gradeWheelPuck(wheel_, {(*current)[1], (*current)[2], (*current)[3]});
    return std::hypot(puck.x(), puck.y()) < 1e-9;
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
    const auto disc = discRect();
    if (disc.width() <= 0.0)
        return;
    const qreal radius = disc.width() / 2.0;
    const bool enabled = isEnabled();
    painter.setOpacity(enabled ? 1.0 : kit::kDisabledOpacity);
    QConicalGradient hues(disc.center(), 0.0);
    constexpr int stops = 36;
    for (int stop = 0; stop <= stops; ++stop) {
        const double t = static_cast<double>(stop) / stops;
        const auto rgb = gradeWheelHueColour(t * 360.0);
        hues.setColorAt(t, QColor::fromRgbF(static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                                            static_cast<float>(rgb[2])));
    }
    const QRectF hole(disc.center().x() - radius * kHoleShare,
                      disc.center().y() - radius * kHoleShare, 2.0 * radius * kHoleShare,
                      2.0 * radius * kHoleShare);
    // The hue ring.
    QPainterPath ring;
    ring.addEllipse(disc);
    ring.addEllipse(hole);
    ring.setFillRule(Qt::OddEvenFill);
    painter.setPen(Qt::NoPen);
    painter.setBrush(hues);
    painter.drawPath(ring);
    // The centre: empty while the colour is neutral, the faded hues once it is not.
    painter.setBrush(kit::color(kit::Color::SurfaceSunken));
    painter.drawEllipse(hole);
    const bool adjusted = !neutral();
    if (adjusted) {
        painter.setBrush(hues);
        painter.drawEllipse(hole);
        QRadialGradient fade(disc.center(), radius * kHoleShare);
        QColor centre = kit::color(kit::Color::SurfaceSunken);
        fade.setColorAt(0.0, centre);
        centre.setAlphaF(0.45F);
        fade.setColorAt(1.0, centre);
        painter.setBrush(fade);
        painter.drawEllipse(hole);
    }
    painter.setOpacity(1.0);
    painter.setBrush(Qt::NoBrush);
    // The rim, brighter under the pointer, Accent while dragging or focused.
    const auto rim = drag_ ? kit::Color::Accent
                     : hasFocus() ? kit::Color::Accent
                     : hovered_   ? kit::Color::BorderHover
                                  : kit::Color::Border;
    kit::applyHairlinePen(painter, kit::color(rim));
    painter.drawEllipse(disc);
    // The puck, joined to the centre once it has left it.
    const auto puck = puckPosition();
    if (adjusted) {
        painter.setPen(QPen(kit::color(kit::Color::Foreground), kit::kHairlineWidth));
        painter.drawLine(disc.center(), puck);
    }
    painter.setPen(QPen(kit::color(drag_ ? kit::Color::Accent : kit::Color::Foreground),
                        hovered_ || drag_ ? kit::kEmphasisStroke + 0.5 : kit::kEmphasisStroke));
    painter.setBrush(kit::color(kit::Color::SurfaceSunken));
    painter.drawEllipse(puck, puckRadius(), puckRadius());
}

bool GradeWheelCanvas::beginDrag(const bool keys) {
    const auto ids = parameters();
    const auto current = values();
    if (!ids || !current ||
        !session_.beginValueEdits({(*ids)[1], (*ids)[2], (*ids)[3]},
                                  tr("Set %1 Colour").arg(gradeWheelName(wheel_))))
        return false;
    Drag drag;
    drag.base = {(*current)[1], (*current)[2], (*current)[3]};
    drag.puck = gradeWheelPuck(wheel_, drag.base);
    drag.at = drag.puck;
    drag.limit = std::max(1.0, std::hypot(drag.puck.x(), drag.puck.y()));
    drag.primary = (*ids)[1];
    drag.keys = keys;
    drag_ = drag;
    return true;
}

void GradeWheelCanvas::moveTo(QPointF puck) {
    // The rim stops a drag, unless the values already sat past it.
    if (const qreal length = std::hypot(puck.x(), puck.y()); length > drag_->limit)
        puck *= drag_->limit / length;
    drag_->at = puck;
    const auto rgb = gradeWheelColour(wheel_, puck, drag_->base);
    if (!session_.updateValueEdits({rgb[0], rgb[1], rgb[2]}))
        cancelDrag();
}

void GradeWheelCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    // Only a press on the disc starts a drag; the corners belong to the card.
    if (event->button() != Qt::LeftButton || !isEnabled() || !onDisc(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    event->accept();
    if (event->modifiers().testFlag(Qt::AltModifier)) {
        emit alternateClicked();
        return;
    }
    if (drag_ && drag_->keys)
        static_cast<void>(session_.commitValueEdit()), drag_.reset();
    if (!beginDrag(false))
        return;
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    drag_->from = event->position();
    drag_->modifiers = modifiers;
    drag_->scale = modifierScale(modifiers);
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        // A Shift-press jumps the puck to the pointer, which it then follows 1:1.
        const auto disc = discRect();
        const qreal radius = std::max<qreal>(1.0, disc.width() / 2.0);
        const QPointF at((event->position().x() - disc.center().x()) / radius,
                         (disc.center().y() - event->position().y()) / radius);
        drag_->limit = 1.0;
        moveTo(at);
        if (drag_) {
            drag_->puck = drag_->at;
            drag_->scale = 1.0;
        }
    }
    update();
}

void GradeWheelCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_ || drag_->keys) {
        const bool over = isEnabled() && onDisc(event->position());
        if (over != hovered_) {
            hovered_ = over;
            setCursor(over ? Qt::CrossCursor : Qt::ArrowCursor);
            update();
        }
        if (!drag_)
            return;
    }
    if (drag_->keys)
        return;
    if (!session_.isValueEditing(drag_->primary)) {
        drag_.reset();
        update();
        return;
    }
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers != drag_->modifiers) {
        // A modifier change continues from where the puck is, so it never jumps.
        drag_->from = event->position();
        drag_->puck = drag_->at;
        drag_->modifiers = modifiers;
        drag_->scale = modifierScale(modifiers);
    }
    const qreal radius = std::max<qreal>(1.0, discRect().width() / 2.0);
    const QPointF delta = (event->position() - drag_->from) * (drag_->scale / radius);
    moveTo(QPointF(drag_->puck.x() + delta.x(), drag_->puck.y() - delta.y()));
    event->accept();
}

void GradeWheelCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_ || drag_->keys) {
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
    if (event->button() != Qt::LeftButton || !onDisc(event->position())) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    // A double-click on the disc resets its colour; the level is its own control.
    static_cast<void>(resetColour());
    event->accept();
}

QMenu* GradeWheelCanvas::makeContextMenu() {
    auto* menu = kit::makeMenu(this);
    const auto current = values();
    const double neutralValue = document::gradeWheelDomain(wheel_).neutral;
    auto* colour = menu->addAction(tr("Reset %1 Colour").arg(gradeWheelName(wheel_)));
    colour->setObjectName(QStringLiteral("gradeWheelResetColour"));
    colour->setEnabled(isEnabled() && !neutral());
    connect(colour, &QAction::triggered, this, [this] { static_cast<void>(resetColour()); });
    auto* all = menu->addAction(tr("Reset %1").arg(gradeWheelName(wheel_)));
    all->setObjectName(QStringLiteral("gradeWheelReset"));
    all->setEnabled(isEnabled() && current &&
                    std::ranges::any_of(*current, [&](const double value) {
                        return value != neutralValue;
                    }));
    connect(all, &QAction::triggered, this, [this] { static_cast<void>(reset()); });
    if (const auto ids = parameters()) {
        std::size_t keyable = 0;
        std::size_t keyed = 0;
        for (const auto id : *ids) {
            const auto state = session_.keyframeDiamondStateForParameter(id);
            keyable += state != KeyframeDiamondState::Unsupported ? 1U : 0U;
            keyed += state == KeyframeDiamondState::AnimatedWithKey ? 1U : 0U;
        }
        if (keyable != 0) {
            menu->addSeparator();
            auto* key = menu->addAction(keyed == keyable ? tr("Remove %1 Keys").arg(gradeWheelName(wheel_))
                                                         : tr("Key %1").arg(gradeWheelName(wheel_)));
            key->setObjectName(QStringLiteral("gradeWheelKey"));
            connect(key, &QAction::triggered, this, &GradeWheelCanvas::keyRequested);
        }
    }
    auto* values = menu->addAction(tr("Show Values"));
    values->setObjectName(QStringLiteral("gradeWheelShowValues"));
    connect(values, &QAction::triggered, this, &GradeWheelCanvas::alternateClicked);
    return menu;
}

void GradeWheelCanvas::contextMenuEvent(QContextMenuEvent* event) {
    if (drag_ || !onDisc(event->pos())) {
        event->accept();
        return;
    }
    const std::unique_ptr<QMenu> menu(makeContextMenu());
    menu->exec(event->globalPos());
    event->accept();
}

void GradeWheelCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QPointF step;
    switch (event->key()) {
    case Qt::Key_Left:
        step = {-1.0, 0.0};
        break;
    case Qt::Key_Right:
        step = {1.0, 0.0};
        break;
    case Qt::Key_Up:
        step = {0.0, 1.0};
        break;
    case Qt::Key_Down:
        step = {0.0, -1.0};
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    if (!isEnabled() || (drag_ && !drag_->keys)) {
        event->accept();
        return;
    }
    // A run of arrow keys nudges the puck as one edit, committed when the key is released.
    if (!drag_ && !beginDrag(true)) {
        event->accept();
        return;
    }
    const double scale = event->modifiers().testFlag(Qt::ShiftModifier)     ? 10.0
                         : event->modifiers().testFlag(Qt::ControlModifier) ? kFineScale
                                                                            : 1.0;
    moveTo(drag_->at + step * (kNudge * scale));
    update();
    event->accept();
}

void GradeWheelCanvas::keyReleaseEvent(QKeyEvent* event) {
    if (drag_ && drag_->keys && !event->isAutoRepeat() &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
         event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        const bool owned = session_.isValueEditing(drag_->primary);
        drag_.reset();
        if (owned)
            static_cast<void>(session_.commitValueEdit());
        update();
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void GradeWheelCanvas::focusOutEvent(QFocusEvent* event) {
    if (drag_ && drag_->keys) {
        const bool owned = session_.isValueEditing(drag_->primary);
        drag_.reset();
        if (owned)
            static_cast<void>(session_.commitValueEdit());
    }
    cancelDrag();
    QWidget::focusOutEvent(event);
    update();
}

void GradeWheelCanvas::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void GradeWheelCanvas::leaveEvent(QEvent* event) {
    if (hovered_) {
        hovered_ = false;
        update();
    }
    QWidget::leaveEvent(event);
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

bool GradeWheelCanvas::resetColour() {
    const auto ids = parameters();
    const auto current = values();
    if (!ids || !current)
        return false;
    const double neutralValue = document::gradeWheelDomain(wheel_).neutral;
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> changes;
    for (std::size_t component = 1; component < ids->size(); ++component)
        if ((*current)[component] != neutralValue)
            changes.emplace_back((*ids)[component], neutralValue);
    if (changes.empty())
        return true;
    return session_.setParameterValues(std::move(changes),
                                       tr("Reset %1 Colour").arg(gradeWheelName(wheel_)));
}

bool GradeWheelCanvas::reset() {
    const auto ids = parameters();
    const auto current = values();
    if (!ids || !current)
        return false;
    const double neutralValue = document::gradeWheelDomain(wheel_).neutral;
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> changes;
    for (std::size_t component = 0; component < ids->size(); ++component)
        if ((*current)[component] != neutralValue)
            changes.emplace_back((*ids)[component], neutralValue);
    if (changes.empty())
        return true;
    return session_.setParameterValues(std::move(changes),
                                       tr("Reset %1").arg(gradeWheelName(wheel_)));
}

// --- GradeContrastCanvas ------------------------------------------------------------------------

GradeContrastCanvas::GradeContrastCanvas(CompositionSession& session, const document::NodeId node,
                                         QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("gradeContrastCurve"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SizeHorCursor);
    setAccessibleName(tr("Pivot"));
    setToolTip(tr("Contrast about the pivot: the marker is the pivot"));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, qOverload<>(&QWidget::update));
}

std::optional<document::ParameterId>
GradeContrastCanvas::parameter(const std::string_view role) const {
    return nodeRoleParameter(session_, node_, role);
}

double GradeContrastCanvas::contrast() const {
    const auto id = parameter(document::gradePrimaryRole(GradePrimary::Contrast));
    return id ? liveScalar(session_, *id).value_or(1.0) : 1.0;
}

double GradeContrastCanvas::pivot() const {
    const auto id = parameter(document::gradePrimaryRole(GradePrimary::Pivot));
    return id ? liveScalar(session_, *id).value_or(document::kDefaultGradePivot)
              : document::kDefaultGradePivot;
}

QRectF GradeContrastCanvas::plotRect() const {
    const qreal marker = kit::px(kit::Spacing::XS) + kit::px(kit::Spacing::XXS);
    return QRectF(rect()).adjusted(marker, kit::px(kit::Spacing::XXS), -marker, -marker - 1.0);
}

double GradeContrastCanvas::pivotX() const {
    const auto plot = plotRect();
    return plot.left() + plot.width() * std::clamp(pivot(), 0.0, 1.0);
}

void GradeContrastCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto plot = plotRect();
    painter.setOpacity(isEnabled() ? 1.0 : kit::kDisabledOpacity);
    // One uniform grey, as every grading graph, with the quarter grid.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::Field));
    painter.drawRect(plot);
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    for (int line = 1; line < 4; ++line) {
        const qreal x = plot.left() + plot.width() * line / 4.0;
        const qreal y = plot.top() + plot.height() * line / 4.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    const auto at = [&](const double x, const double y) {
        return QPointF(plot.left() + plot.width() * x, plot.bottom() - plot.height() * y);
    };
    // The identity, then the curve: pivot * (x / pivot)^contrast.
    painter.setPen(QPen(kit::color(kit::Color::Faint), kit::kHairlineWidth, Qt::DashLine));
    painter.drawLine(at(0.0, 0.0), at(1.0, 1.0));
    const double c = contrast();
    const double p = std::max(pivot(), document::kMinimumGradePivot);
    QPainterPath path;
    const int steps = std::max(16, static_cast<int>(plot.width() / 2.0));
    for (int step = 0; step <= steps; ++step) {
        const double x = static_cast<double>(step) / steps;
        const double y = x <= 0.0 ? 0.0 : p * std::pow(x / p, c);
        const auto point = at(x, std::clamp(y, -0.05, 1.05));
        if (step == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    painter.save();
    painter.setClipRect(plot);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kit::color(kit::Color::Foreground), kit::kChartLineStroke));
    painter.drawPath(path);
    painter.restore();
    // The pivot: a line up the plot and the marker under it.
    const qreal x = pivotX();
    const auto ink = kit::color(drag_ || hasFocus() ? kit::Color::Accent : kit::Color::Muted);
    kit::applyHairlinePen(painter, ink);
    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    const qreal half = kit::px(kit::Spacing::XS) + kit::px(kit::Spacing::XXS);
    QPainterPath marker;
    marker.moveTo(x, plot.bottom());
    marker.lineTo(x + half, plot.bottom() + half);
    marker.lineTo(x - half, plot.bottom() + half);
    marker.closeSubpath();
    painter.setPen(QPen(ink, kit::kHairlineWidth));
    painter.setBrush(kit::color(kit::Color::Foreground));
    painter.drawPath(marker);
    painter.setOpacity(1.0);
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);
}

void GradeContrastCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    event->accept();
    const auto id = parameter(document::gradePrimaryRole(GradePrimary::Pivot));
    if (!id || !session_.beginValueEdit(*id))
        return;
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    drag_ = Drag{*id, event->position().x(), pivot(), kit::KSlider::dragScale(modifiers),
                 modifiers};
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        // A Shift-press jumps the pivot to the pointer, which it then follows.
        const auto plot = plotRect();
        const double value = std::clamp((event->position().x() - plot.left()) / plot.width(),
                                        document::kMinimumGradePivot, 1.0);
        drag_->base = value;
        drag_->scale = 1.0;
        if (!session_.updateValueEdit(value))
            cancelDrag();
    }
    update();
}

void GradeContrastCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_)
        return;
    if (!session_.isValueEditing(drag_->parameter)) {
        drag_.reset();
        update();
        return;
    }
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers != drag_->modifiers) {
        drag_->base = pivot();
        drag_->fromX = event->position().x();
        drag_->scale = kit::KSlider::dragScale(modifiers);
        drag_->modifiers = modifiers;
    }
    const double travel = (event->position().x() - drag_->fromX) * drag_->scale;
    const double next = std::clamp(drag_->base + travel / std::max(1.0, plotRect().width()),
                                   document::kMinimumGradePivot, document::kMaximumGradePivot);
    if (!session_.updateValueEdit(next))
        cancelDrag();
    event->accept();
}

void GradeContrastCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool owned = session_.isValueEditing(drag_->parameter);
    drag_.reset();
    if (owned)
        static_cast<void>(session_.commitValueEdit());
    update();
    event->accept();
}

void GradeContrastCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    static_cast<void>(resetPivot());
    event->accept();
}

bool GradeContrastCanvas::resetPivot() {
    const auto id = parameter(document::gradePrimaryRole(GradePrimary::Pivot));
    if (!id)
        return false;
    if (pivot() == document::kDefaultGradePivot)
        return true;
    return session_.setParameterValue(*id, document::kDefaultGradePivot, tr("Reset Pivot"));
}

QMenu* GradeContrastCanvas::makeContextMenu() {
    auto* menu = kit::makeMenu(this);
    auto* reset = menu->addAction(tr("Reset Pivot"));
    reset->setObjectName(QStringLiteral("gradeContrastResetPivot"));
    reset->setEnabled(isEnabled() && pivot() != document::kDefaultGradePivot);
    connect(reset, &QAction::triggered, this, [this] { static_cast<void>(resetPivot()); });
    const auto id = parameter(document::gradePrimaryRole(GradePrimary::Pivot));
    const auto state =
        id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    if (state != KeyframeDiamondState::Unsupported) {
        auto* key = menu->addAction(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                                    : tr("Add Key"));
        key->setObjectName(QStringLiteral("gradeContrastKey"));
        connect(key, &QAction::triggered, this, [this, parameter = *id] {
            static_cast<void>(session_.toggleKeyframeForParameter(parameter));
        });
    }
    return menu;
}

void GradeContrastCanvas::contextMenuEvent(QContextMenuEvent* event) {
    if (drag_) {
        event->accept();
        return;
    }
    const std::unique_ptr<QMenu> menu(makeContextMenu());
    menu->exec(event->globalPos());
    event->accept();
}

void GradeContrastCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void GradeContrastCanvas::focusOutEvent(QFocusEvent* event) {
    cancelDrag();
    QWidget::focusOutEvent(event);
    update();
}

void GradeContrastCanvas::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void GradeContrastCanvas::cancelDrag() {
    if (!drag_)
        return;
    const auto id = drag_->parameter;
    drag_.reset();
    if (session_.isValueEditing(id))
        session_.cancelValueEdit();
    update();
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
        const auto name = gradeWheelName(wheel);
        auto& cell = cells_[index];
        cell.title = new kit::KLabel(name, this, kit::TypeRole::UiSmall);
        cell.title->setObjectName(QStringLiteral("gradeWheelTitle"));
        cell.title->setTone(kit::Color::Foreground);
        cell.diamond = new KeyframeGroupDiamond(session_, name, this);
        cell.diamond->setObjectName(QStringLiteral("gradeWheelDiamond"));
        cell.reset = new kit::KButton(kit::IconId::Reset, QString{}, this);
        cell.reset->setObjectName(QStringLiteral("gradeWheelReset"));
        cell.reset->setControlSize(kit::KButton::ControlSize::Compact);
        cell.reset->setAccessibleName(tr("Reset %1").arg(name));
        cell.reset->setToolTip(tr("Reset %1: colour and level").arg(name));
        // The Values disclosure: its caret turns down while the wheel's rows are shown.
        auto* disclosure = new kit::KButton(kit::IconId::CaretRight, tr("Values"), this);
        disclosure->setVariant(kit::KButton::Variant::Ghost);
        disclosure->setControlSize(kit::KButton::ControlSize::Compact);
        disclosure->setCheckable(true);
        disclosure->setObjectName(QStringLiteral("gradeWheelValues"));
        disclosure->setAccessibleName(tr("%1 values").arg(name));
        disclosure->setToolTip(tr("Show the %1 rows, with their sockets, to link them").arg(name));
        cell.values = disclosure;
        connect(cell.values, &QAbstractButton::clicked, this,
                [this, index] { setExpanded(index, !expanded_[index]); });
        cell.canvas = new GradeWheelCanvas(session_, node_, wheel, this);
        connect(cell.canvas, &GradeWheelCanvas::alternateClicked, this,
                [this, index] { setExpanded(index, !expanded_[index]); });
        connect(cell.canvas, &GradeWheelCanvas::keyRequested, cell.diamond,
                [diamond = cell.diamond] { static_cast<void>(diamond->toggle()); });
        connect(cell.reset, &QAbstractButton::clicked, cell.canvas,
                [canvas = cell.canvas] { static_cast<void>(canvas->reset()); });
        // The level: its label, its value with its unit, and the jog strip under them.
        const auto masterRole = document::gradeWheelRole(wheel, 0);
        const auto masterKey = document::gradeWheelSchemaKey(wheel, 0);
        const auto domain = document::gradeWheelDomain(wheel);
        cell.level = new GradingValueRow(session_, node_, masterRole, masterKey,
                                         tr("%1 level").arg(name), domain.neutral, this);
        cell.level->setDiamondShown(false);
        cell.level->setShowsSign(wheel != GradeWheel::Gain);
        cell.level->setDescription(gradeWheelLevelDescription(wheel));
        const auto presentation = document::parameterPresentation(
            masterKey, document::ParameterValueKind::Float64, masterRole);
        GradingSliderSpec jog;
        jog.mode = kit::KSlider::DragMode::Jog;
        jog.minimum = presentation.softMinimum;
        jog.maximum = presentation.softMaximum;
        jog.neutral = domain.neutral;
        jog.jogStep = presentation.step / presentation.displayScale;
        jog.gradient = jogStops();
        cell.level->setSlider(jog);
        if (wheel == GradeWheel::Gamma)
            cell.level->setValueToolTip([](const double value) {
                return tr("Gamma level: power %1").arg(std::pow(2.0, -value), 0, 'f', 2);
            });
        // The red, green and blue values, each with its channel's colour.
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto role = document::gradeWheelRole(wheel, channel + 1);
            auto* field = new kit::KValueField(this);
            field->setObjectName(QStringLiteral("gradeWheelColour.%1")
                                     .arg(QString::fromStdString(role)));
            field->setCompact(true);
            field->setOutlinedAtRest(true);
            field->setMaximumWidth(QWIDGETSIZE_MAX);
            configureValueField(*field, document::gradeWheelSchemaKey(wheel, channel + 1),
                                document::ParameterValueKind::Float64, role);
            // Shown at two places, stored at three: the level above carries the finer reading.
            auto shown = field->presentation();
            shown.decimals = 2;
            field->setPresentation(shown);
            field->setShowsSign(wheel != GradeWheel::Gain);
            field->setChannelLine(kit::color(channelInk(channel)));
            field->setDefaultValue(domain.neutral);
            const auto label = tr("%1 %2").arg(name, channelName(channel));
            field->setAccessibleName(label);
            field->setToolTip(label);
            field->setResetText(tr("Reset %1").arg(label));
            auto* key = new QAction(tr("Add Key"), field);
            key->setObjectName(QStringLiteral("gradeWheelColourKey"));
            connect(key, &QAction::triggered, this, [this, role] {
                if (const auto id = parameter(role))
                    static_cast<void>(session_.toggleKeyframeForParameter(*id));
            });
            field->addAction(key);
            properties::bindValueEdit(session_, *field, [this, role] {
                return parameter(role).value_or(document::ParameterId{});
            });
            connect(field, &kit::KValueField::valueChanged, this, [this, role, label](double value) {
                if (const auto id = parameter(role))
                    static_cast<void>(
                        session_.setParameterValue(*id, value, tr("Set %1").arg(label)));
            });
            cell.colour[channel] = field;
        }
    }
    primariesTitle_ = new kit::KLabel(tr("Primaries"), this, kit::TypeRole::UiSmall);
    primariesTitle_->setObjectName(QStringLiteral("gradePrimariesTitle"));
    primariesTitle_->setTone(kit::Color::Foreground);
    primariesReset_ = new kit::KButton(kit::IconId::Reset, QString{}, this);
    primariesReset_->setObjectName(QStringLiteral("gradePrimariesReset"));
    primariesReset_->setControlSize(kit::KButton::ControlSize::Compact);
    primariesReset_->setAccessibleName(tr("Reset Primaries"));
    primariesReset_->setToolTip(tr("Reset the six primaries"));
    connect(primariesReset_, &QAbstractButton::clicked, this,
            [this] { static_cast<void>(resetPrimaries()); });
    for (std::size_t index = 0; index < primaries_.size(); ++index) {
        const auto which = document::kGradePrimaries[index];
        const auto role = std::string(document::gradePrimaryRole(which));
        auto* row = new GradingValueRow(session_, node_, role, document::gradePrimarySchemaKey(which),
                                        primaryName(which),
                                        document::gradePrimaryDomain(which).neutral, this);
        row->setDescription(primaryDescription(which));
        GradingSliderSpec slider;
        switch (which) {
        case GradePrimary::Temperature:
        case GradePrimary::Tint:
            row->setShowsSign(true);
            slider.minimum = -100.0;
            slider.maximum = 100.0;
            slider.neutral = 0.0;
            slider.gradient =
                which == GradePrimary::Temperature ? temperatureStops() : tintStops();
            row->setSlider(slider);
            break;
        case GradePrimary::Contrast:
            slider.minimum = 0.0;
            slider.maximum = 2.0;
            slider.neutral = 1.0;
            slider.ticks = 0.5;
            row->setSlider(slider);
            break;
        case GradePrimary::Saturation:
            slider.minimum = 0.0;
            slider.maximum = 2.0;
            slider.neutral = 1.0;
            slider.gradient = greyToColourStops(0.5);
            row->setSlider(slider);
            break;
        case GradePrimary::Exposure:
            row->setShowsSign(true);
            slider.minimum = -4.0;
            slider.maximum = 4.0;
            slider.neutral = 0.0;
            slider.ticks = 1.0;
            slider.gradient = darkToLightStops();
            row->setSlider(slider);
            break;
        case GradePrimary::Pivot:
            break;
        }
        row->setObjectName(QStringLiteral("gradePrimary.%1").arg(QString::fromStdString(role)));
        primaries_[index] = row;
    }
    contrast_ = new GradeContrastCanvas(session_, node_, this);
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &GradeCardWidget::refresh);
    refresh();
}

std::optional<document::ParameterId> GradeCardWidget::parameter(const std::string_view role) const {
    return nodeRoleParameter(session_, node_, role);
}

GradeWheelCanvas* GradeCardWidget::canvas(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].canvas;
}
GradingValueRow* GradeCardWidget::level(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].level;
}
kit::KSlider* GradeCardWidget::master(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].level->slider();
}
kit::KValueField* GradeCardWidget::colour(const GradeWheel wheel, const std::size_t channel) const {
    return cells_[static_cast<std::size_t>(wheel)].colour.at(channel);
}
kit::KButton* GradeCardWidget::resetButton(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].reset;
}
kit::KButton* GradeCardWidget::valuesButton(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].values;
}
KeyframeGroupDiamond* GradeCardWidget::diamond(const GradeWheel wheel) const {
    return cells_[static_cast<std::size_t>(wheel)].diamond;
}
GradingValueRow* GradeCardWidget::primaryRow(const GradePrimary which) const {
    return primaries_[static_cast<std::size_t>(which)];
}
kit::KValueField* GradeCardWidget::primary(const GradePrimary which) const {
    return primaries_[static_cast<std::size_t>(which)]->field();
}

bool GradeCardWidget::showsRole(const std::string_view role) const {
    for (std::size_t index = 0; index < cells_.size(); ++index)
        for (std::size_t component = 0; component < document::kGradeWheelComponentCount;
             ++component)
            if (role == document::gradeWheelRole(document::kGradeWheels[index], component))
                return expanded_[index];
    // The primaries draw their own rows, with their sockets beside them.
    return false;
}

std::optional<int> GradeCardWidget::socketY(const std::string_view role) const {
    for (const auto* row : primaries_)
        if (row->role() == role)
            return row->y() + row->socketY();
    return std::nullopt;
}

void GradeCardWidget::setValuesExpanded(const GradeWheel wheel, const bool expanded) {
    setExpanded(static_cast<std::size_t>(wheel), expanded);
}
bool GradeCardWidget::valuesExpanded(const GradeWheel wheel) const {
    return expanded_[static_cast<std::size_t>(wheel)];
}

void GradeCardWidget::setExpanded(const std::size_t group, const bool expanded) {
    auto* button = cells_[group].values;
    {
        const QSignalBlocker blocker(button);
        button->setChecked(expanded);
    }
    button->setIconId(expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    if (expanded_[group] == expanded)
        return;
    expanded_[group] = expanded;
    emit rowsChanged();
}

bool GradeCardWidget::resetPrimaries() {
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> changes;
    for (const auto which : document::kGradePrimaries) {
        const auto id = parameter(document::gradePrimaryRole(which));
        const auto value = id ? liveScalar(session_, *id) : std::nullopt;
        const double neutral = document::gradePrimaryDomain(which).neutral;
        if (id && value && *value != neutral && scalarEditable(session_, *id))
            changes.emplace_back(*id, neutral);
    }
    if (changes.empty())
        return true;
    return session_.setParameterValues(std::move(changes), tr("Reset Primaries"));
}

int GradeCardWidget::cellMinimumWidth() const {
    const auto& cell = cells_.front();
    int width = kit::px(kit::Size::ImageThumbnail) + control();
    // The title row: the widest wheel name, the diamond, Reset and Values.
    int title = 0;
    for (const auto& each : cells_)
        title = std::max(title, each.title->sizeHint().width());
    title += 3 * small() + kit::px(kit::Size::IconSmall) + control() +
             cell.values->sizeHint().width();
    width = std::max(width, title);
    // The level row, and three colour fields wide enough for their widest reading and unit.
    const QFontMetrics value(kit::font(kit::TypeRole::Value));
    for (const auto& each : cells_) {
        width = std::max(width, each.level->minimumWidthHint());
        const auto presentation = each.colour[0]->presentation();
        const int unit = presentation.unit.isEmpty()
                             ? 0
                             : value.horizontalAdvance(presentation.unit) + small();
        const int field = value.horizontalAdvance(QStringLiteral("+0.00")) + unit + 2 * small() +
                          kit::px(kit::Spacing::XXS);
        width = std::max(width, 3 * field + 2 * small());
    }
    return width;
}

int GradeCardWidget::minimumWidthHint() const {
    int primaries = 0;
    for (const auto* row : primaries_)
        primaries = std::max(primaries, row->minimumWidthHint());
    return std::max(2 * cellMinimumWidth() + gap(), primaries);
}

GradeCardWidget::Metrics GradeCardWidget::metrics(const int width) const {
    Metrics m;
    m.cell = std::max(0, (width - gap()) / 2);
    m.disc = static_cast<int>(std::round(m.cell * kDiscShare));
    // Title, disc, level row and its jog strip, then the colour row.
    m.cellHeight = control() + small() + m.disc + small() + 2 * control() + small() + control();
    m.primariesTop = 2 * m.cellHeight + gap() + gap();
    return m;
}

int GradeCardWidget::heightAt(const int width) const {
    const auto m = metrics(width);
    int height = m.primariesTop + control() + small();
    for (const auto* row : primaries_)
        height += row->heightHint() + small();
    // The Pivot row's contrast curve.
    height += 2 * control() + small();
    return height - small();
}

QSize GradeCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void GradeCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void GradeCardWidget::place() {
    const int w = width();
    const auto m = metrics(w);
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        const int x = static_cast<int>(index % 2) * (m.cell + gap());
        int y = static_cast<int>(index / 2) * (m.cellHeight + gap());
        auto& c = cells_[index];
        // The title row: the name, then the diamond, Reset and Values at its right.
        const int values = c.values->sizeHint().width();
        int right = x + m.cell - values;
        c.values->setGeometry(right, y, values, control());
        right -= small() + control();
        c.reset->setGeometry(right, y, control(), control());
        right -= small() + c.diamond->width();
        c.diamond->move(right, y + (control() - c.diamond->height()) / 2);
        c.title->setGeometry(x, y, std::max(0, right - small() - x), control());
        y += control() + small();
        c.canvas->setGeometry(x + (m.cell - m.disc) / 2, y, m.disc, m.disc);
        y += m.disc + small();
        c.level->setGeometry(x, y, m.cell, c.level->heightHint());
        if (auto* jog = c.level->slider()) {
            const int strip = static_cast<int>(std::round(m.disc * kJogShare));
            jog->setGeometry((m.cell - strip) / 2, control(), strip, control());
        }
        y += c.level->heightHint() + small();
        const int field = (m.cell - 2 * small()) / 3;
        for (std::size_t channel = 0; channel < 3; ++channel)
            c.colour[channel]->setGeometry(
                x + static_cast<int>(channel) * (field + small()), y,
                channel == 2 ? m.cell - 2 * (field + small()) : field, control());
    }
    int y = m.primariesTop;
    primariesTitle_->setGeometry(0, y, std::max(0, w - control() - small()), control());
    primariesReset_->setGeometry(std::max(0, w - control()), y, control(), control());
    y += control() + small();
    // One primary per row, the names in one column, the field taking the rest of the row.
    int labels = 0;
    for (const auto* row : primaries_)
        labels = std::max(labels, row->labelWidthHint());
    for (std::size_t index = 0; index < primaries_.size(); ++index) {
        auto* row = primaries_[index];
        row->setLabelWidth(labels);
        row->setGeometry(0, y, w, row->heightHint());
        y += row->heightHint() + small();
        if (document::kGradePrimaries[index] == GradePrimary::Pivot) {
            // The contrast curve: two control rows tall.
            const int picture = control() + control();
            contrast_->setGeometry(0, y, w, picture);
            y += picture + small();
        }
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
                colour = colour && scalarEditable(session_, (*ids)[component]);
        cell.canvas->setEnabled(colour);
        cell.reset->setEnabled(!nodeLocked && ids.has_value());
        cell.level->refresh(nodeLocked);
        if (ids)
            cell.diamond->setParameters({ids->begin(), ids->end()});
        cell.diamond->refresh();
        for (std::size_t channel = 0; channel < 3; ++channel) {
            auto* field = cell.colour[channel];
            const auto id = parameter(document::gradeWheelRole(wheel, channel + 1));
            field->setEnabled(id && !nodeLocked && scalarEditable(session_, *id));
            if (id && !field->hasLiveEdit())
                if (const auto value = liveScalar(session_, *id)) {
                    const QSignalBlocker blocker(field);
                    field->setValue(*value);
                }
            const auto state = id ? session_.keyframeDiamondStateForParameter(*id)
                                  : KeyframeDiamondState::Unsupported;
            for (auto* action : field->actions()) {
                action->setVisible(state != KeyframeDiamondState::Unsupported);
                action->setText(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                                : tr("Add Key"));
            }
        }
        cell.canvas->update();
    }
    for (auto* row : primaries_)
        row->refresh(nodeLocked);
    const auto pivot = parameter(document::gradePrimaryRole(GradePrimary::Pivot));
    contrast_->setEnabled(pivot && !nodeLocked && scalarEditable(session_, *pivot));
    contrast_->update();
}

} // namespace bloom::ui

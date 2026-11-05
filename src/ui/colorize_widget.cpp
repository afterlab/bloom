#include <bloom/ui/colorize_widget.hpp>

#include "properties_sections.hpp"
#include "properties_value_edits.hpp"

#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/gradient_parameter_row.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/gradient_editor.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>

#include <QAction>
#include <QContextMenuEvent>
#include <QCoreApplication>
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

constexpr std::string_view kModeRole = "mode";
constexpr std::string_view kMidpointRole = "midpoint";
constexpr std::string_view kGradientRole = "gradient";
constexpr int kGradientSamples = 48;
constexpr double kMidpointMinimum = 0.01;
constexpr double kMidpointMaximum = 0.99;

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QColor clampedDisplay(const std::array<double, 3>& rgb) {
    return QColor::fromRgbF(static_cast<float>(std::clamp(rgb[0], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[2], 0.0, 1.0)));
}

// The node's own default for `role`, the value a reset returns to.
[[nodiscard]] std::optional<document::ParameterValue> defaultValue(const std::string_view role) {
    const auto* definition =
        document::builtInNodeDefinitions().find(document::kColorizeNodeType, 1);
    if (definition == nullptr)
        return std::nullopt;
    const auto parameter =
        std::ranges::find(definition->parameters, role, &document::ParameterDefinition::role);
    if (parameter == definition->parameters.end())
        return std::nullopt;
    return parameter->defaultValue;
}

[[nodiscard]] double defaultScalar(const std::string_view role) {
    const auto value = defaultValue(role);
    const auto* scalar = value ? std::get_if<double>(&*value) : nullptr;
    return scalar ? *scalar : 0.0;
}

struct WeightRow final {
    std::string_view role;
    std::string_view schemaKey;
    const char* name;
    std::array<double, 3> hue;
};

// The six hues of the black-and-white mix, each drawn at the end of its slider's track.
const std::array<WeightRow, 6>& weightRows() {
    static const std::array<WeightRow, 6> rows{WeightRow{"reds",
                                                         document::kColorizeRedsSchemaKey,
                                                         QT_TRANSLATE_NOOP("Colorize", "Reds"),
                                                         {0.92, 0.22, 0.2}},
                                               WeightRow{"yellows",
                                                         document::kColorizeYellowsSchemaKey,
                                                         QT_TRANSLATE_NOOP("Colorize", "Yellows"),
                                                         {0.92, 0.84, 0.2}},
                                               WeightRow{"greens",
                                                         document::kColorizeGreensSchemaKey,
                                                         QT_TRANSLATE_NOOP("Colorize", "Greens"),
                                                         {0.26, 0.8, 0.3}},
                                               WeightRow{"cyans",
                                                         document::kColorizeCyansSchemaKey,
                                                         QT_TRANSLATE_NOOP("Colorize", "Cyans"),
                                                         {0.2, 0.8, 0.86}},
                                               WeightRow{"blues",
                                                         document::kColorizeBluesSchemaKey,
                                                         QT_TRANSLATE_NOOP("Colorize", "Blues"),
                                                         {0.3, 0.42, 0.96}},
                                               WeightRow{"magentas",
                                                         document::kColorizeMagentasSchemaKey,
                                                         QT_TRANSLATE_NOOP("Colorize", "Magentas"),
                                                         {0.86, 0.3, 0.86}}};
    return rows;
}

[[nodiscard]] std::optional<std::array<double, 4>> liveColour(const CompositionSession& session,
                                                              const document::ParameterId id) {
    const auto value = session.liveValue(id);
    const auto* colour = value ? std::get_if<core::Color4d>(&*value) : nullptr;
    if (colour == nullptr)
        return std::nullopt;
    return std::array{colour->red, colour->green, colour->blue, colour->alpha};
}

void openKeyMenuEntry(QMenu& menu, CompositionSession& session,
                      const std::optional<document::ParameterId> id, const QString& objectName) {
    const auto state =
        id ? session.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    if (state == KeyframeDiamondState::Unsupported)
        return;
    auto* key =
        menu.addAction(state == KeyframeDiamondState::AnimatedWithKey ? QObject::tr("Remove Key")
                                                                      : QObject::tr("Add Key"));
    key->setObjectName(objectName);
    QObject::connect(key, &QAction::triggered, &menu, [&session, parameter = *id] {
        static_cast<void>(session.toggleKeyframeForParameter(parameter));
    });
}

} // namespace

// --- ColorizeColorRow ---------------------------------------------------------------------------

ColorizeColorRow::ColorizeColorRow(CompositionSession& session, const document::NodeId node,
                                   std::string role, const std::string_view schemaKey, QString name,
                                   QWidget* parent)
    : QWidget(parent), session_(session), node_(node), role_(std::move(role)),
      schemaKey_(schemaKey), name_(std::move(name)) {
    const auto objectRole = QString::fromStdString(role_);
    setObjectName(QStringLiteral("colorizeColorRow.%1").arg(objectRole));
    label_ = new kit::KLabel(name_, this, kit::TypeRole::UiSmall);
    label_->setObjectName(QStringLiteral("colorizeColorLabel.%1").arg(objectRole));
    label_->setTone(kit::Color::Muted);
    label_->installEventFilter(this);
    chip_ = new kit::KColorChip(this);
    chip_->setObjectName(QStringLiteral("colorizeColorChip.%1").arg(objectRole));
    chip_->setAccessibleName(name_);
    chip_->setControlSize(kit::KColorChip::ControlSize::Compact);
    chip_->installEventFilter(this);
    diamond_ = new KeyframeDiamond(session_, role_, this);
    diamond_->setObjectName(QStringLiteral("colorizeColorDiamond"));
    keyAction_ = new QAction(tr("Add Key"), this);
    connect(keyAction_, &QAction::triggered, this, [this] {
        if (const auto id = parameter())
            static_cast<void>(session_.toggleKeyframeForParameter(*id));
    });
    properties::bindValueEdit(session_, *chip_,
                              [this] { return parameter().value_or(document::ParameterId{}); });
    connect(chip_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& colour) {
        if (refreshing_)
            return;
        if (const auto id = parameter())
            static_cast<void>(session_.setParameterValue(
                *id,
                core::Color4d{static_cast<double>(colour.red), static_cast<double>(colour.green),
                              static_cast<double>(colour.blue), static_cast<double>(colour.alpha)},
                tr("Set %1").arg(name_)));
    });
}

std::optional<document::ParameterId> ColorizeColorRow::parameter() const {
    return nodeRoleParameter(session_, node_, role_);
}

std::optional<std::array<double, 4>> ColorizeColorRow::value() const {
    const auto id = parameter();
    return id ? liveColour(session_, *id) : std::nullopt;
}

void ColorizeColorRow::setDescription(const QString& description) {
    label_->setToolTip(description);
    label_->setAccessibleDescription(description);
    chip_->setToolTip(description);
    chip_->setAccessibleDescription(description);
}

void ColorizeColorRow::setLabelWidth(const int width) {
    labelWidth_ = std::max(0, width);
    place();
}

int ColorizeColorRow::labelWidthHint() const {
    return QFontMetrics(label_->font()).horizontalAdvance(name_) + small();
}

int ColorizeColorRow::minimumWidthHint() const {
    return std::max(labelWidth_, labelWidthHint()) + small() + 2 * control() + small() +
           kit::px(kit::Size::IconSmall);
}

int ColorizeColorRow::socketY() const { return control() / 2; }

void ColorizeColorRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place();
}

void ColorizeColorRow::place() {
    const int w = width();
    const int labelWidth = std::max(labelWidth_, labelWidthHint());
    label_->setGeometry(0, 0, labelWidth, control());
    const int left = labelWidth + small();
    const int diamond = kit::px(kit::Size::IconSmall) + small();
    const int chipHeight = std::min(control(), chip_->sizeHint().height());
    chip_->setGeometry(left, (control() - chipHeight) / 2, std::max(0, w - left - diamond),
                       chipHeight);
    diamond_->move(w - kit::px(kit::Size::IconSmall), (control() - diamond_->height()) / 2);
}

bool ColorizeColorRow::reset() {
    const auto id = parameter();
    const auto neutral = defaultValue(role_);
    if (!id || !neutral)
        return false;
    if (const auto current = session_.liveValue(*id); current && *current == *neutral)
        return true;
    return session_.setParameterValue(*id, *neutral, tr("Reset %1").arg(name_));
}

QMenu* ColorizeColorRow::makeContextMenu() {
    auto* menu = kit::makeMenu(this);
    auto* reset = menu->addAction(tr("Reset %1").arg(name_));
    reset->setObjectName(QStringLiteral("colorizeColorReset"));
    reset->setEnabled(chip_->isEnabled());
    connect(reset, &QAction::triggered, this, [this] { static_cast<void>(this->reset()); });
    openKeyMenuEntry(*menu, session_, parameter(), QStringLiteral("colorizeColorKey"));
    return menu;
}

bool ColorizeColorRow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == label_ && event->type() == QEvent::MouseButtonDblClick &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && chip_->isEnabled()) {
        static_cast<void>(reset());
        return true;
    }
    if ((watched == label_ || watched == chip_) && event->type() == QEvent::ContextMenu) {
        const std::unique_ptr<QMenu> menu(makeContextMenu());
        menu->exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ColorizeColorRow::refresh(const bool locked) {
    const auto id = parameter();
    const bool enabled = id && !locked && scalarEditable(session_, *id);
    chip_->setEnabled(enabled);
    if (const auto colour = value(); colour && !session_.isValueEditing(*id)) {
        refreshing_ = true;
        properties::refreshColor(
            session_, schemaKey_,
            core::Color4d{(*colour)[0], (*colour)[1], (*colour)[2], (*colour)[3]}, chip_);
        refreshing_ = false;
    }
    diamond_->setParameterId(id.value_or(document::ParameterId{}));
    diamond_->refresh();
    const auto state =
        id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    keyAction_->setVisible(state != KeyframeDiamondState::Unsupported);
    keyAction_->setEnabled(!locked);
}

// --- ColorizeRampBar ----------------------------------------------------------------------------

ColorizeRampBar::ColorizeRampBar(CompositionSession& session, const document::NodeId node,
                                 QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("colorizeRampBar"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setAccessibleName(tr("Gradient"));
}

void ColorizeRampBar::setStops(std::vector<Stop> stops, const bool ramp) {
    stops_ = std::move(stops);
    ramp_ = ramp && stops_.size() == 3;
    setToolTip(ramp_ ? tr("Drag the middle stop to move the midpoint; click a stop to pick its "
                          "colour")
                     : tr("Click a stop to pick its colour"));
    update();
}

void ColorizeRampBar::setStopClicked(std::function<void(const std::string&)> clicked) {
    stopClicked_ = std::move(clicked);
}

std::optional<document::ParameterId> ColorizeRampBar::midpointParameter() const {
    return nodeRoleParameter(session_, node_, kMidpointRole);
}

double ColorizeRampBar::midpoint() const {
    const auto id = midpointParameter();
    return id ? liveScalar(session_, *id).value_or(0.18) : 0.18;
}

int ColorizeRampBar::heightHint() const { return control() + control() / 2 + small(); }

QRectF ColorizeRampBar::barRect() const {
    // Inset by half a handle on each side, so the end stops sit fully inside the widget.
    const qreal inset = control() / 4.0;
    return {inset, 0.0, std::max<qreal>(0.0, width() - inset - inset), static_cast<qreal>(control())};
}

QPointF ColorizeRampBar::stopCentre(const std::size_t index) const {
    const auto bar = barRect();
    const double position = index < stops_.size() ? stops_[index].position : 0.0;
    return {bar.left() + position * bar.width(), bar.bottom() + small() / 2.0 + control() / 4.0};
}

std::array<double, 3> ColorizeRampBar::referenceAt(const double t) const {
    if (stops_.empty())
        return {};
    if (t <= stops_.front().position)
        return stops_.front().colour;
    for (std::size_t index = 1; index < stops_.size(); ++index) {
        const auto& from = stops_[index - 1];
        const auto& to = stops_[index];
        if (t <= to.position) {
            const double span = to.position - from.position;
            const double u = span > 0.0 ? (t - from.position) / span : 1.0;
            return {from.colour[0] + u * (to.colour[0] - from.colour[0]),
                    from.colour[1] + u * (to.colour[1] - from.colour[1]),
                    from.colour[2] + u * (to.colour[2] - from.colour[2])};
        }
    }
    return stops_.back().colour;
}

QColor ColorizeRampBar::colourAt(const double t) const {
    // The node interpolates scene-linear colour; the bar shows each sample as the display does.
    const auto reference = referenceAt(std::clamp(t, 0.0, 1.0));
    const auto converter = session_.colorConverter(document::kColorizeShadowsSchemaKey);
    const auto colour =
        kit::KColor::fromRgba(static_cast<float>(reference[0]), static_cast<float>(reference[1]),
                              static_cast<float>(reference[2]), 1.0F, kit::ColorSpace::Reference);
    if (const auto display = colour.converted(kit::ColorSpace::Display, converter))
        return clampedDisplay({static_cast<double>(display->red),
                               static_cast<double>(display->green),
                               static_cast<double>(display->blue)});
    return clampedDisplay(reference);
}

void ColorizeRampBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto bar = barRect();
    QLinearGradient gradient(bar.topLeft(), bar.topRight());
    for (int sample = 0; sample <= kGradientSamples; ++sample) {
        const double t = static_cast<double>(sample) / kGradientSamples;
        gradient.setColorAt(t, colourAt(t));
    }
    const qreal radius = kit::px(kit::Spacing::XXS);
    const bool enabled = isEnabled();
    painter.setOpacity(enabled ? 1.0 : kit::kDisabledOpacity);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(bar, radius, radius);
    painter.setBrush(Qt::NoBrush);
    kit::applyHairlinePen(
        painter, kit::color(drag_ || hasFocus() ? kit::Color::Accent : kit::Color::Border));
    painter.drawRoundedRect(bar, radius, radius);
    // Each stop: a tick into the bar and a swatch handle under it, pointing up at its position.
    const qreal half = control() / 4.0;
    for (std::size_t index = 0; index < stops_.size(); ++index) {
        const auto centre = stopCentre(index);
        const bool movable = ramp_ && index == 1;
        QPainterPath handle;
        handle.moveTo(centre.x(), bar.bottom());
        handle.lineTo(centre.x() + half, centre.y() - half / 2.0);
        handle.lineTo(centre.x() + half, centre.y() + half);
        handle.lineTo(centre.x() - half, centre.y() + half);
        handle.lineTo(centre.x() - half, centre.y() - half / 2.0);
        handle.closeSubpath();
        painter.setBrush(colourAt(stops_[index].position));
        kit::applyHairlinePen(
            painter, kit::color(movable && drag_ ? kit::Color::Accent : kit::Color::Foreground));
        painter.drawPath(handle);
    }
    painter.setOpacity(1.0);
}

std::optional<std::size_t> ColorizeRampBar::stopAt(const QPointF point) const {
    const qreal reach = control() / 3.0;
    std::optional<std::size_t> best;
    qreal nearest = reach;
    for (std::size_t index = 0; index < stops_.size(); ++index) {
        const auto centre = stopCentre(index);
        const qreal distance = std::abs(point.x() - centre.x());
        if (point.y() >= barRect().bottom() - small() && distance <= nearest) {
            nearest = distance;
            best = index;
        }
    }
    return best;
}

void ColorizeRampBar::mousePressEvent(QMouseEvent* event) {
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
    pressedStop_ = stopAt(event->position());
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    // The midpoint moves from its stop, or jumps to the pointer on a Shift-press over the bar.
    if (!ramp_ || (pressedStop_ != std::optional<std::size_t>{1} && !shift))
        return;
    const auto id = midpointParameter();
    if (!id || !scalarEditable(session_, *id) || !session_.beginValueEdit(*id))
        return;
    const auto modifiers = event->modifiers() & Qt::ControlModifier;
    drag_ =
        Drag{*id,  event->position().x(), midpoint(), kit::KSlider::dragScale(modifiers), modifiers,
             false};
    if (shift) {
        const auto bar = barRect();
        const double at = std::clamp((event->position().x() - bar.left()) / bar.width(),
                                     kMidpointMinimum, kMidpointMaximum);
        drag_->base = at;
        drag_->moved = true;
        pressedStop_.reset();
        if (!session_.updateValueEdit(at))
            cancelDrag();
    }
    update();
}

void ColorizeRampBar::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_)
        return;
    if (!session_.isValueEditing(drag_->parameter)) {
        drag_.reset();
        update();
        return;
    }
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers != drag_->modifiers) {
        // A modifier change continues from the midpoint reached, so nothing jumps.
        drag_->base = midpoint();
        drag_->fromX = event->position().x();
        drag_->scale = kit::KSlider::dragScale(modifiers);
        drag_->modifiers = modifiers;
    }
    const double dx = event->position().x() - drag_->fromX;
    if (std::abs(dx) > 0.0)
        drag_->moved = true;
    const double next =
        std::clamp(drag_->base + dx * drag_->scale / std::max<qreal>(1.0, barRect().width()),
                   kMidpointMinimum, kMidpointMaximum);
    if (!session_.updateValueEdit(next))
        cancelDrag();
    event->accept();
}

void ColorizeRampBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    bool moved = false;
    if (drag_) {
        moved = drag_->moved;
        const bool owned = session_.isValueEditing(drag_->parameter);
        drag_.reset();
        // One command for the gesture; a press that moved nothing commits nothing.
        if (owned) {
            if (moved)
                static_cast<void>(session_.commitValueEdit());
            else
                session_.cancelValueEdit();
        }
    }
    const auto clicked = pressedStop_;
    pressedStop_.reset();
    if (!moved && clicked && *clicked == stopAt(event->position()) && stopClicked_ &&
        *clicked < stops_.size())
        stopClicked_(stops_[*clicked].role);
    update();
    event->accept();
}

void ColorizeRampBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !ramp_) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    static_cast<void>(resetMidpoint());
    event->accept();
}

bool ColorizeRampBar::resetMidpoint() {
    const auto id = midpointParameter();
    if (!id || !ramp_)
        return false;
    const double neutral = defaultScalar(kMidpointRole);
    if (midpoint() == neutral)
        return true;
    return session_.setParameterValue(*id, neutral, tr("Reset Midpoint"));
}

QMenu* ColorizeRampBar::makeContextMenu() {
    auto* menu = kit::makeMenu(this);
    auto* reset = menu->addAction(tr("Reset Midpoint"));
    reset->setObjectName(QStringLiteral("colorizeRampReset"));
    reset->setEnabled(isEnabled() && ramp_ && midpoint() != defaultScalar(kMidpointRole));
    connect(reset, &QAction::triggered, this, [this] { static_cast<void>(resetMidpoint()); });
    if (ramp_)
        openKeyMenuEntry(*menu, session_, midpointParameter(), QStringLiteral("colorizeRampKey"));
    return menu;
}

void ColorizeRampBar::contextMenuEvent(QContextMenuEvent* event) {
    if (drag_) {
        event->accept();
        return;
    }
    const std::unique_ptr<QMenu> menu(makeContextMenu());
    menu->exec(event->globalPos());
    event->accept();
}

void ColorizeRampBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ColorizeRampBar::focusOutEvent(QFocusEvent* event) {
    cancelDrag();
    QWidget::focusOutEvent(event);
    update();
}

void ColorizeRampBar::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void ColorizeRampBar::cancelDrag() {
    pressedStop_.reset();
    if (!drag_)
        return;
    const auto id = drag_->parameter;
    drag_.reset();
    if (session_.isValueEditing(id))
        session_.cancelValueEdit();
    update();
}

// --- ColorizeCardWidget -------------------------------------------------------------------------

ColorizeCardWidget::ColorizeCardWidget(CompositionSession& session, const document::NodeId node,
                                       const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeColorize") : QStringLiteral("propertiesColorize"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    amount_ = new GradingValueRow(session_, node_, "amount", document::kColorizeAmountSchemaKey,
                                  tr("Amount"), 1.0, this);
    amount_->setDescription(tr("How much of the colorized result replaces the original colour."));
    GradingSliderSpec amountSlider;
    amountSlider.minimum = 0.0;
    amountSlider.maximum = 1.0;
    amountSlider.neutral = 1.0;
    amountSlider.ticks = 0.25;
    amountSlider.gradient = greyToColourStops(0.95);
    amount_->setSlider(amountSlider);

    ramp_ = new ColorizeRampBar(session_, node_, this);
    GradientParameterRow::Spec gradient;
    gradient.role = std::string(kGradientRole);
    gradient.colorSchemaKey = std::string(document::kColorizeShadowsSchemaKey);
    gradient.name = tr("Gradient");
    gradient.description = tr("The colours the input maps through, darkest input on the left.");
    gradient.objectPrefix = QStringLiteral("colorizeGradient");
    // A node saved before the gradient: the shadows/midtones/highlights ramp it renders with.
    gradient.fallback = [this]() -> std::optional<document::GradientValue> {
        const auto colour = [this](const std::string_view role) -> std::optional<core::Color4d> {
            const auto id = nodeRoleParameter(session_, node_, role);
            const auto live = id ? session_.liveValue(*id) : std::nullopt;
            const auto* value = live ? std::get_if<core::Color4d>(&*live) : nullptr;
            return value ? std::optional<core::Color4d>(*value) : std::nullopt;
        };
        const auto shadows = colour("shadows");
        const auto midtones = colour("midtones");
        const auto highlights = colour("highlights");
        const auto midpointId = nodeRoleParameter(session_, node_, kMidpointRole);
        const double midpoint =
            midpointId ? liveScalar(session_, *midpointId).value_or(defaultScalar(kMidpointRole))
                       : defaultScalar(kMidpointRole);
        if (!shadows || !midtones || !highlights)
            return document::defaultColorizeGradient();
        return document::GradientValue{{{0.0, *shadows},
                                        {std::clamp(midpoint, kMidpointMinimum, kMidpointMaximum),
                                         *midtones},
                                        {1.0, *highlights}}};
    };
    gradient_ = new GradientParameterRow(session_, node_, std::move(gradient), this);
    // #543: the stops' rows open, close and follow their links.
    connect(gradient_, &GradientParameterRow::layoutChanged, this, [this] {
        place();
        emit rowsChanged();
    });
    ramp_->setStopClicked([this](const std::string& role) {
        if (auto* row = colorRow(role); row && row->chip()->isEnabled())
            row->chip()->openPicker();
    });

    const auto colour = [&](const char* role, const std::string_view schemaKey, QString name,
                            QString description) {
        auto* row = new ColorizeColorRow(session_, node_, role, schemaKey, std::move(name), this);
        row->setDescription(description);
        colours_.push_back(row);
    };
    colour("fillColor", document::kColorizeColorSchemaKey, tr("Fill color"),
           tr("The flat colour every visible pixel takes; the alpha is kept."));
    colour("shadows", document::kColorizeShadowsSchemaKey, tr("Shadows"),
           tr("The colour black maps to."));
    colour("midtones", document::kColorizeMidtonesSchemaKey, tr("Midtones"),
           tr("The colour the midpoint maps to."));
    colour("highlights", document::kColorizeHighlightsSchemaKey, tr("Highlights"),
           tr("The colour white maps to."));

    const auto value = [&](const char* role, const std::string_view schemaKey, QString name,
                           QString description, const std::optional<GradingSliderSpec>& slider) {
        auto* row = new GradingValueRow(session_, node_, role, schemaKey, std::move(name),
                                        defaultScalar(role), this);
        row->setDescription(description);
        if (slider)
            row->setSlider(*slider);
        values_.push_back(row);
    };
    value("midpoint", document::kColorizeMidpointSchemaKey, tr("Midpoint"),
          tr("Where the midtones sit along the gradient; 0.18 is scene-linear mid grey."),
          std::nullopt);
    GradingSliderSpec repetitions;
    repetitions.minimum = 0.0;
    repetitions.maximum = 8.0;
    repetitions.neutral = 1.0;
    repetitions.ticks = 1.0;
    repetitions.gradient = jogStops();
    value("repetitions", document::kColorizeRepetitionsSchemaKey, tr("Repetitions"),
          tr("How many times the gradient spans the input's 0 to 1."), repetitions);
    GradingSliderSpec phase;
    phase.minimum = 0.0;
    phase.maximum = 1.0;
    phase.neutral = 0.0;
    phase.ticks = 0.25;
    phase.gradient = jogStops();
    value("phase", document::kColorizePhaseSchemaKey, tr("Phase (cycles)"),
          tr("Shifts the gradient along the input, in whole gradients."), phase);
    for (const auto& weight : weightRows()) {
        GradingSliderSpec slider;
        slider.minimum = -2.0;
        slider.maximum = 3.0;
        slider.neutral = defaultScalar(weight.role);
        slider.ticks = 1.0;
        slider.gradient = {{0.0, clampedDisplay({0.03, 0.03, 0.03})},
                           {0.4, clampedDisplay({0.2, 0.2, 0.2})},
                           {1.0, clampedDisplay(weight.hue)}};
        value(weight.role.data(), weight.schemaKey,
              QCoreApplication::translate("Colorize", weight.name),
              tr("How bright the %1 become in the grey.")
                  .arg(QCoreApplication::translate("Colorize", weight.name).toLower()),
              slider);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &ColorizeCardWidget::refresh);
    refresh();
}

ColorizeColorRow* ColorizeCardWidget::colorRow(const std::string_view role) const {
    const auto found =
        std::ranges::find_if(colours_, [&](const auto* row) { return row->role() == role; });
    return found == colours_.end() ? nullptr : *found;
}

GradingValueRow* ColorizeCardWidget::valueRow(const std::string_view role) const {
    if (role == "amount")
        return amount_;
    const auto found =
        std::ranges::find_if(values_, [&](const auto* row) { return row->role() == role; });
    return found == values_.end() ? nullptr : *found;
}

std::vector<QWidget*> ColorizeCardWidget::shownRows() const {
    std::vector<QWidget*> rows{amount_};
    const auto add = [&](QWidget* row) {
        if (row != nullptr)
            rows.push_back(row);
    };
    switch (mode_) {
    case document::kColorizeFill:
        add(colorRow("fillColor"));
        break;
    case document::kColorizeTint:
        add(ramp_);
        add(colorRow("shadows"));
        add(colorRow("highlights"));
        break;
    case document::kColorizeTritone:
        add(ramp_);
        add(colorRow("shadows"));
        add(colorRow("midtones"));
        add(colorRow("highlights"));
        add(valueRow("midpoint"));
        break;
    case document::kColorizeGradientMap:
        // #541: the multi-stop gradient replaces the three-colour ramp.
        add(gradient_);
        add(valueRow("repetitions"));
        add(valueRow("phase"));
        break;
    case document::kColorizeBlackAndWhite:
        for (const auto& weight : weightRows())
            add(valueRow(weight.role));
        break;
    default:
        break;
    }
    return rows;
}

int ColorizeCardWidget::rowHeight(const QWidget* row) const {
    if (row == ramp_)
        return ramp_->heightHint();
    if (row == gradient_)
        return gradient_->heightHint();
    if (const auto* value = qobject_cast<const GradingValueRow*>(row))
        return value->heightHint();
    return control();
}

bool ColorizeCardWidget::showsRole(const std::string_view role) const {
    // The mode always shows; the Gradient Map's two choices show in Gradient Map. Every other
    // value is drawn by the widget in its mode and kept behind it otherwise.
    if (role == kModeRole)
        return true;
    if (role == "source" || role == "cycle")
        return mode_ == document::kColorizeGradientMap;
    return false;
}

std::optional<int> ColorizeCardWidget::socketY(const std::string_view role) const {
    for (auto* row : shownRows()) {
        // #543: a gradient stop's colour or position row.
        if (row == gradient_)
            if (const auto at = gradient_->socketY(role))
                return gradient_->y() + *at;
        if (const auto* value = qobject_cast<const GradingValueRow*>(row);
            value && value->role() == role)
            return value->y() + value->socketY();
        if (const auto* colour = qobject_cast<const ColorizeColorRow*>(row);
            colour && colour->role() == role)
            return colour->y() + colour->socketY();
    }
    return std::nullopt;
}

int ColorizeCardWidget::heightAt(const int) const {
    int height = 0;
    for (const auto* row : shownRows())
        height += rowHeight(row) + small();
    return std::max(0, height - small());
}

int ColorizeCardWidget::minimumWidthHint() const {
    int width = kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
    // Every row of every mode, so the card keeps its width when the mode changes.
    width = std::max(width, amount_->minimumWidthHint());
    width = std::max(width, gradient_->minimumWidthHint());
    for (const auto* row : colours_)
        width = std::max(width, row->minimumWidthHint());
    for (const auto* row : values_)
        width = std::max(width, row->minimumWidthHint());
    return width;
}

QSize ColorizeCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void ColorizeCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void ColorizeCardWidget::place() {
    const auto rows = shownRows();
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = std::max(amount_->labelWidthHint(), gradient_->labelWidthHint());
    for (const auto* row : colours_)
        labels = std::max(labels, row->labelWidthHint());
    for (const auto* row : values_)
        labels = std::max(labels, row->labelWidthHint());
    amount_->setLabelWidth(labels);
    for (auto* row : colours_) {
        row->setLabelWidth(labels);
        row->setVisible(std::ranges::find(rows, row) != rows.end());
    }
    for (auto* row : values_) {
        row->setLabelWidth(labels);
        row->setVisible(std::ranges::find(rows, row) != rows.end());
    }
    ramp_->setVisible(std::ranges::find(rows, ramp_) != rows.end());
    gradient_->setLabelWidth(labels);
    gradient_->setVisible(std::ranges::find(rows, gradient_) != rows.end());
    int y = 0;
    for (auto* row : rows) {
        const int h = rowHeight(row);
        row->setGeometry(0, y, width(), h);
        y += h + small();
    }
}

void ColorizeCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    std::int64_t mode = document::kColorizeTint;
    if (const auto id = nodeRoleParameter(session_, node_, kModeRole))
        if (const auto value = session_.liveValue(*id))
            if (const auto* stored = std::get_if<std::int64_t>(&*value))
                mode = *stored;
    const bool changed = mode != mode_;
    mode_ = mode;
    amount_->refresh(locked);
    for (auto* row : colours_)
        row->refresh(locked);
    for (auto* row : values_)
        row->refresh(locked);
    gradient_->refresh(locked);
    // The ramp bar draws the colours and the midpoint the node reads.
    const auto colourOf = [&](const std::string_view role) {
        const auto* row = colorRow(role);
        const auto value = row ? row->value() : std::nullopt;
        return value ? std::array{(*value)[0], (*value)[1], (*value)[2]} : std::array<double, 3>{};
    };
    std::vector<ColorizeRampBar::Stop> stops;
    const bool ramp = mode_ == document::kColorizeTritone;
    stops.push_back({"shadows", 0.0, colourOf("shadows")});
    if (ramp)
        stops.push_back({"midtones",
                         std::clamp(ramp_->midpoint(), kMidpointMinimum, kMidpointMaximum),
                         colourOf("midtones")});
    stops.push_back({"highlights", 1.0, colourOf("highlights")});
    ramp_->setStops(std::move(stops), ramp);
    // The midpoint's own editability is checked when its stop is dragged; the stops still open
    // their colours' pickers when the midpoint is linked.
    ramp_->setEnabled(!locked);
    if (changed) {
        place();
        updateGeometry();
        emit rowsChanged();
    }
}

} // namespace bloom::ui

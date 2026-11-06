#include <bloom/ui/grading_controls.hpp>

#include "properties_value_edits.hpp"
#include "value_presentation.hpp"

#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/grade_curve.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <memory>

namespace bloom::ui {
namespace {

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QColor display(const std::array<double, 3>& rgb) {
    return QColor::fromRgbF(static_cast<float>(std::clamp(rgb[0], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[2], 0.0, 1.0)));
}

} // namespace

std::optional<document::ParameterId> nodeRoleParameter(const CompositionSession& session,
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

std::optional<double> liveScalar(const CompositionSession& session,
                                 const document::ParameterId parameter) {
    const auto value = session.liveValue(parameter);
    const auto* scalar = value ? std::get_if<double>(&*value) : nullptr;
    return scalar ? std::optional(*scalar) : std::nullopt;
}

bool scalarEditable(const CompositionSession& session, const document::ParameterId id) {
    const auto* composition = session.composition();
    const auto* parameter = composition ? composition->parameters().find(id) : nullptr;
    return parameter != nullptr && !composition->parameterLocked(id) &&
           !std::holds_alternative<document::DriverBindingSource>(parameter->source);
}

QGradientStops darkToLightStops() {
    return {{0.0, display({0.04, 0.04, 0.04})}, {1.0, display({0.92, 0.92, 0.92})}};
}

QGradientStops jogStops() {
    return {{0.0, display({0.07, 0.07, 0.07})}, {1.0, display({0.52, 0.52, 0.52})}};
}

QGradientStops greyToColourStops(const double neutralShare) {
    // A warm hue as it looks at saturation 0 (grey), at 1 (as it is) and beyond (more vivid).
    const auto vivid = runtime::gradeHueColor(0.05);
    const double luma = 0.213 * vivid[0] + 0.715 * vivid[1] + 0.072 * vivid[2];
    const auto at = [&](const double saturation) {
        return display({luma + (vivid[0] - luma) * saturation, luma + (vivid[1] - luma) * saturation,
                        luma + (vivid[2] - luma) * saturation});
    };
    const double share = std::clamp(neutralShare, 0.05, 0.95);
    const double most = 1.0 / share;
    return {{0.0, at(0.0)}, {share, at(1.0)}, {1.0, at(most)}};
}

QGradientStops temperatureStops() {
    return {{0.0, display({0.30, 0.52, 0.95})},
            {0.5, display({0.62, 0.62, 0.62})},
            {1.0, display({0.98, 0.66, 0.22})}};
}

QGradientStops tintStops() {
    return {{0.0, display({0.30, 0.80, 0.34})},
            {0.5, display({0.62, 0.62, 0.62})},
            {1.0, display({0.88, 0.34, 0.86})}};
}

// --- GradingValueRow ----------------------------------------------------------------------------

GradingValueRow::GradingValueRow(CompositionSession& session, const document::NodeId node,
                                 std::string role, const std::string_view schemaKey, QString name,
                                 const double neutral, QWidget* parent)
    : QWidget(parent), session_(session), node_(node), role_(std::move(role)),
      name_(std::move(name)), neutral_(neutral) {
    const auto objectRole = QString::fromStdString(role_);
    setObjectName(QStringLiteral("gradingRow.%1").arg(objectRole));
    label_ = new kit::KLabel(name_, this, kit::TypeRole::UiSmall);
    label_->setObjectName(QStringLiteral("gradingRowLabel.%1").arg(objectRole));
    label_->setTone(kit::Color::Muted);
    label_->installEventFilter(this);
    field_ = new kit::KValueField(this);
    field_->setObjectName(QStringLiteral("gradingRowField.%1").arg(objectRole));
    field_->setCompact(true);
    field_->setMaximumWidth(QWIDGETSIZE_MAX);
    field_->setOutlinedAtRest(true);
    configureValueField(*field_, schemaKey, document::ParameterValueKind::Float64, role_);
    field_->setAccessibleName(name_);
    field_->setDefaultValue(neutral_ * field_->displayScale());
    field_->setResetText(tr("Reset %1").arg(name_));
    diamond_ = new KeyframeDiamond(session_, role_, this);
    diamond_->setObjectName(QStringLiteral("gradingRowDiamond"));
    keyAction_ = new QAction(tr("Add Key"), this);
    keyAction_->setObjectName(QStringLiteral("gradingRowKey"));
    connect(keyAction_, &QAction::triggered, this, [this] {
        if (const auto id = parameter())
            static_cast<void>(session_.toggleKeyframeForParameter(*id));
    });
    resetAction_ = new QAction(tr("Reset %1").arg(name_), this);
    resetAction_->setObjectName(QStringLiteral("gradingRowReset"));
    connect(resetAction_, &QAction::triggered, this, [this] { static_cast<void>(reset()); });
    field_->addAction(keyAction_);
    properties::bindValueEdit(session_, *field_,
                              [this] { return parameter().value_or(document::ParameterId{}); });
    connect(field_, &kit::KValueField::valueChanged, this,
            [this](const double shown) { write(shown / field_->displayScale()); });
}

std::optional<document::ParameterId> GradingValueRow::parameter() const {
    return nodeRoleParameter(session_, node_, role_);
}

void GradingValueRow::setSlider(const GradingSliderSpec& spec) {
    if (slider_ == nullptr) {
        slider_ = new kit::KSlider(this);
        slider_->setObjectName(QStringLiteral("gradingRowSlider.%1")
                                   .arg(QString::fromStdString(role_)));
        slider_->setAccessibleName(name_);
        slider_->setResetText(tr("Reset %1").arg(name_));
        slider_->addAction(keyAction_);
        properties::bindValueEdit(session_, *slider_,
                                  [this] { return parameter().value_or(document::ParameterId{}); });
        connect(slider_, &kit::KSlider::valueChanged, this,
                [this](const double value) { write(value); });
    }
    slider_->setDragMode(spec.mode);
    {
        // Setting the range clamps the thumb, which is not an edit: a soft minimum above zero must
        // not write itself into the document. The next refresh reads the value.
        const QSignalBlocker blocker(slider_);
        slider_->setRange(spec.minimum, spec.maximum);
        const auto presentation = field_->presentation();
        slider_->setLimits(presentation.minimum / presentation.displayScale,
                           presentation.maximum / presentation.displayScale);
    }
    slider_->setNeutral(spec.neutral);
    slider_->setTickInterval(spec.ticks);
    slider_->setTrackGradient(spec.gradient);
    slider_->setJogStep(spec.jogStep);
    slider_->setDefaultValue(neutral_);
    widthShare_ = std::clamp(spec.widthShare, 0.1, 1.0);
    place();
}

void GradingValueRow::setShowsSign(const bool shows) { field_->setShowsSign(shows); }

void GradingValueRow::setDiamondShown(const bool shown) {
    diamondShown_ = shown;
    place();
    refresh(false);
}

void GradingValueRow::setDescription(const QString& description) {
    label_->setToolTip(description);
    label_->setAccessibleDescription(description);
    field_->setAccessibleDescription(description);
    if (slider_ != nullptr)
        slider_->setAccessibleDescription(description);
}

void GradingValueRow::setValueToolTip(std::function<QString(double)> tooltip) {
    valueToolTip_ = std::move(tooltip);
}

void GradingValueRow::write(const double value) {
    if (const auto id = parameter())
        static_cast<void>(session_.setParameterValue(*id, value, tr("Set %1").arg(name_)));
}

void GradingValueRow::setLabelWidth(const int width) {
    labelWidth_ = std::max(0, width);
    place();
}

int GradingValueRow::labelWidthHint() const {
    return QFontMetrics(label_->font()).horizontalAdvance(name_) + small();
}

int GradingValueRow::heightHint() const {
    return slider_ == nullptr ? control() : 2 * control();
}

int GradingValueRow::minimumWidthHint() const {
    // The label, a field wide enough for its widest reading with its unit, and the diamond.
    const QFontMetrics value(kit::font(kit::TypeRole::Value));
    const auto presentation = field_->presentation();
    const double widest = std::max(std::abs(presentation.softMinimum), std::abs(presentation.softMaximum));
    QString text = QString::number(std::floor(widest) + 0.0, 'f', presentation.decimals);
    if (text.size() < 2 + presentation.decimals)
        text.prepend(QLatin1Char('0'));
    text.prepend(QLatin1Char('-'));
    const int unit = presentation.unit.isEmpty()
                         ? 0
                         : value.horizontalAdvance(presentation.unit) + kit::px(kit::Spacing::XS);
    // The field's text inset on both sides (2 x Spacing::S).
    const int fieldWidth = value.horizontalAdvance(text) + unit + kit::px(kit::Spacing::L);
    return std::max(labelWidth_, labelWidthHint()) + small() + fieldWidth +
           (diamondShown_ ? small() + kit::px(kit::Size::IconSmall) : 0);
}

int GradingValueRow::socketY() const { return control() / 2; }

void GradingValueRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place();
}

void GradingValueRow::place() {
    const int w = width();
    const int labelWidth = std::max(labelWidth_, labelWidthHint());
    const int diamond = diamondShown_ ? kit::px(kit::Size::IconSmall) + small() : 0;
    label_->setGeometry(0, 0, labelWidth, control());
    const int fieldLeft = labelWidth + small();
    field_->setGeometry(fieldLeft, 0, std::max(0, w - fieldLeft - diamond), control());
    diamond_->move(w - kit::px(kit::Size::IconSmall), (control() - diamond_->height()) / 2);
    if (slider_ != nullptr) {
        const int sliderWidth = static_cast<int>(std::round(w * widthShare_));
        slider_->setGeometry((w - sliderWidth) / 2, control(), sliderWidth, control());
    }
}

bool GradingValueRow::reset() {
    const auto id = parameter();
    const auto current = id ? liveScalar(session_, *id) : std::nullopt;
    if (!id || !current)
        return false;
    if (*current == neutral_)
        return true;
    return session_.setParameterValue(*id, neutral_, tr("Reset %1").arg(name_));
}

QMenu* GradingValueRow::makeContextMenu() {
    auto* menu = kit::makeMenu(this);
    resetAction_->setEnabled(field_->isEnabled() && field_->isNonDefault());
    menu->addAction(resetAction_);
    menu->addAction(keyAction_);
    return menu;
}

bool GradingValueRow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == label_) {
        // The name resets its value on a double-click and offers the value's menu, as a value
        // field's label does.
        if (event->type() == QEvent::MouseButtonDblClick &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && field_->isEnabled()) {
            static_cast<void>(reset());
            return true;
        }
        if (event->type() == QEvent::ContextMenu) {
            const std::unique_ptr<QMenu> menu(makeContextMenu());
            menu->exec(static_cast<QContextMenuEvent*>(event)->globalPos());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GradingValueRow::refresh(const bool locked) {
    const auto id = parameter();
    const bool enabled = id && !locked && scalarEditable(session_, *id);
    field_->setEnabled(enabled);
    if (slider_ != nullptr)
        slider_->setEnabled(enabled);
    const auto* composition = session_.composition();
    const auto* record = composition && id ? composition->parameters().find(*id) : nullptr;
    const bool linked =
        record != nullptr && std::holds_alternative<document::DriverBindingSource>(record->source);
    const auto value = id && !linked ? liveScalar(session_, *id) : std::nullopt;
    if (linked && !field_->hasLiveEdit()) {
        // A linked value is the link's to set: the row says so rather than show a stale number.
        const QSignalBlocker blocker(field_);
        field_->setSpecialValueText(tr("Linked"));
        field_->setValue(field_->minimum());
    } else if (!field_->hasLiveEdit()) {
        field_->setSpecialValueText({});
    }
    if (value) {
        if (!field_->hasLiveEdit()) {
            const QSignalBlocker blocker(field_);
            field_->setValue(*value * field_->displayScale());
        }
        if (slider_ != nullptr && !slider_->hasLiveEdit()) {
            const QSignalBlocker blocker(slider_);
            slider_->setValue(*value);
        }
        if (valueToolTip_)
            field_->setToolTip(valueToolTip_(*value));
    }
    diamond_->setParameterId(id.value_or(document::ParameterId{}));
    diamond_->refresh();
    if (!diamondShown_)
        diamond_->setVisible(false);
    const auto state =
        id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    keyAction_->setVisible(state != KeyframeDiamondState::Unsupported);
    keyAction_->setEnabled(!locked);
    keyAction_->setText(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                        : tr("Add Key"));
}

} // namespace bloom::ui

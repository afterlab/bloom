#include <bloom/ui/gradient_parameter_row.hpp>

#include "properties_sections.hpp"
#include "properties_value_edits.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/gradient_editor.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalBlocker>

#include <algorithm>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
namespace {

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QString named(const QString& prefix, const char* suffix) {
    return prefix + QLatin1StringView(suffix);
}

} // namespace

GradientParameterRow::GradientParameterRow(CompositionSession& session,
                                           const document::NodeId node, Spec spec,
                                           QWidget* parent)
    : QWidget(parent), session_(session), node_(node), spec_(std::move(spec)) {
    const auto& prefix = spec_.objectPrefix;
    setObjectName(named(prefix, "Row"));
    label_ = new kit::KLabel(spec_.name, this, kit::TypeRole::UiSmall);
    label_->setObjectName(named(prefix, "Label"));
    label_->setTone(kit::Color::Muted);
    label_->setToolTip(spec_.description);
    label_->installEventFilter(this);
    editor_ = new kit::KGradientEditor(this);
    editor_->setObjectName(named(prefix, "Editor"));
    editor_->setAccessibleName(spec_.name);
    editor_->setStopLimits(1, document::kMaximumGradientStops);
    editor_->setColorConverter(session_.colorConverter(spec_.colorSchemaKey));
    diamond_ = new KeyframeDiamond(session_, spec_.role, this);
    diamond_->setObjectName(named(prefix, "Diamond"));
    stopLabel_ = new kit::KLabel(tr("Stop"), this, kit::TypeRole::UiSmall);
    stopLabel_->setObjectName(named(prefix, "StopLabel"));
    stopLabel_->setTone(kit::Color::Muted);
    stopLabel_->setToolTip(tr("The selected stop's colour and where it sits along the gradient."));
    chip_ = new kit::KColorChip(this);
    chip_->setObjectName(named(prefix, "StopColor"));
    chip_->setAccessibleName(tr("Stop colour"));
    chip_->setControlSize(kit::KColorChip::ControlSize::Compact);
    position_ = new kit::KValueField(this);
    position_->setObjectName(named(prefix, "StopPosition"));
    position_->setAccessibleName(tr("Stop position"));
    position_->setCompact(true);
    position_->setOutlinedAtRest(true);
    position_->setRange(0.0, 100.0);
    position_->setDecimals(1);
    position_->setSingleStep(1.0);
    position_->setUnit(QStringLiteral("%"));
    keyAction_ = new QAction(tr("Add Key"), this);
    keyAction_->setObjectName(named(prefix, "Key"));
    connect(keyAction_, &QAction::triggered, this, [this] {
        if (const auto id = ensureParameter())
            static_cast<void>(session_.toggleKeyframeForParameter(*id));
    });
    editor_->addAction(keyAction_);
    const auto bound = [this] { return ensureParameter().value_or(document::ParameterId{}); };
    properties::bindValueEdit(session_, *editor_, bound);
    properties::bindValueEdit(session_, *chip_, bound);
    properties::bindValueEdit(session_, *position_, bound);
    connect(editor_, &kit::KGradientEditor::stopsChanged, this, [this] {
        // Only a gesture writes; a projection or a cancelled gesture's restore does not.
        if (const auto id = parameter(); id && session_.isValueEditing(*id))
            write(editorValue());
    });
    connect(editor_, &kit::KGradientEditor::selectedStopChanged, this, [this] {
        const auto* composition = session_.composition();
        refresh(composition == nullptr || composition->nodeLocked(node_));
    });
    connect(editor_, &kit::KGradientEditor::stopColorRequested, this, [this](std::size_t) {
        if (chip_->isEnabled())
            chip_->openPicker();
    });
    connect(chip_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& colour) {
        const auto selected = editor_->selectedStop();
        if (refreshing_ || !selected)
            return;
        auto value = editorValue();
        if (*selected >= value.stops.size())
            return;
        value.stops[*selected].color = {static_cast<double>(colour.red),
                                        static_cast<double>(colour.green),
                                        static_cast<double>(colour.blue),
                                        static_cast<double>(colour.alpha)};
        write(std::move(value));
    });
    connect(position_, &kit::KValueField::valueChanged, this, [this](const double percent) {
        const auto selected = editor_->selectedStop();
        if (refreshing_ || !selected)
            return;
        auto value = editorValue();
        if (*selected >= value.stops.size())
            return;
        value.stops[*selected].position = std::clamp(percent / 100.0, 0.0, 1.0);
        write(std::move(value));
    });
}

std::optional<document::ParameterId> GradientParameterRow::parameter() const {
    return nodeRoleParameter(session_, node_, spec_.role);
}

std::optional<document::GradientValue> GradientParameterRow::defaultValue() const {
    const auto* composition = session_.composition();
    const auto* record = composition ? composition->graph().findNode(node_) : nullptr;
    const auto* definition =
        record ? document::builtInNodeDefinitions().find(record->typeId, record->schemaVersion)
               : nullptr;
    if (definition == nullptr)
        return std::nullopt;
    const auto found =
        std::ranges::find(definition->parameters, spec_.role, &document::ParameterDefinition::role);
    const auto* gradient = found == definition->parameters.end()
                               ? nullptr
                               : std::get_if<document::GradientValue>(&found->defaultValue);
    return gradient ? std::optional<document::GradientValue>(*gradient) : std::nullopt;
}

std::optional<document::GradientValue> GradientParameterRow::value() const {
    if (const auto id = parameter()) {
        const auto live = session_.liveValue(*id);
        const auto* gradient = live ? std::get_if<document::GradientValue>(&*live) : nullptr;
        return gradient ? std::optional<document::GradientValue>(*gradient) : std::nullopt;
    }
    if (spec_.fallback)
        if (auto fallback = spec_.fallback())
            return fallback;
    return defaultValue();
}

std::optional<document::ParameterId> GradientParameterRow::ensureParameter() {
    if (const auto id = parameter())
        return id;
    const auto seeded = value();
    if (!seeded ||
        !session_.setNodeParameterValue(node_, spec_.role, *seeded, tr("Use Gradient")))
        return std::nullopt;
    return parameter();
}

document::GradientValue GradientParameterRow::editorValue() const {
    document::GradientValue value;
    for (const auto& stop : editor_->stops())
        value.stops.push_back({stop.position,
                               {static_cast<double>(stop.color.red),
                                static_cast<double>(stop.color.green),
                                static_cast<double>(stop.color.blue),
                                static_cast<double>(stop.color.alpha)}});
    return value;
}

void GradientParameterRow::write(document::GradientValue value) {
    std::ranges::stable_sort(value.stops, {}, &document::GradientStop::position);
    if (const auto id = ensureParameter())
        static_cast<void>(session_.setParameterValue(*id, std::move(value), tr("Set Gradient")));
}

void GradientParameterRow::setLabelWidth(const int width) {
    labelWidth_ = std::max(0, width);
    place();
}

int GradientParameterRow::labelWidthHint() const {
    const QFontMetrics metrics(label_->font());
    return std::max(metrics.horizontalAdvance(label_->text()),
                    metrics.horizontalAdvance(stopLabel_->text())) +
           small();
}

int GradientParameterRow::heightHint() const {
    return editor_->sizeHint().height() + small() + control();
}

int GradientParameterRow::minimumWidthHint() const {
    return std::max(labelWidth_, labelWidthHint()) + small() + 3 * control() + small() +
           std::max(kit::px(kit::Size::PropertiesFieldMinWidth),
                    position_->minimumSizeHint().width()) +
           small() + kit::px(kit::Size::IconSmall);
}

void GradientParameterRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place();
}

void GradientParameterRow::place() {
    const int w = width();
    const int labelWidth = std::max(labelWidth_, labelWidthHint());
    const int left = labelWidth + small();
    const int diamond = kit::px(kit::Size::IconSmall) + small();
    const int editorHeight = editor_->sizeHint().height();
    label_->setGeometry(0, 0, labelWidth, control());
    editor_->setGeometry(left, 0, std::max(0, w - left - diamond), editorHeight);
    diamond_->move(w - kit::px(kit::Size::IconSmall), (control() - diamond_->height()) / 2);
    const int y = editorHeight + small();
    stopLabel_->setGeometry(0, y, labelWidth, control());
    const int fieldWidth =
        std::max(kit::px(kit::Size::PropertiesFieldWidth), position_->minimumSizeHint().width());
    const int chipWidth = std::max(0, w - left - diamond - small() - fieldWidth);
    const int chipHeight = std::min(control(), chip_->sizeHint().height());
    chip_->setGeometry(left, y + (control() - chipHeight) / 2, chipWidth, chipHeight);
    position_->setGeometry(left + chipWidth + small(), y, fieldWidth, control());
}

bool GradientParameterRow::reset() {
    const auto neutral = defaultValue();
    if (!neutral)
        return false;
    if (const auto current = value(); current && *current == *neutral && parameter())
        return true;
    if (const auto id = ensureParameter())
        return session_.setParameterValue(*id, *neutral, tr("Reset Gradient"));
    return false;
}

QMenu* GradientParameterRow::makeContextMenu() {
    auto* menu = kit::makeMenu(this);
    auto* reset = menu->addAction(tr("Reset Gradient"));
    reset->setObjectName(named(spec_.objectPrefix, "Reset"));
    reset->setEnabled(editor_->isEnabled());
    connect(reset, &QAction::triggered, this, [this] { static_cast<void>(this->reset()); });
    menu->addAction(keyAction_);
    return menu;
}

bool GradientParameterRow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == label_ && event->type() == QEvent::MouseButtonDblClick &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && editor_->isEnabled()) {
        static_cast<void>(reset());
        return true;
    }
    if (watched == label_ && event->type() == QEvent::ContextMenu) {
        const std::unique_ptr<QMenu> menu(makeContextMenu());
        menu->exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void GradientParameterRow::refresh(const bool locked) {
    const auto id = parameter();
    // A node saved without the gradient is editable too: the first edit binds it.
    const bool enabled = !locked && (!id || scalarEditable(session_, *id));
    editor_->setEnabled(enabled);
    editor_->setColorConverter(session_.colorConverter(spec_.colorSchemaKey));
    const auto gradient = value();
    if (gradient && !editor_->hasLiveEdit()) {
        std::vector<kit::KGradientEditor::Stop> stops;
        for (const auto& stop : gradient->stops)
            stops.push_back({stop.position,
                             kit::KColor::fromRgba(static_cast<float>(stop.color.red),
                                                   static_cast<float>(stop.color.green),
                                                   static_cast<float>(stop.color.blue),
                                                   static_cast<float>(stop.color.alpha),
                                                   kit::ColorSpace::Reference)});
        const QSignalBlocker blocker(editor_);
        editor_->setStops(std::move(stops));
    }
    if (!editor_->selectedStop() && !editor_->stops().empty()) {
        const QSignalBlocker blocker(editor_);
        editor_->setSelectedStop(0);
    }
    const auto selected = editor_->selectedStop();
    const bool stopShown = selected && *selected < editor_->stops().size();
    chip_->setEnabled(enabled && stopShown);
    position_->setEnabled(enabled && stopShown);
    if (stopShown) {
        const auto& stop = editor_->stops()[*selected];
        refreshing_ = true;
        if (!chip_->isPickerOpen() || !session_.valueEditActive())
            properties::refreshColor(session_, spec_.colorSchemaKey,
                                     core::Color4d{static_cast<double>(stop.color.red),
                                                   static_cast<double>(stop.color.green),
                                                   static_cast<double>(stop.color.blue),
                                                   static_cast<double>(stop.color.alpha)},
                                     chip_);
        if (!position_->hasLiveEdit()) {
            const QSignalBlocker blocker(position_);
            position_->setValue(stop.position * 100.0);
        }
        refreshing_ = false;
    }
    diamond_->setParameterId(id.value_or(document::ParameterId{}));
    diamond_->refresh();
    const auto state =
        id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    keyAction_->setText(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                       : tr("Add Key"));
    keyAction_->setEnabled(!locked);
}

} // namespace bloom::ui

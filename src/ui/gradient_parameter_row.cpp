#include <bloom/ui/gradient_parameter_row.hpp>

#include "properties_sections.hpp"
#include "properties_value_edits.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
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
#include <cmath>
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

[[nodiscard]] kit::KColor referenceColor(const core::Color4d& color) {
    return kit::KColor::fromRgba(static_cast<float>(color.red), static_cast<float>(color.green),
                                 static_cast<float>(color.blue), static_cast<float>(color.alpha),
                                 kit::ColorSpace::Reference);
}

[[nodiscard]] core::Color4d documentColor(const kit::KColor& color) {
    return {static_cast<double>(color.red), static_cast<double>(color.green),
            static_cast<double>(color.blue), static_cast<double>(color.alpha)};
}

using document::GradientStopComponent;

[[nodiscard]] QString labelText(const std::string_view label) {
    return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
}

// #546: the editor's bar in the gradient's own blend, through the document's definition.
[[nodiscard]] kit::KColor sampleStops(const std::vector<kit::KGradientEditor::Stop>& stops,
                                      const document::GradientModes modes, const double position) {
    document::GradientValue value;
    value.modes = modes;
    for (const auto& stop : stops)
        value.stops.push_back({stop.position, documentColor(stop.color)});
    return referenceColor(document::gradientColorAt(value, position));
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
    // #546: how the colour blends between stops, as Blender's Color Ramp offers it.
    const auto dropdown = [this, &prefix](const char* suffix, const QString& accessible,
                                          const QString& tip) {
        auto* box = new kit::KDropdown(this);
        box->setObjectName(named(prefix, suffix));
        box->setAccessibleName(accessible);
        box->setControlSize(kit::KDropdown::ControlSize::Dense);
        box->setToolTip(tip);
        return box;
    };
    const auto rowLabel = [this, &prefix](const QString& name, const char* suffix,
                                          const QString& tip) {
        auto* label = new kit::KLabel(name, this, kit::TypeRole::UiSmall);
        label->setObjectName(named(prefix, suffix));
        label->setTone(kit::Color::Muted);
        label->setToolTip(tip);
        return label;
    };
    const auto modeTip = tr("How the colour blends between stops: RGB mixes red, green and blue; "
                            "HSV and HSL turn the hue around the colour wheel.");
    colorModeLabel_ = rowLabel(tr("Color mode"), "ColorModeLabel", modeTip);
    colorMode_ = dropdown("ColorMode", tr("Color mode"), modeTip);
    for (const auto mode : document::kGradientColorModes)
        colorMode_->addItem(labelText(document::gradientColorModeLabel(mode)),
                            static_cast<int>(mode));
    colorMode_->setItemToolTip(0, tr("Mix red, green and blue"));
    colorMode_->setItemToolTip(1, tr("Blend hue, saturation and value"));
    colorMode_->setItemToolTip(2, tr("Blend hue, saturation and lightness"));
    const auto interpolationTip = tr("How the colour moves from one stop to the next");
    interpolationLabel_ = rowLabel(tr("Interpolation"), "InterpolationLabel", interpolationTip);
    interpolation_ = dropdown("Interpolation", tr("Interpolation"), interpolationTip);
    for (const auto mode : document::kGradientInterpolations)
        interpolation_->addItem(labelText(document::gradientInterpolationLabel(mode)),
                                static_cast<int>(mode));
    interpolation_->setItemToolTip(0, tr("Hold each stop's colour up to the next stop"));
    interpolation_->setItemToolTip(1, tr("Blend evenly from stop to stop"));
    interpolation_->setItemToolTip(2, tr("Blend slowly out of and into each stop"));
    interpolation_->setItemToolTip(3, tr("A smooth curve through every stop"));
    interpolation_->setItemToolTip(4, tr("A smooth curve that passes near the stops"));
    interpolation_->setItemToolTip(5, tr("Stay near each stop's colour, then turn quickly"));
    hue_ = dropdown("Hue", tr("Hue direction"),
                    tr("Which way the hue turns around the colour wheel between stops"));
    for (const auto mode : document::kGradientHueDirections)
        hue_->addItem(labelText(document::gradientHueDirectionLabel(mode)), static_cast<int>(mode));
    hue_->setItemToolTip(0, tr("The shorter way around"));
    hue_->setItemToolTip(1, tr("The longer way around"));
    hue_->setItemToolTip(2, tr("Always towards yellow, green and blue (hue rising)"));
    hue_->setItemToolTip(3, tr("Always towards magenta, blue and green (hue falling)"));
    const auto chosen = [this](kit::KDropdown* box, auto apply) {
        connect(box, &kit::KDropdown::activated, this, [this, box, apply](const int index) {
            if (refreshing_)
                return;
            const auto current = value();
            if (!current)
                return;
            auto modes = current->modes;
            apply(modes, box->itemData(index).toInt());
            writeModes(modes);
        });
    };
    chosen(colorMode_, [](document::GradientModes& modes, const int value) {
        modes.colorMode = static_cast<document::GradientColorMode>(value);
    });
    chosen(interpolation_, [](document::GradientModes& modes, const int value) {
        modes.interpolation = static_cast<document::GradientInterpolation>(value);
    });
    chosen(hue_, [](document::GradientModes& modes, const int value) {
        modes.hueDirection = static_cast<document::GradientHueDirection>(value);
    });
    keyAction_ = new QAction(tr("Add Key"), this);
    keyAction_->setObjectName(named(prefix, "Key"));
    connect(keyAction_, &QAction::triggered, this, [this] {
        if (const auto id = ensureParameter())
            static_cast<void>(session_.toggleKeyframeForParameter(*id));
    });
    editor_->addAction(keyAction_);
    // #543: the Values disclosure: its caret turns down while the stops' rows are shown.
    disclosure_ = new kit::KButton(kit::IconId::CaretRight, tr("Values"), this);
    disclosure_->setObjectName(named(prefix, "Values"));
    disclosure_->setVariant(kit::KButton::Variant::Ghost);
    disclosure_->setControlSize(kit::KButton::ControlSize::Compact);
    disclosure_->setAccessibleName(tr("%1 values").arg(spec_.name));
    disclosure_->setToolTip(
        tr("Show each stop's colour and position, with their sockets, to link them"));
    connect(disclosure_, &QAbstractButton::clicked, this, [this] { setValuesShown(!expanded_); });
    const auto bound = [this] { return ensureParameter().value_or(document::ParameterId{}); };
    properties::bindValueEdit(session_, *editor_, bound);
    properties::bindValueEdit(session_, *chip_, bound);
    properties::bindValueEdit(session_, *position_, bound);
    connect(editor_, &kit::KGradientEditor::stopsChanged, this, [this] {
        // Only a gesture writes; a projection or a cancelled gesture's restore does not.
        if (const auto id = parameter(); id && session_.isValueEditing(*id))
            writeStops(editor_->stops());
    });
    connect(&session_, &CompositionSession::drivenValuesChanged, this, [this] {
        const auto* composition = session_.composition();
        refresh(composition == nullptr || composition->nodeLocked(node_));
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
        if (*selected >= editor_->stops().size() || editor_->stops()[*selected].colorLinked)
            return;
        auto stops = editor_->stops();
        stops[*selected].color = referenceColor(documentColor(colour));
        writeStops(std::move(stops));
    });
    connect(position_, &kit::KValueField::valueChanged, this, [this](const double percent) {
        const auto selected = editor_->selectedStop();
        if (refreshing_ || !selected)
            return;
        if (*selected >= editor_->stops().size() || editor_->stops()[*selected].positionLinked)
            return;
        auto stops = editor_->stops();
        stops[*selected].position = std::clamp(percent / 100.0, 0.0, 1.0);
        writeStops(std::move(stops));
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

std::optional<document::ParameterId>
GradientParameterRow::stopLink(const std::uint32_t stop,
                               const document::GradientStopComponent component) const {
    const auto* composition = session_.composition();
    const auto* record = composition ? composition->graph().findNode(node_) : nullptr;
    if (record == nullptr)
        return std::nullopt;
    const auto port = document::gradientStopPortName(spec_.role, stop, component);
    const auto binding =
        std::ranges::find(record->parameters, port, &document::ParameterBinding::role);
    if (binding == record->parameters.end())
        return std::nullopt;
    const auto* link = composition->parameters().find(binding->parameterId);
    return link && std::holds_alternative<document::DriverBindingSource>(link->source)
               ? std::optional(binding->parameterId)
               : std::nullopt;
}

std::optional<document::GradientValue> GradientParameterRow::shownValue() const {
    auto shown = value();
    if (!shown)
        return shown;
    for (std::uint32_t stop = 0; stop < shown->stops.size(); ++stop) {
        for (const auto component :
             {GradientStopComponent::Color, GradientStopComponent::Position}) {
            const auto link = stopLink(stop, component);
            const auto* driven = link ? session_.drivenValue(*link) : nullptr;
            if (driven == nullptr)
                continue;
            if (component == GradientStopComponent::Color) {
                if (const auto* color = std::get_if<core::Color4d>(driven))
                    shown->stops[stop].color = *color;
                continue;
            }
            std::optional<double> position;
            if (const auto* scalar = std::get_if<double>(driven))
                position = *scalar;
            else if (const auto* integer = std::get_if<std::int64_t>(driven))
                position = static_cast<double>(*integer);
            else if (const auto* boolean = std::get_if<bool>(driven))
                position = *boolean ? 1.0 : 0.0;
            if (position && std::isfinite(*position))
                shown->stops[stop].position = std::clamp(*position, 0.0, 1.0);
        }
    }
    return shown;
}

void GradientParameterRow::writeStops(std::vector<kit::KGradientEditor::Stop> stops) {
    // The editor's stops are numbered by the stored gradient they were set from (`projected_`):
    // write the kept ones in that stored order and new ones after them, so a stop keeps its index
    // (and its links) through a move; a removed stop takes its links with it.
    const auto base = projected_ ? *projected_ : document::GradientValue{};
    std::vector<std::size_t> order(stops.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::ranges::stable_sort(order, [&](const std::size_t left, const std::size_t right) {
        const auto& a = stops[left].origin;
        const auto& b = stops[right].origin;
        return a && (!b || *a < *b);
    });
    const auto stopOf = [&](const kit::KGradientEditor::Stop& stop) {
        document::GradientStop stored{stop.position, documentColor(stop.color)};
        // A linked part is the link's: the stop keeps what it was authored with under it.
        if (stop.origin && *stop.origin < base.stops.size()) {
            if (stop.colorLinked)
                stored.color = base.stops[*stop.origin].color;
            if (stop.positionLinked)
                stored.position = base.stops[*stop.origin].position;
        }
        return stored;
    };
    // Equal positions draw in stored order, so the stored order must also be the editor's at
    // a tie; otherwise the stops are stored as the editor shows them.
    const auto sortedMatches = [&](const std::vector<std::size_t>& candidate) {
        std::vector<std::size_t> sorted = candidate;
        std::ranges::stable_sort(sorted, {},
                                 [&](const std::size_t index) { return stops[index].position; });
        std::vector<std::size_t> editor(stops.size());
        for (std::size_t index = 0; index < editor.size(); ++index)
            editor[index] = index;
        std::ranges::stable_sort(editor, {},
                                 [&](const std::size_t index) { return stops[index].position; });
        return sorted == editor;
    };
    if (!sortedMatches(order))
        std::ranges::stable_sort(order, {},
                                 [&](const std::size_t index) { return stops[index].position; });
    document::GradientValue value;
    value.modes = base.modes;
    std::vector<std::optional<std::uint32_t>> newIndexOfOld(base.stops.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        const auto& stop = stops[order[index]];
        value.stops.push_back(stopOf(stop));
        if (stop.origin && *stop.origin < newIndexOfOld.size())
            newIndexOfOld[*stop.origin] = static_cast<std::uint32_t>(index);
    }
    writeStored(std::move(value), std::move(newIndexOfOld));
}

void GradientParameterRow::writeStored(document::GradientValue value,
                                       std::vector<std::optional<std::uint32_t>> newIndexOfOld,
                                       const QString& label) {
    if (const auto id = ensureParameter())
        static_cast<void>(session_.setGradientValue(*id, std::move(value), node_, spec_.role,
                                                    std::move(newIndexOfOld),
                                                    label.isEmpty() ? tr("Set Gradient") : label));
}

void GradientParameterRow::writeModes(const document::GradientModes modes) {
    auto stored = value();
    if (!stored || stored->modes == modes)
        return;
    stored->modes = modes;
    std::vector<std::optional<std::uint32_t>> same(stored->stops.size());
    for (std::uint32_t index = 0; index < same.size(); ++index)
        same[index] = index;
    writeStored(std::move(*stored), std::move(same), tr("Set Gradient Blend"));
}

void GradientParameterRow::setValuesShown(const bool shown) {
    // The caret carries the state; a checked plate would read as a pressed action.
    disclosure_->setAccessibleDescription(shown ? tr("Expanded") : tr("Collapsed"));
    disclosure_->setIconId(shown ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    if (expanded_ == shown)
        return;
    expanded_ = shown;
    const auto* composition = session_.composition();
    refresh(composition == nullptr || composition->nodeLocked(node_));
}

std::optional<int> GradientParameterRow::socketY(const std::string_view port) const {
    const auto stop = document::parseGradientStopPort(port);
    if (!stop || stop->role != spec_.role)
        return std::nullopt;
    for (const auto& row : stopRows_)
        if (row.shown && row.stop == stop->stop && row.component == stop->component)
            return row.y + control() / 2;
    return std::nullopt;
}

void GradientParameterRow::rebuildStopRows(const std::size_t stops) {
    const auto wanted = 2 * stops;
    while (stopRows_.size() > wanted) {
        auto& row = stopRows_.back();
        delete row.label;
        delete row.chip;
        delete row.field;
        delete row.link;
        stopRows_.pop_back();
    }
    const auto bound = [this] { return ensureParameter().value_or(document::ParameterId{}); };
    while (stopRows_.size() < wanted) {
        const auto index = stopRows_.size();
        StopRow row;
        row.stop = static_cast<std::uint32_t>(index / 2);
        row.component =
            index % 2 == 0 ? GradientStopComponent::Color : GradientStopComponent::Position;
        const bool colour = row.component == GradientStopComponent::Color;
        const auto number = static_cast<int>(row.stop) + 1;
        const auto prefix = spec_.objectPrefix +
                            QStringLiteral("Stop%1%2")
                                .arg(number)
                                .arg(colour ? QStringLiteral("Color") : QStringLiteral("Position"));
        const auto name =
            colour ? tr("Stop %1 color").arg(number) : tr("Stop %1 position").arg(number);
        row.label = new kit::KLabel(name, this, kit::TypeRole::UiSmall);
        row.label->setObjectName(prefix + QStringLiteral("Label"));
        row.label->setTone(kit::Color::Muted);
        row.label->setToolTip(
            colour ? tr("Stop %1's colour; link a colour to its socket to drive it.").arg(number)
                   : tr("Where stop %1 sits along the gradient; link a number to "
                        "its socket to drive it.")
                         .arg(number));
        if (colour) {
            row.chip = new kit::KColorChip(this);
            row.chip->setObjectName(prefix);
            row.chip->setAccessibleName(name);
            row.chip->setControlSize(kit::KColorChip::ControlSize::Compact);
            properties::bindValueEdit(session_, *row.chip, bound);
            connect(row.chip, &kit::KColorChip::colorChanged, this,
                    [this, stop = row.stop](const kit::KColor& picked) {
                        if (refreshing_ || !projected_ || stop >= projected_->stops.size())
                            return;
                        auto value = *projected_;
                        value.stops[stop].color = documentColor(picked);
                        std::vector<std::optional<std::uint32_t>> same(value.stops.size());
                        for (std::uint32_t index = 0; index < same.size(); ++index)
                            same[index] = index;
                        writeStored(std::move(value), std::move(same));
                    });
        } else {
            row.field = new kit::KValueField(this);
            row.field->setObjectName(prefix);
            row.field->setAccessibleName(name);
            row.field->setCompact(true);
            row.field->setOutlinedAtRest(true);
            row.field->setRange(0.0, 100.0);
            row.field->setDecimals(1);
            row.field->setSingleStep(1.0);
            row.field->setUnit(QStringLiteral("%"));
            properties::bindValueEdit(session_, *row.field, bound);
            connect(row.field, &kit::KValueField::valueChanged, this,
                    [this, stop = row.stop](const double percent) {
                        if (refreshing_ || !projected_ || stop >= projected_->stops.size())
                            return;
                        auto value = *projected_;
                        value.stops[stop].position = std::clamp(percent / 100.0, 0.0, 1.0);
                        std::vector<std::optional<std::uint32_t>> same(value.stops.size());
                        for (std::uint32_t index = 0; index < same.size(); ++index)
                            same[index] = index;
                        writeStored(std::move(value), std::move(same));
                    });
        }
        row.link = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
        row.link->setObjectName(prefix + QStringLiteral("Link"));
        row.link->setTone(kit::Color::Muted);
        for (QWidget* widget : {static_cast<QWidget*>(row.label), static_cast<QWidget*>(row.chip),
                                static_cast<QWidget*>(row.field), static_cast<QWidget*>(row.link)})
            if (widget != nullptr)
                widget->show();
        stopRows_.push_back(row);
    }
}

int GradientParameterRow::stopRowsTop() const {
    // The bar, the Stop row, the Color mode and Interpolation (or Hue) rows, the Values button.
    return editor_->sizeHint().height() + 4 * (small() + control());
}

void GradientParameterRow::setLabelWidth(const int width) {
    labelWidth_ = std::max(0, width);
    place();
}

int GradientParameterRow::labelWidthHint() const {
    const QFontMetrics metrics(label_->font());
    int widest = std::max(metrics.horizontalAdvance(label_->text()),
                          metrics.horizontalAdvance(stopLabel_->text()));
    // The blend row's label reads Interpolation or Hue: room for either, so it never shifts.
    for (const auto& text : {colorModeLabel_->text(), tr("Interpolation"), tr("Hue")})
        widest = std::max(widest, metrics.horizontalAdvance(text));
    for (const auto& row : stopRows_)
        if (row.shown)
            widest = std::max(widest, metrics.horizontalAdvance(row.label->text()));
    return widest + small();
}

int GradientParameterRow::heightHint() const {
    int height = stopRowsTop();
    for (const auto& row : stopRows_)
        if (row.shown)
            height += small() + control();
    return height;
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
    // #546: the blend rows; the Hue dropdown takes the Interpolation one's place for HSV and HSL.
    const int modeWidth = std::max(0, w - left - diamond);
    const int modeY = y + control() + small();
    colorModeLabel_->setGeometry(0, modeY, labelWidth, control());
    colorMode_->setGeometry(left, modeY, modeWidth, control());
    const int curveY = modeY + control() + small();
    interpolationLabel_->setGeometry(0, curveY, labelWidth, control());
    interpolation_->setGeometry(left, curveY, modeWidth, control());
    hue_->setGeometry(left, curveY, modeWidth, control());
    const int valuesY = curveY + control() + small();
    disclosure_->setGeometry(left, valuesY, std::max(disclosure_->sizeHint().width(), 0),
                             control());
    // #543: each shown stop row: its name, then its chip or field, or the link that drives it.
    int rowY = stopRowsTop();
    const int controlWidth = std::max(0, w - left - diamond);
    for (auto& row : stopRows_) {
        if (!row.shown) {
            row.label->hide();
            if (row.chip)
                row.chip->hide();
            if (row.field)
                row.field->hide();
            row.link->hide();
            continue;
        }
        rowY += small();
        row.y = rowY;
        row.label->setGeometry(0, rowY, labelWidth, control());
        row.label->show();
        const bool linked = !row.link->text().isEmpty();
        row.link->setGeometry(left, rowY, controlWidth, control());
        row.link->setVisible(linked);
        if (row.chip) {
            row.chip->setGeometry(left, rowY + (control() - chipHeight) / 2, controlWidth,
                                  chipHeight);
            row.chip->setVisible(!linked);
        }
        if (row.field) {
            row.field->setGeometry(left, rowY, fieldWidth, control());
            row.field->setVisible(!linked);
        }
        rowY += control();
    }
}

bool GradientParameterRow::reset() {
    const auto neutral = defaultValue();
    if (!neutral)
        return false;
    const auto current = value();
    if (current && *current == *neutral && parameter())
        return true;
    // A link on a stop the reset gradient no longer has goes with the stop.
    std::vector<std::optional<std::uint32_t>> kept(current ? current->stops.size() : 0);
    for (std::uint32_t index = 0; index < kept.size() && index < neutral->stops.size(); ++index)
        kept[index] = index;
    if (const auto id = ensureParameter())
        return session_.setGradientValue(*id, *neutral, node_, spec_.role, std::move(kept),
                                         tr("Reset Gradient"));
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
    // #543: the stops' links, by stored index; the editor draws what renders (the driven values).
    const auto linked = [this](const std::uint32_t stop, const GradientStopComponent component) {
        return stopLink(stop, component).has_value();
    };
    // #546: the blend pickers and the bar follow the gradient's modes.
    const auto modes = gradient ? gradient->modes : document::GradientModes{};
    {
        refreshing_ = true;
        const bool rgb = modes.colorMode == document::GradientColorMode::Rgb;
        colorMode_->setCurrentIndex(colorMode_->findData(static_cast<int>(modes.colorMode)));
        interpolation_->setCurrentIndex(
            interpolation_->findData(static_cast<int>(modes.interpolation)));
        hue_->setCurrentIndex(hue_->findData(static_cast<int>(modes.hueDirection)));
        interpolationLabel_->setText(rgb ? tr("Interpolation") : tr("Hue"));
        interpolationLabel_->setToolTip(rgb ? interpolation_->toolTip() : hue_->toolTip());
        interpolation_->setVisible(rgb);
        hue_->setVisible(!rgb);
        for (auto* box : {colorMode_, interpolation_, hue_})
            box->setEnabled(enabled);
        refreshing_ = false;
    }
    editor_->setColorSampler(
        modes.isLinearRgb()
            ? kit::KGradientEditor::ColorSampler{}
            : kit::KGradientEditor::ColorSampler(
                  [modes](const std::vector<kit::KGradientEditor::Stop>& stops,
                          const double position) { return sampleStops(stops, modes, position); }));
    if (gradient && !editor_->hasLiveEdit()) {
        projected_ = gradient;
        const auto shown = shownValue().value_or(*gradient);
        std::vector<kit::KGradientEditor::Stop> stops;
        for (std::uint32_t index = 0; index < shown.stops.size(); ++index) {
            kit::KGradientEditor::Stop stop{shown.stops[index].position,
                                            referenceColor(shown.stops[index].color)};
            stop.colorLinked = linked(index, GradientStopComponent::Color);
            stop.positionLinked = linked(index, GradientStopComponent::Position);
            stops.push_back(stop);
        }
        const QSignalBlocker blocker(editor_);
        editor_->setStops(std::move(stops));
    }
    if (!editor_->selectedStop() && !editor_->stops().empty()) {
        const QSignalBlocker blocker(editor_);
        editor_->setSelectedStop(0);
    }
    const auto selected = editor_->selectedStop();
    const bool stopShown = selected && *selected < editor_->stops().size();
    const auto* selectedStop = stopShown ? &editor_->stops()[*selected] : nullptr;
    chip_->setEnabled(enabled && stopShown && !selectedStop->colorLinked);
    position_->setEnabled(enabled && stopShown && !selectedStop->positionLinked);
    const auto linkText = [this](const std::optional<document::ParameterId> link) {
        return link ? tr("Linked to %1").arg(session_.driverDisplayName(*link)) : QString{};
    };
    if (stopShown) {
        const auto& stop = *selectedStop;
        const auto origin = stop.origin ? static_cast<std::uint32_t>(*stop.origin) : 0U;
        refreshing_ = true;
        if (!chip_->isPickerOpen() || !session_.valueEditActive())
            properties::refreshColor(session_, spec_.colorSchemaKey, documentColor(stop.color),
                                     chip_);
        chip_->setToolTip(stop.colorLinked && stop.origin
                              ? linkText(stopLink(origin, GradientStopComponent::Color))
                              : tr("The selected stop's colour"));
        if (!position_->hasLiveEdit()) {
            const QSignalBlocker blocker(position_);
            // A linked position is the link's to set: the field says so.
            position_->setSpecialValueText(stop.positionLinked ? tr("Linked") : QString{});
            position_->setValue(stop.positionLinked ? position_->minimum() : stop.position * 100.0);
        }
        position_->setToolTip(stop.positionLinked && stop.origin
                                  ? linkText(stopLink(origin, GradientStopComponent::Position))
                                  : tr("The selected stop's position along the gradient"));
        refreshing_ = false;
    }
    // #543: a row per stop component, shown with the Values rows or while linked.
    std::size_t count = projected_ ? projected_->stops.size() : 0;
    bool anyLink = false;
    if (const auto* composition = session_.composition())
        if (const auto* record = composition->graph().findNode(node_))
            for (const auto& binding : record->parameters)
                if (const auto stop = document::parseGradientStopPort(binding.role);
                    stop && stop->role == spec_.role) {
                    count = std::max<std::size_t>(count, stop->stop + 1U);
                    anyLink = true;
                }
    rebuildStopRows(count);
    for (auto& row : stopRows_) {
        const auto link = stopLink(row.stop, row.component);
        row.shown = expanded_ || link.has_value();
        row.link->setText(linkText(link));
        const auto driven = link ? session_.drivenValueText(*link) : QString{};
        row.link->setToolTip(driven.isEmpty() ? row.link->text()
                                              : row.link->text() + QStringLiteral(": ") + driven);
        const bool exists = projected_ && row.stop < projected_->stops.size();
        refreshing_ = true;
        if (row.chip) {
            row.chip->setEnabled(enabled && exists);
            if (exists && (!row.chip->isPickerOpen() || !session_.valueEditActive()))
                properties::refreshColor(session_, spec_.colorSchemaKey,
                                         projected_->stops[row.stop].color, row.chip);
        }
        if (row.field) {
            row.field->setEnabled(enabled && exists);
            if (exists && !row.field->hasLiveEdit()) {
                const QSignalBlocker blocker(row.field);
                row.field->setValue(projected_->stops[row.stop].position * 100.0);
            }
        }
        refreshing_ = false;
    }
    if (anyLink)
        session_.refreshDrivenValues();
    diamond_->setParameterId(id.value_or(document::ParameterId{}));
    diamond_->refresh();
    const auto state =
        id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    keyAction_->setText(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                       : tr("Add Key"));
    keyAction_->setEnabled(!locked);
    place();
    if (const int height = heightHint(); height != lastHeight_) {
        lastHeight_ = height;
        updateGeometry();
        Q_EMIT layoutChanged();
    }
}

} // namespace bloom::ui

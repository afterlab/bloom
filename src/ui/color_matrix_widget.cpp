#include <bloom/ui/color_matrix_widget.hpp>

#include "properties_registry_row.hpp"
#include "properties_value_edits.hpp"
#include "value_presentation.hpp"

#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QFontMetrics>
#include <QSignalBlocker>

#include <algorithm>
#include <variant>

namespace bloom::ui {
namespace {

constexpr std::string_view kChannels = "rgba";

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QString channelLetter(const int channel) {
    return QStringLiteral("RGBA").mid(channel, 1);
}

[[nodiscard]] std::string schemaKey(const std::string_view role) {
    return document::colorCleanupSchemaKey(document::kColorMatrixNodeType, role);
}

// A constant choice of the node (its size, its preset), or `fallback`.
[[nodiscard]] std::int64_t choiceOf(const CompositionSession& session, const document::NodeId node,
                                    const std::string_view role, const std::int64_t fallback) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::int64_t>(&*value) : nullptr;
    return stored != nullptr ? *stored : fallback;
}

} // namespace

ColorMatrixCardWidget::ColorMatrixCardWidget(CompositionSession& session,
                                             const document::NodeId node, const bool card,
                                             QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeColorMatrix") : QStringLiteral("propertiesColorMatrix"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    for (int channel = 0; channel < 4; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        columnLabels_[index] = new kit::KLabel(channelLetter(channel), this, kit::TypeRole::UiSmall);
        columnLabels_[index]->setObjectName(QStringLiteral("colorMatrixColumn.%1").arg(channel));
        columnLabels_[index]->setTone(kit::Color::Muted);
        columnLabels_[index]->setAlignment(Qt::AlignCenter);
        columnLabels_[index]->setToolTip(
            tr("From %1: how much of the input's %1 goes into each result").arg(channelLetter(channel)));
        rowLabels_[index] = new kit::KLabel(channelLetter(channel), this, kit::TypeRole::UiSmall);
        rowLabels_[index]->setObjectName(QStringLiteral("colorMatrixRow.%1").arg(channel));
        rowLabels_[index]->setTone(kit::Color::Muted);
        rowLabels_[index]->setAlignment(Qt::AlignCenter);
        rowLabels_[index]->setToolTip(
            tr("Result %1: what the %1 of the result is made from").arg(channelLetter(channel)));
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            auto& cell = cells_[static_cast<std::size_t>(row * 4 + column)];
            cell.role = {kChannels[static_cast<std::size_t>(row)],
                         kChannels[static_cast<std::size_t>(column)]};
            const auto key = schemaKey(cell.role);
            const auto name = parameterRowLabel(key, cell.role);
            cell.field = new kit::KValueField(this);
            cell.field->setObjectName(
                QStringLiteral("colorMatrixCell.%1").arg(QString::fromStdString(cell.role)));
            cell.field->setCompact(true);
            cell.field->setMaximumWidth(QWIDGETSIZE_MAX);
            cell.field->setOutlinedAtRest(true);
            configureValueField(*cell.field, key, document::ParameterValueKind::Float64, cell.role);
            cell.field->setAccessibleName(name);
            cell.field->setToolTip(name);
            cell.field->setDefaultValue(row == column ? 1.0 : 0.0);
            cell.field->setResetText(tr("Reset %1").arg(name));
            cell.key = new QAction(tr("Add Key"), cell.field);
            cell.key->setObjectName(QStringLiteral("colorMatrixKey"));
            const auto role = cell.role;
            connect(cell.key, &QAction::triggered, this, [this, role] {
                if (const auto id = nodeRoleParameter(session_, node_, role))
                    static_cast<void>(session_.toggleKeyframeForParameter(*id));
            });
            cell.field->addAction(cell.key);
            properties::bindValueEdit(session_, *cell.field, [this, role] {
                return nodeRoleParameter(session_, node_, role).value_or(document::ParameterId{});
            });
            connect(cell.field, &kit::KValueField::valueChanged, this,
                    [this, role, name](const double value) {
                        if (const auto id = nodeRoleParameter(session_, node_, role))
                            static_cast<void>(
                                session_.setParameterValue(*id, value, tr("Set %1").arg(name)));
                    });
        }
    }
    note_ = new kit::KLabel(tr("A preset is chosen, so these elements are not read. Choose Custom "
                               "matrix to use them."),
                            this, kit::TypeRole::UiSmall);
    note_->setObjectName(QStringLiteral("colorMatrixPresetNote"));
    note_->setTone(kit::Color::Muted);
    note_->setWordWrap(true);
    expandValues_ = new kit::KButton(kit::IconId::CaretRight, tr("Values"), this);
    expandValues_->setVariant(kit::KButton::Variant::Ghost);
    expandValues_->setControlSize(kit::KButton::ControlSize::Compact);
    expandValues_->setCheckable(true);
    expandValues_->setObjectName(QStringLiteral("colorMatrixValues"));
    expandValues_->setAccessibleName(tr("Matrix element values"));
    expandValues_->setToolTip(tr("Show each element as a row, with its key and its socket, to link it"));
    connect(expandValues_, &QAbstractButton::clicked, this, [this] { setValuesExpanded(!expanded_); });
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &ColorMatrixCardWidget::refresh);
    refresh();
}

kit::KValueField* ColorMatrixCardWidget::cell(const int row, const int column) const {
    if (row < 0 || row > 3 || column < 0 || column > 3)
        return nullptr;
    return cells_[static_cast<std::size_t>(row * 4 + column)].field;
}

bool ColorMatrixCardWidget::elementRole(const std::string_view role) const {
    return role.size() == 2 && kChannels.find(role[0]) != std::string_view::npos &&
           kChannels.find(role[1]) != std::string_view::npos;
}

bool ColorMatrixCardWidget::showsRole(const std::string_view role) const {
    if (!elementRole(role))
        return true;
    // The elements' rows wait behind the grid; open, only those the size reads are shown.
    const bool alpha = role[0] == 'a' || role[1] == 'a';
    return expanded_ && (side_ == 4 || !alpha);
}

void ColorMatrixCardWidget::setValuesExpanded(const bool expanded) {
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;
    expandValues_->setChecked(expanded_);
    expandValues_->setIconId(expanded_ ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    emit rowsChanged();
}

int ColorMatrixCardWidget::heightAt(const int width) const {
    const int header = columnLabels_[0]->sizeHint().height();
    int height = header + small() + side_ * control() + (side_ - 1) * small();
    if (preset_)
        height += small() + note_->heightForWidth(std::max(1, width));
    height += small() + expandValues_->sizeHint().height();
    return height;
}

int ColorMatrixCardWidget::minimumWidthHint() const {
    const int label = QFontMetrics(rowLabels_[0]->font()).horizontalAdvance(QStringLiteral("M")) +
                      small();
    // A cell is never narrower than a value field can be drawn, so the grid of the chosen size
    // sets the card's width.
    const auto* field = cells_[0].field;
    const int cellWidth = std::max({field->minimumWidth(), field->minimumSizeHint().width(),
                                    kit::px(kit::Size::Control) * 2});
    return std::max(label + side_ * cellWidth + side_ * small(),
                    kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control));
}

QSize ColorMatrixCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void ColorMatrixCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void ColorMatrixCardWidget::place() {
    const int label = QFontMetrics(rowLabels_[0]->font()).horizontalAdvance(QStringLiteral("M")) +
                      small();
    const int header = columnLabels_[0]->sizeHint().height();
    const int available = std::max(0, width() - label - side_ * small());
    const int cellWidth = side_ > 0 ? available / side_ : 0;
    const auto x = [&](const int column) { return label + small() + column * (cellWidth + small()); };
    for (int channel = 0; channel < 4; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        const bool shown = channel < side_;
        columnLabels_[index]->setVisible(shown);
        rowLabels_[index]->setVisible(shown);
        if (!shown)
            continue;
        columnLabels_[index]->setGeometry(x(channel), 0, cellWidth, header);
        rowLabels_[index]->setGeometry(0, header + small() + channel * (control() + small()), label,
                                       control());
    }
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            auto* field = cells_[static_cast<std::size_t>(row * 4 + column)].field;
            const bool shown = row < side_ && column < side_;
            field->setVisible(shown);
            if (shown)
                field->setGeometry(x(column), header + small() + row * (control() + small()),
                                   cellWidth, control());
        }
    int y = header + small() + side_ * control() + (side_ - 1) * small();
    note_->setVisible(preset_);
    if (preset_) {
        y += small();
        const int noteHeight = note_->heightForWidth(std::max(1, width()));
        note_->setGeometry(0, y, width(), noteHeight);
        y += noteHeight;
    }
    y += small();
    const auto button = expandValues_->sizeHint();
    expandValues_->setGeometry(0, y, std::min(width(), button.width()), button.height());
}

void ColorMatrixCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    const int side = choiceOf(session_, node_, "matrixSize", document::kColorMatrixSize3) ==
                             document::kColorMatrixSize4
                         ? 4
                         : 3;
    const bool preset =
        choiceOf(session_, node_, "preset", document::kColorMatrixCustom) !=
        document::kColorMatrixCustom;
    const bool changed = side != side_ || preset != preset_;
    side_ = side;
    preset_ = preset;
    for (auto& cell : cells_) {
        const auto id = nodeRoleParameter(session_, node_, cell.role);
        const auto* record = composition && id ? composition->parameters().find(*id) : nullptr;
        const bool linked = record != nullptr &&
                            std::holds_alternative<document::DriverBindingSource>(record->source);
        cell.field->setEnabled(id && !locked && !preset_ && scalarEditable(session_, *id));
        if (cell.field->hasLiveEdit())
            continue;
        const QSignalBlocker blocker(cell.field);
        if (linked) {
            // A linked element is the link's to set.
            cell.field->setSpecialValueText(tr("Linked"));
            cell.field->setValue(cell.field->minimum());
        } else {
            cell.field->setSpecialValueText({});
            if (const auto value = id ? liveScalar(session_, *id) : std::nullopt)
                cell.field->setValue(*value);
        }
        const auto state = id ? session_.keyframeDiamondStateForParameter(*id)
                              : KeyframeDiamondState::Unsupported;
        cell.key->setVisible(state != KeyframeDiamondState::Unsupported);
        cell.key->setEnabled(!locked && !preset_);
        cell.key->setText(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                         : tr("Add Key"));
    }
    if (changed) {
        place();
        updateGeometry();
        emit rowsChanged();
    } else if (!note_->isVisibleTo(this) && preset_) {
        place();
    }
}

} // namespace bloom::ui

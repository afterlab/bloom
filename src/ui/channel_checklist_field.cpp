#include "channel_checklist_field.hpp"

#include "properties_text_edit.hpp"

#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/ui/channel_set_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/popup_window.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

[[nodiscard]] QString everyToken(const ChannelChecklistKind kind) {
    return kind == ChannelChecklistKind::OutputLayers ? QString{} : QStringLiteral("*");
}

} // namespace

std::optional<ChannelChecklistKind> channelChecklistKind(const std::string_view schemaKey) {
    if (schemaKey == "bloom.channel-copy.select")
        return ChannelChecklistKind::Copy;
    if (schemaKey == "bloom.channel-remove.select")
        return ChannelChecklistKind::Remove;
    if (schemaKey == document::kCompositionOutputLayersParameterSchemaKey)
        return ChannelChecklistKind::OutputLayers;
    return std::nullopt;
}

std::vector<ChannelChecklistEntry>
channelChecklistEntries(const runtime::ChannelInspectionListing& listing,
                        const ChannelChecklistKind kind) {
    std::vector<ChannelChecklistEntry> entries;
    for (const auto& layer : listing.layers) {
        if (layer.empty()) {
            if (kind == ChannelChecklistKind::OutputLayers)
                continue;
            // The root layer has no name a pattern can use: its channels are listed one by one.
            for (const auto& channel : listing.channels)
                if (channel.find('.') == std::string::npos)
                    entries.push_back(
                        {QString::fromStdString(channel), QString::fromStdString(channel)});
            continue;
        }
        entries.push_back({QString::fromStdString(layer),
                           QObject::tr("%1 (layer)").arg(QString::fromStdString(layer))});
    }
    return entries;
}

std::optional<std::vector<bool>>
channelChecklistSelection(const QString& pattern, const std::vector<ChannelChecklistEntry>& entries,
                          const ChannelChecklistKind kind) {
    const auto trimmed = pattern.trimmed();
    if (trimmed == everyToken(kind))
        return std::vector<bool>(entries.size(), true);
    std::vector<bool> checked(entries.size(), false);
    for (const auto& part : trimmed.split(QLatin1Char(','))) {
        const auto token = part.trimmed();
        if (token.isEmpty())
            continue;
        const auto found = std::ranges::find(entries, token, &ChannelChecklistEntry::token);
        if (found == entries.end())
            return std::nullopt;
        checked[static_cast<std::size_t>(found - entries.begin())] = true;
    }
    return checked;
}

QString channelChecklistPattern(const std::vector<bool>& checked,
                                const std::vector<ChannelChecklistEntry>& entries,
                                const ChannelChecklistKind kind) {
    if (!entries.empty() && checked.size() == entries.size() &&
        std::ranges::all_of(checked, [](const bool on) { return on; }))
        return everyToken(kind);
    QStringList tokens;
    for (std::size_t index = 0; index < entries.size() && index < checked.size(); ++index)
        if (checked[index])
            tokens.push_back(entries[index].token);
    return tokens.join(QLatin1Char(','));
}

ChannelChecklistField::ChannelChecklistField(CompositionSession& session,
                                             const document::NodeId node, std::string role,
                                             const ChannelChecklistKind kind, QWidget* parent)
    : QWidget(parent), session_(session), node_(node), role_(std::move(role)), kind_(kind) {
    setObjectName(QStringLiteral("channelChecklistField"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    summary_ = new kit::KButton(this);
    summary_->setObjectName(QStringLiteral("channelChecklistSummary"));
    summary_->setVariant(kit::KButton::Variant::Secondary);
    summary_->setControlSize(kit::KButton::ControlSize::Compact);
    summary_->setIconId(kit::IconId::CaretDown);
    layout->addWidget(summary_, 1);
    pattern_ = new kit::KLineEdit(this);
    pattern_->setObjectName(QStringLiteral("channelChecklistPattern"));
    pattern_->setTypeRole(kit::TypeRole::Value);
    pattern_->setPlaceholderText(kind == ChannelChecklistKind::OutputLayers ? tr("Every layer")
                                                                            : tr("No channels"));
    pattern_->setToolTip(tr("Comma-separated channel names, layer names, globs such as "
                            "diffuse.* or roles such as @depth"));
    pattern_->hide();
    layout->addWidget(pattern_, 1);
    properties::bindTextEdit(session_, *pattern_, [this] { return parameter(); });
    connect(pattern_, &QLineEdit::editingFinished, this, [this] {
        // An unbound layer list (an older Composition Output) has no parameter to edit live.
        if (!parameter().isValid())
            write(pattern_->text().trimmed());
    });
    toggle_ = new kit::KIconToggle(kit::IconId::Text, this);
    toggle_->setObjectName(QStringLiteral("channelChecklistPatternToggle"));
    toggle_->setCheckable(true);
    toggle_->setAccessibleName(tr("Advanced pattern"));
    toggle_->setToolTip(tr("Advanced pattern: type channel patterns instead of choosing"));
    layout->addWidget(toggle_);
    connect(summary_, &QAbstractButton::clicked, this, &ChannelChecklistField::openChecklist);
    connect(toggle_, &QAbstractButton::toggled, this, [this](const bool on) {
        if (refreshing_)
            return;
        userAdvanced_ = on;
        refresh();
    });
    if (auto* controller = session_.channelSetController())
        connect(controller, &ChannelSetController::changed, this, &ChannelChecklistField::refresh);
    refresh();
}

ChannelChecklistField::~ChannelChecklistField() {
    if (popup_ != nullptr)
        popup_->deleteLater();
}

document::ParameterId ChannelChecklistField::parameter() const {
    const auto* composition = session_.composition();
    const auto* node = composition != nullptr ? composition->graph().findNode(node_) : nullptr;
    if (node != nullptr)
        for (const auto& binding : node->parameters)
            if (binding.role == role_)
                return binding.parameterId;
    return {};
}

void ChannelChecklistField::write(const QString& pattern) {
    (void)session_.setNodeParameterValue(
        node_, role_, document::ParameterValue{pattern.toStdString()}, tr("Set Channels"));
}

void ChannelChecklistField::refresh() {
    refreshing_ = true;
    const auto id = parameter();
    const auto stored =
        id.isValid() ? session_.constantStringValue(id).value_or(QString{}) : QString{};
    auto* controller = session_.channelSetController();
    if (controller != nullptr && !controller->listingCurrent(node_))
        controller->request(node_);
    const auto* listing = controller != nullptr ? controller->listing(node_) : nullptr;
    entries_ = listing != nullptr ? channelChecklistEntries(*listing, kind_)
                                  : std::vector<ChannelChecklistEntry>{};
    selection_ = channelChecklistSelection(stored, entries_, kind_);
    // A pattern the checklist cannot show (a glob, a role, a channel the set no longer carries)
    // keeps the pattern field open: the checklist must never silently rewrite it.
    const bool forced = listing != nullptr && !entries_.empty() && !selection_.has_value() &&
                        !stored.trimmed().isEmpty();
    const bool advanced = userAdvanced_ || forced;
    {
        const QSignalBlocker blocker(toggle_);
        toggle_->setChecked(advanced);
    }
    toggle_->setEnabled(!forced);
    toggle_->setToolTip(forced ? tr("This pattern has tokens a checklist cannot show; edit it "
                                    "here, or clear it to choose from the list")
                               : tr("Advanced pattern: type channel patterns instead of choosing"));
    applyAdvanced(advanced);
    if (!pattern_->hasFocus())
        pattern_->setText(stored);
    QString text;
    if (listing == nullptr)
        text = tr("Reading channels…");
    else if (entries_.empty())
        text = listing->diagnostic.empty() ? tr("No channels")
                                           : QString::fromStdString(listing->diagnostic);
    else if (!selection_)
        text = stored;
    else {
        const auto count = static_cast<int>(std::ranges::count(*selection_, true));
        if (count == static_cast<int>(entries_.size()))
            text = kind_ == ChannelChecklistKind::OutputLayers ? tr("Every layer") : tr("All");
        else if (count == 0)
            text = tr("None");
        else if (count == 1)
            text = entries_[static_cast<std::size_t>(std::ranges::find(*selection_, true) -
                                                     selection_->begin())]
                       .label;
        else
            text = tr("%1 of %2").arg(count).arg(entries_.size());
    }
    summary_->setText(text);
    summary_->setToolTip(selection_ && !entries_.empty() ? stored : text);
    const auto* composition = session_.composition();
    const bool editable = composition != nullptr && !composition->nodeLocked(node_);
    summary_->setEnabled(editable && !entries_.empty() && selection_.has_value());
    pattern_->setEnabled(editable);
    if (popup_ != nullptr && popup_->isVisible())
        for (auto* check :
             popup_->findChildren<kit::KCheckBox*>(QStringLiteral("channelChecklistEntry"))) {
            const auto index = check->property("entryIndex").toULongLong();
            const QSignalBlocker blocker(check);
            check->setChecked(selection_ && index < selection_->size() && (*selection_)[index]);
        }
    refreshing_ = false;
}

void ChannelChecklistField::applyAdvanced(const bool on) {
    summary_->setVisible(!on);
    pattern_->setVisible(on);
    if (on && popup_ != nullptr)
        popup_->close();
}

QWidget* ChannelChecklistField::checklist() const {
    return popup_ != nullptr ? popup_->content() : nullptr;
}

void ChannelChecklistField::openChecklist() {
    if (entries_.empty() || !selection_)
        return;
    if (popup_ != nullptr)
        popup_->close();
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("channelChecklist"));
    auto* layout = new QVBoxLayout(content);
    const auto inset = kit::px(kit::Spacing::S);
    layout->setContentsMargins(inset, inset, inset, inset);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto* check = new kit::KCheckBox(entries_[index].label, content);
        check->setObjectName(QStringLiteral("channelChecklistEntry"));
        check->setCheckable(true);
        check->setProperty("entryIndex", QVariant::fromValue<qulonglong>(index));
        check->setProperty("token", entries_[index].token);
        check->setChecked((*selection_)[index]);
        layout->addWidget(check);
        connect(check, &kit::KSwitch::toggled, this,
                [this, index](const bool on) { setEntryChecked(index, on); });
    }
    auto* popup = new kit::KPopupWindow(nullptr, kit::Elevation::Popup);
    popup->setObjectName(QStringLiteral("channelChecklistPopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setContent(content);
    popup_ = popup;
    const int border = std::max(1, static_cast<int>(std::lround(kit::kHairlineWidth)));
    const auto size = content->sizeHint().expandedTo(QSize(summary_->width(), 0));
    popup->openFor(*summary_, size.grownBy(QMargins(border, border, border, border)));
}

void ChannelChecklistField::setEntryChecked(const std::size_t index, const bool checked) {
    if (refreshing_ || !selection_ || index >= selection_->size())
        return;
    auto next = *selection_;
    next[index] = checked;
    // The Composition Output's empty pattern means every layer, so it cannot say "none": the last
    // checked layer stays checked.
    if (kind_ == ChannelChecklistKind::OutputLayers &&
        std::ranges::none_of(next, std::identity{})) {
        refresh();
        return;
    }
    write(channelChecklistPattern(next, entries_, kind_));
    refresh();
}

} // namespace bloom::ui

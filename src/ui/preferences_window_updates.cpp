#include "preferences_window.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

namespace bloom::ui {

// The Updates section of the General page (#734) lives in its own translation unit so the dialog
// construction file stays under the size budget. One switch edits the draft; Apply stores it.
void PreferencesWindow::buildUpdatesSection(QVBoxLayout* layout, QWidget* content) {
    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesUpdatesSection"), tr("Updates"),
        QStringLiteral("preferences/sections/updates/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*section, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.automaticUpdateCheck = base.automaticUpdateCheck;
    });
    auto* rows = section->bodyLayout();

    auto* automatic = addSwitchRow(rows, content, QStringLiteral("preferencesAutomaticUpdateSwitch"),
                                   tr("Check for updates automatically"), false);
    automatic->setChecked(draft_.automaticUpdateCheck);
    connect(automatic, &QAbstractButton::toggled, this, [this](const bool enabled) {
        draft_.automaticUpdateCheck = enabled;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, automatic] {
        const QSignalBlocker blocker(automatic);
        automatic->setChecked(draft_.automaticUpdateCheck);
    });

    // Muted helper text under the switch, in the value column.
    auto* holder = new QWidget(content);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(
        kit::px(kit::Spacing::RowPadding) + kit::px(kit::Size::PreferencesLabelWidth) +
            kit::px(kit::Spacing::PropertyGutter),
        0, kit::px(kit::Spacing::RowPadding) + kit::px(kit::Size::PropertiesDiamondColumn),
        kit::px(kit::Spacing::XXS));
    auto* helper = new kit::KLabel(
        tr("Once a day, Bloom asks whether a newer version exists. It sends nothing but that "
           "request."),
        holder, kit::TypeRole::UiSmall);
    helper->setObjectName(QStringLiteral("preferencesAutomaticUpdateHint"));
    helper->setTone(kit::Color::Muted);
    helper->setWordWrap(true);
    row->addWidget(helper, 1);
    rows->addWidget(holder);
}

} // namespace bloom::ui

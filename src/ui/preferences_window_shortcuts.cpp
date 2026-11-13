#include "preferences_window.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

namespace bloom::ui {

namespace {

// The window command that already answers to `keys`, by name, or empty when none does. The two
// actions this page edits are skipped: their keys are the draft's.
QString commandUsing(const QWidget* window, const QKeySequence& keys) {
    if (window == nullptr || keys.isEmpty())
        return {};
    for (const auto* action : window->findChildren<QAction*>()) {
        if (action->objectName() == QStringLiteral("purgePreviewCacheAction") ||
            action->objectName() == QStringLiteral("purgeMediaCacheAction"))
            continue;
        for (const auto& bound : action->shortcuts())
            if (bound.matches(keys) == QKeySequence::ExactMatch) {
                auto name = action->text();
                name.remove(QLatin1Char('&'));
                return name.isEmpty() ? action->objectName() : name;
            }
    }
    return {};
}

} // namespace

// #688: the Shortcuts page. It lists the commands whose keys are the artist's to choose; a key
// another command of the window already answers to is refused under the field, and an emptied
// field leaves the command without a key. Like every page it edits the draft; the menus show the
// new keys after Apply.
QWidget* PreferencesWindow::buildShortcutsPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesShortcutsSection"), tr("Shortcuts"),
        QStringLiteral("preferences/sections/shortcuts/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*section, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.purgePreviewCacheShortcut = base.purgePreviewCacheShortcut;
        draft.purgeMediaCacheShortcut = base.purgeMediaCacheShortcut;
    });
    auto* rows = section->bodyLayout();

    auto* refusal = new kit::KLabel(QString{}, content, kit::TypeRole::UiSmall);
    refusal->setObjectName(QStringLiteral("preferencesShortcutRefusal"));
    refusal->setTone(kit::Color::Error);
    refusal->setWordWrap(true);
    refusal->hide();

    const auto addShortcut = [&](const QString& objectName, const QString& label,
                                 QString ApplicationPreferences::*member,
                                 QString ApplicationPreferences::*other) {
        auto* field = new QKeySequenceEdit(content);
        field->setObjectName(objectName);
        field->setAccessibleName(label);
        field->setMaximumSequenceLength(1);
        field->setClearButtonEnabled(true);
        field->setToolTip(tr("Click, then press the keys for %1. Clear the field for no shortcut.")
                              .arg(label));
        field->setMinimumWidth(kit::px(kit::Size::PreferencesLabelWidth));
        field->setKeySequence(QKeySequence::fromString(draft_.*member, QKeySequence::PortableText));
        (void)addPreferencesRow(rows, content, label, {field});
        connect(field, &QKeySequenceEdit::keySequenceChanged, this,
                [this, field, refusal, label, member, other](const QKeySequence& keys) {
                    // Refused: a key another command of the window has, or the other row's.
                    QString taken = commandUsing(parentWidget(), keys);
                    if (taken.isEmpty() && !keys.isEmpty() &&
                        keys.matches(QKeySequence::fromString(draft_.*other,
                                                              QKeySequence::PortableText)) ==
                            QKeySequence::ExactMatch)
                        taken = tr("the other Purge command");
                    if (!taken.isEmpty()) {
                        refusal->setText(
                            tr("%1 is already the shortcut of %2. %3 keeps its shortcut.")
                                .arg(keys.toString(QKeySequence::NativeText), taken, label));
                        refusal->show();
                        const QSignalBlocker blocker(field);
                        field->setKeySequence(
                            QKeySequence::fromString(draft_.*member, QKeySequence::PortableText));
                        return;
                    }
                    refusal->hide();
                    draft_.*member = keys.toString(QKeySequence::PortableText);
                    updateApplyEnabled();
                });
        refreshCallbacks_.push_back([this, field, refusal, member] {
            const QSignalBlocker blocker(field);
            field->setKeySequence(
                QKeySequence::fromString(draft_.*member, QKeySequence::PortableText));
            refusal->hide();
        });
    };
    addShortcut(QStringLiteral("preferencesPurgePreviewCacheShortcut"), tr("Purge preview cache"),
                &ApplicationPreferences::purgePreviewCacheShortcut,
                &ApplicationPreferences::purgeMediaCacheShortcut);
    addShortcut(QStringLiteral("preferencesPurgeMediaCacheShortcut"), tr("Purge media cache"),
                &ApplicationPreferences::purgeMediaCacheShortcut,
                &ApplicationPreferences::purgePreviewCacheShortcut);
    rows->addWidget(refusal);

    layout->addStretch(1);
    return wrapPage(content, QStringLiteral("preferencesShortcutsPage"));
}

} // namespace bloom::ui

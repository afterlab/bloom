#include "svg_import_mode_dialog.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>

#include <QVBoxLayout>

namespace bloom::ui {

SvgImportModeDialog::SvgImportModeDialog(const std::size_t count, const commands::SvgImportMode initial,
                                         QWidget& owner)
    : kit::KDialog(tr("Import SVG"), QStringLiteral("svgImportModeDialog"), owner) {
    auto* heading = new kit::KLabel(count > 1 ? tr("How should these %1 SVG files import?").arg(count)
                                              : tr("How should this SVG import?"),
                                    this, kit::TypeRole::Title);
    heading->setObjectName(QStringLiteral("svgImportModeHeading"));
    heading->setWordWrap(true);
    contentLayout()->addWidget(heading);

    choices_ = new kit::KRadioGroup(this);
    choices_->setObjectName(QStringLiteral("svgImportModeChoices"));
    choices_->setAccessibleName(tr("SVG import mode"));
    choices_->setPresentation(kit::KRadioGroup::Presentation::Discrete);
    (void)choices_->addOption(tr("As one layer"));
    (void)choices_->addOption(tr("As editable shapes"));
    choices_->setCurrentIndex(initial == commands::SvgImportMode::Shapes ? 1 : 0);
    contentLayout()->addWidget(choices_);

    note_ = new kit::KLabel(QString{}, this);
    note_->setObjectName(QStringLiteral("svgImportModeNote"));
    note_->setTone(kit::Color::Muted);
    note_->setWordWrap(true);
    contentLayout()->addWidget(note_);
    refreshNote();
    connect(choices_, &kit::KRadioGroup::currentIndexChanged, this, [this](int) { refreshNote(); });

    remember_ = new kit::KCheckBox(this);
    remember_->setObjectName(QStringLiteral("svgImportModeRemember"));
    auto* form = addForm();
    (void)form->addCheckRow(QString{}, *remember_, tr("Remember my choice"));
    remember_->setToolTip(tr("Use this choice for every SVG without asking. Preferences > Import "
                             "changes it."));

    contentLayout()->addStretch(1);
    (void)addButton(Role::Accept, tr("Import"));
    (void)addButton(Role::Reject, tr("Cancel"));
    setInitialFocus(choices_);
}

commands::SvgImportMode SvgImportModeDialog::mode() const {
    return choices_->currentIndex() == 1 ? commands::SvgImportMode::Shapes : commands::SvgImportMode::Layer;
}

bool SvgImportModeDialog::remember() const { return remember_->isChecked(); }

void SvgImportModeDialog::refreshNote() {
    note_->setText(choices_->currentIndex() == 1
                       ? tr("A shape layer and a path node for every element, so points can be keyed "
                            "and path nodes wired in. A complex file becomes many nodes.")
                       : tr("A single vector layer that stays crisp at any scale, rotation and render "
                            "size. Break Apart turns it into editable shapes later."));
}

} // namespace bloom::ui

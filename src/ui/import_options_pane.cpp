#include "import_options_pane.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QSignalBlocker>

#include <algorithm>

namespace bloom::ui {
namespace {
constexpr auto kSequenceKey = "dialogs/import/sequences";
constexpr auto kSkipMissingKey = "dialogs/import/skip-missing-frames";
constexpr auto kCompositionKey = "dialogs/import/create-composition";
// The highest frame number a range names: the sequence scanner's own frame-number ceiling is far
// above any real sequence, so this only keeps the field to a readable width.
constexpr std::int64_t kMaximumFrame = 99999999;

[[nodiscard]] kit::KCheckBox* check(QWidget* parent, const char* name, const QString& label) {
    auto* box = new kit::KCheckBox(label, parent);
    box->setObjectName(QString::fromLatin1(name));
    // Every row a control tall, so the check rows line up with the frame fields and the label.
    box->setMinimumHeight(kit::px(kit::Size::Control));
    return box;
}

[[nodiscard]] kit::KValueField* frameField(QWidget* parent, const char* name, const QString& label,
                                           const std::int64_t value) {
    auto* field = new kit::KValueField(parent);
    field->setObjectName(QString::fromLatin1(name));
    field->setAccessibleName(label);
    field->setIntegerMode(true);
    field->setIntegerRange(0, kMaximumFrame);
    field->setIntegerValue(value);
    field->setDefaultValue(static_cast<double>(value));
    field->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    return field;
}
} // namespace

ImportOptionsPane::ImportOptionsPane(const bool offerComposition, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("importOptionsPane"));
    const QSettings settings;
    sequence_ = check(this, "importOptionsSequence", tr("Import numbered images as a sequence"));
    sequence_->setChecked(settings.value(QLatin1String(kSequenceKey), true).toBool());
    skip_ = check(this, "importOptionsSkipMissing", tr("Skip missing frames"));
    skip_->setToolTip(tr("Play the frames a sequence has back to back"));
    skip_->setChecked(settings.value(QLatin1String(kSkipMissingKey), false).toBool());
    range_ = check(this, "importOptionsRange", tr("Only frames"));
    first_ = frameField(this, "importOptionsFirstFrame", tr("First frame"), 1);
    auto* to = new kit::KLabel(tr("to"), this);
    last_ = frameField(this, "importOptionsLastFrame", tr("Last frame"), 100);
    composition_ = check(this, "importOptionsComposition", tr("Create a composition"));
    composition_->setToolTip(tr("Make a composition from the imported footage"));
    composition_->setChecked(offerComposition &&
                             settings.value(QLatin1String(kCompositionKey), false).toBool());
    composition_->setVisible(offerComposition);

    auto* rangeRow = new QHBoxLayout;
    rangeRow->setContentsMargins(0, 0, 0, 0);
    rangeRow->setSpacing(kit::px(kit::Spacing::S));
    rangeRow->addWidget(range_);
    rangeRow->addWidget(first_);
    rangeRow->addWidget(to);
    rangeRow->addWidget(last_);
    rangeRow->addStretch(1);
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(kit::px(kit::Spacing::L));
    grid->setVerticalSpacing(kit::px(kit::Spacing::S));
    grid->addWidget(sequence_, 0, 0);
    grid->addWidget(skip_, 0, 1);
    grid->addLayout(rangeRow, 1, 0);
    grid->addWidget(composition_, 1, 1);
    // Two left-aligned columns; the room to the right stays empty.
    grid->setColumnStretch(2, 1);

    connect(sequence_, &QAbstractButton::toggled, this, [this](bool) { refresh(); });
    connect(range_, &QAbstractButton::toggled, this, [this](bool) { refresh(); });
    // The range stays ordered: moving one end past the other moves the other with it.
    connect(first_, &kit::KValueField::integerValueChanged, this, [this](const qint64 value) {
        if (last_->integerValue() < value) {
            const QSignalBlocker blocked(last_);
            last_->setIntegerValue(value);
        }
    });
    connect(last_, &kit::KValueField::integerValueChanged, this, [this](const qint64 value) {
        if (first_->integerValue() > value) {
            const QSignalBlocker blocked(first_);
            first_->setIntegerValue(value);
        }
    });
    refresh();
}

commands::ImportOptions ImportOptionsPane::options() const {
    commands::ImportOptions options;
    options.sequences = sequence_->isChecked();
    if (options.sequences) {
        options.skipMissingFrames = skip_->isChecked();
        if (range_->isChecked())
            options.frameRange = document::AssetSequenceRange{
                first_->integerValue(), std::max(first_->integerValue(), last_->integerValue())};
    }
    return options;
}

bool ImportOptionsPane::createComposition() const {
    return !composition_->isHidden() && composition_->isChecked();
}

void ImportOptionsPane::remember() const {
    QSettings settings;
    settings.setValue(QLatin1String(kSequenceKey), sequence_->isChecked());
    settings.setValue(QLatin1String(kSkipMissingKey), skip_->isChecked());
    if (!composition_->isHidden())
        settings.setValue(QLatin1String(kCompositionKey), composition_->isChecked());
}

void ImportOptionsPane::refresh() {
    // Skip and the frame range are about sequences; single images have neither.
    const bool sequences = sequence_->isChecked();
    skip_->setEnabled(sequences);
    range_->setEnabled(sequences);
    first_->setEnabled(sequences && range_->isChecked());
    last_->setEnabled(sequences && range_->isChecked());
}

} // namespace bloom::ui

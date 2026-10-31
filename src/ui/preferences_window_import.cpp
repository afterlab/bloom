#include "interpret_footage_dialog.hpp"
#include "preferences_window.hpp"
#include "properties_sections.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QHBoxLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace bloom::ui {
namespace {
// The rate field's number as Interpret Footage reads it: 23.976 and 29.97 name the exact
// 24000/1001 and 30000/1001, any other value is exact as shown to three decimals. 0 is the
// composition's own rate; a rate outside Interpret Footage's bounds keeps the previous one.
[[nodiscard]] std::optional<std::optional<document::FrameRate>> rateFromField(const double value) {
    if (value <= 0.0)
        return std::optional<document::FrameRate>{};
    auto text = QString::number(value, 'f', 3);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    const auto rate = parseFootageFrameRate(text);
    if (!rate || !document::isValidFootageFrameRate(*rate))
        return std::nullopt;
    return rate;
}
[[nodiscard]] double rateField(const std::optional<document::FrameRate>& rate) {
    return rate ? static_cast<double>(rate->numerator()) / rate->denominator() : 0.0;
}
// Seconds to the hundredth; 0 is the composition's length.
[[nodiscard]] std::optional<core::RationalTime> durationFromField(const double value) {
    if (value <= 0.0)
        return std::nullopt;
    return core::RationalTime::create(std::llround(value * 100.0), 100);
}
[[nodiscard]] double durationField(const std::optional<core::RationalTime>& duration) {
    return duration ? duration->toSeconds() : 0.0;
}
} // namespace

// The Import page (#381): what newly imported footage starts with. Its own translation unit, like
// Sounds, so the dialog construction file stays under the size budget.
QWidget* PreferencesWindow::buildImportPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesImportSection"), tr("Import"),
        QStringLiteral("preferences/sections/import/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*section, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.importSequenceFrameRate = base.importSequenceFrameRate;
        draft.importStillDuration = base.importStillDuration;
    });
    auto* rows = section->bodyLayout();

    auto* rate =
        addAutomaticRow(rows, content, QStringLiteral("preferencesImportSequenceFrameRateField"),
                        tr("Sequence frame rate"), 1000.0, 3, QStringLiteral("fps"), 24.0,
                        tr("Match composition"), tr("Assume a rate"));
    rate->setToolTip(tr("The frame rate a newly imported image sequence plays at. Match "
                        "composition shows one image per composition frame."));
    rate->setValue(rateField(draft_.importSequenceFrameRate));
    connect(rate, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (const auto parsed = rateFromField(value)) {
            draft_.importSequenceFrameRate = *parsed;
            updateApplyEnabled();
        }
    });
    refreshCallbacks_.push_back([this, rate] {
        const QSignalBlocker blocker(rate);
        rate->setValue(rateField(draft_.importSequenceFrameRate));
    });

    auto* still = addAutomaticRow(
        rows, content, QStringLiteral("preferencesImportStillDurationField"), tr("Still duration"),
        86400.0, 2, QStringLiteral("s"), 5.0, tr("Composition length"), tr("Fixed length"));
    still->setToolTip(tr("How long a newly imported still lasts as a layer. Composition length "
                         "spans the whole composition."));
    still->setValue(durationField(draft_.importStillDuration));
    connect(still, &kit::KValueField::valueChanged, this, [this](const double value) {
        draft_.importStillDuration = durationFromField(value);
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, still] {
        const QSignalBlocker blocker(still);
        still->setValue(durationField(draft_.importStillDuration));
    });

    // Muted helper text in the value column, as the other pages' helper rows.
    auto* holder = new QWidget(content);
    auto* helper = new QHBoxLayout(holder);
    helper->setContentsMargins(
        kit::px(kit::Spacing::RowPadding) + kit::px(kit::Size::PreferencesLabelWidth) +
            kit::px(kit::Spacing::PropertyGutter),
        0, kit::px(kit::Spacing::RowPadding) + kit::px(kit::Size::PropertiesDiamondColumn),
        kit::px(kit::Spacing::XXS));
    auto* hint = new kit::KLabel(
        tr("Footage imported from now on starts with these. Interpret Footage changes footage "
           "already in the project."),
        holder);
    hint->setObjectName(QStringLiteral("preferencesImportHint"));
    hint->setTypeRole(kit::TypeRole::UiSmall);
    hint->setTone(kit::Color::Muted);
    hint->setWordWrap(true);
    helper->addWidget(hint, 1);
    rows->addWidget(holder);

    layout->addStretch(1);
    return wrapPage(content, QStringLiteral("preferencesImportPage"));
}

} // namespace bloom::ui

#include "export_output_module_dialog.hpp"

#include "export_detail_support.hpp"
#include "export_panel_text.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace bloom::ui::exportpanel {
namespace {

using detail::choicesOf;
using detail::fill;
using detail::isImage;
using detail::isMovie;
using detail::select;
using document::ExportFormat;

[[nodiscard]] QWidget* fieldRow(QWidget* parent, std::initializer_list<QWidget*> widgets) {
    auto* holder = new QWidget(parent);
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::S));
    for (auto* widget : widgets)
        layout->addWidget(widget);
    layout->addStretch(1);
    return holder;
}

[[nodiscard]] kit::KDropdown* dropdown(QWidget* parent, const QString& name, const QString& accessible) {
    auto* field = new kit::KDropdown(parent);
    field->setObjectName(name);
    field->setAccessibleName(accessible);
    return field;
}

[[nodiscard]] kit::KCheckBox* check(QWidget* parent, const QString& name, const QString& accessible) {
    auto* box = new kit::KCheckBox(parent);
    box->setObjectName(name);
    box->setAccessibleName(accessible);
    return box;
}

// H.264 and HEVC take a target bit rate; the codec's own default is what an output without one uses
// (the same figures the engine encodes with and the queue's size estimate assumes).
[[nodiscard]] bool takesBitrate(const ExportFormat format) {
    return format == ExportFormat::H264Mp4 || format == ExportFormat::H264Mov ||
           format == ExportFormat::HevcMp4 || format == ExportFormat::HevcMov;
}

[[nodiscard]] std::int64_t defaultBitrateKbps(const ExportFormat format) {
    return format == ExportFormat::HevcMp4 || format == ExportFormat::HevcMov ? 20000 : 30000;
}

[[nodiscard]] QString quoted(const QString& name) { return QStringLiteral("“%1”").arg(name); }

} // namespace

OutputModuleDialog::OutputModuleDialog(const host::ExportOutputView& output,
                                       std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue,
                                       ExportColourChoices colours, QWidget& owner,
                                       ExportQueueAdapter* const presets)
    : kit::KDialog(tr("Output Module"), QStringLiteral("exportOutputModuleDialog"), owner,
                   Qt::WindowModal, Width::Wide),
      output_(output), catalogue_(std::move(catalogue)), colours_(std::move(colours)),
      presets_(presets), base_(output.record.settings) {
    // One group, two columns side by side; every form of the dialog shares one label column.
    const auto group = [this](const QString& title, const QString& name) {
        auto* section = new kit::KSection(title, this);
        section->setObjectName(name);
        section->setResetEnabled(false);
        contentLayout()->addWidget(section);
        auto* columns = new QWidget(section->body());
        auto* columnsLayout = new QHBoxLayout(columns);
        columnsLayout->setContentsMargins(0, 0, 0, 0);
        columnsLayout->setSpacing(kit::px(kit::Spacing::XL));
        section->bodyLayout()->addWidget(columns);
        const auto column = [this, columns, columnsLayout] {
            auto* widget = new QWidget(columns);
            auto* layout = new QVBoxLayout(widget);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(kit::px(kit::Spacing::S));
            auto* form = addForm(*layout, *widget);
            layout->addStretch(1);
            columnsLayout->addWidget(widget, 1);
            return form;
        };
        auto* left = column();
        auto* right = column();
        return std::tuple{section, left, right};
    };

    // The format, what happens to an existing file, and what follows the render.
    auto [mainGroup, mainLeft, mainRight] = group(quoted(presetText(output, catalogue_.get())),
                                             QStringLiteral("exportOutputModuleMain"));
    main_ = mainGroup;
    mainLeft_ = mainLeft;
    format_ = dropdown(this, QStringLiteral("exportFormat"), tr("Format"));
    // Save as preset sits beside Format, as Composition Settings offers it beside Preset; a save
    // that could not complete says why under the row.
    savePreset_ = new kit::KIconButton(this);
    savePreset_->setObjectName(QStringLiteral("exportSavePreset"));
    savePreset_->setIcon(kit::icon(kit::IconId::Add, kit::IconRole::Chrome));
    savePreset_->setToolTip(tr("Save as preset…"));
    savePreset_->setAccessibleName(tr("Save as preset…"));
    savePreset_->setVisible(presets_ != nullptr);
    auto* formatHolder = new QWidget(this);
    auto* formatColumn = new QVBoxLayout(formatHolder);
    formatColumn->setContentsMargins(0, 0, 0, 0);
    formatColumn->setSpacing(kit::px(kit::Spacing::XXS));
    formatColumn->addWidget(fieldRow(formatHolder, {format_, savePreset_}));
    presetError_ = new kit::KLabel(QString{}, formatHolder, kit::TypeRole::UiSmall);
    presetError_->setObjectName(QStringLiteral("exportSavePresetError"));
    presetError_->setWordWrap(true);
    presetError_->setTone(kit::Color::Error);
    presetError_->hide();
    formatColumn->addWidget(presetError_);
    (void)mainLeft->addRow(tr("Format"), formatHolder);
    overwrite_ = dropdown(this, QStringLiteral("exportOverwrite"), tr("If a file is there"));
    overwrite_->addItem(tr("Ask first"), static_cast<int>(document::ExportOverwritePolicy::AskBeforeReplacing));
    overwrite_->addItem(tr("Replace"), static_cast<int>(document::ExportOverwritePolicy::Replace));
    overwrite_->addItem(tr("Don't render"), static_cast<int>(document::ExportOverwritePolicy::Refuse));
    (void)mainLeft->addRow(tr("If a file is there"), fieldRow(this, {overwrite_}));
    import_ = check(this, QStringLiteral("exportImportIntoAssets"), tr("Add to Assets when done"));
    (void)mainRight->addCheckRow(tr("Post-render"), *import_, tr("Add to Assets when done"));
    cachedFrames_ = check(this, QStringLiteral("exportUseCachedFrames"), tr("Use cached frames"));
    (void)mainRight->addCheckRow(tr("Cached frames"), *cachedFrames_, tr("Use cached frames"));

    // How the frames are encoded: only what this format takes.
    auto [quality, qualityLeft, qualityRight] = group(tr("Quality"), QStringLiteral("exportOutputModuleQuality"));
    quality_ = quality;
    qualityLeft_ = qualityLeft;
    qualityRight_ = qualityRight;
    profile_ = dropdown(this, QStringLiteral("exportProfile"), tr("Profile"));
    profileRow_ = fieldRow(this, {profile_});
    (void)qualityLeft->addRow(tr("Profile"), profileRow_);
    bitrate_ = new kit::KValueField(this);
    bitrate_->setObjectName(QStringLiteral("exportBitrate"));
    bitrate_->setAccessibleName(tr("Bit rate"));
    bitrate_->setIntegerMode(true);
    bitrate_->setIntegerRange(1, 2'000'000);
    bitrate_->setUnit(tr("kb/s"));
    bitrate_->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    bitrateRow_ = fieldRow(this, {bitrate_});
    (void)qualityLeft->addRow(tr("Bit rate"), bitrateRow_);
    compression_ = dropdown(this, QStringLiteral("exportCompression"), tr("Compression"));
    compressionRow_ = fieldRow(this, {compression_});
    (void)qualityLeft->addRow(tr("Compression"), compressionRow_);
    bakeLook_ = check(this, QStringLiteral("exportBakeLook"), tr("Bake in the look"));
    (void)qualityLeft->addCheckRow(tr("Look"), *bakeLook_, tr("Bake in the look"));
    lookRow_ = bakeLook_;
    includeAudio_ = check(this, QStringLiteral("exportIncludeAudio"), tr("Include audio"));
    (void)qualityLeft->addCheckRow(tr("Audio"), *includeAudio_, tr("Include audio"));
    audioRow_ = includeAudio_;
    // #570: bring the programme to a loudness target (EBU R128) under a true-peak ceiling.
    normalizeLoudness_ = check(this, QStringLiteral("exportNormalizeLoudness"), tr("Normalize loudness"));
    (void)qualityLeft->addCheckRow(tr("Loudness"), *normalizeLoudness_, tr("Normalize loudness"));
    loudnessTarget_ = new kit::KValueField(this);
    loudnessTarget_->setObjectName(QStringLiteral("exportLoudnessTarget"));
    loudnessTarget_->setAccessibleName(tr("Loudness target"));
    loudnessTarget_->setIntegerMode(true);
    loudnessTarget_->setIntegerRange(-40, -5);
    loudnessTarget_->setUnit(tr("LUFS"));
    loudnessTarget_->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    loudnessTargetRow_ = fieldRow(this, {loudnessTarget_});
    (void)qualityLeft->addRow(tr("Target"), loudnessTargetRow_);
    truePeak_ = new kit::KValueField(this);
    truePeak_->setObjectName(QStringLiteral("exportTruePeak"));
    truePeak_->setAccessibleName(tr("True peak limit"));
    truePeak_->setIntegerMode(true);
    truePeak_->setIntegerRange(-12, 0);
    truePeak_->setUnit(tr("dBTP"));
    truePeak_->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    truePeakRow_ = fieldRow(this, {truePeak_});
    (void)qualityLeft->addRow(tr("True peak"), truePeakRow_);
    colourSpace_ = dropdown(this, QStringLiteral("exportColourSpace"), tr("Color space"));
    colourSpaceRow_ = fieldRow(this, {colourSpace_});
    (void)qualityRight->addRow(tr("Color space"), colourSpaceRow_);
    display_ = dropdown(this, QStringLiteral("exportDisplay"), tr("Display"));
    displayRow_ = fieldRow(this, {display_});
    (void)qualityRight->addRow(tr("Display"), displayRow_);
    view_ = dropdown(this, QStringLiteral("exportView"), tr("View"));
    viewRow_ = fieldRow(this, {view_});
    (void)qualityRight->addRow(tr("View"), viewRow_);

    // File numbers, for image sequences only.
    auto [numbering, numberingLeft, numberingRight] =
        group(tr("Numbering"), QStringLiteral("exportOutputModuleNumbering"));
    numbering_ = numbering;
    startNumber_ = new kit::KValueField(this);
    startNumber_->setObjectName(QStringLiteral("exportSequenceStart"));
    startNumber_->setAccessibleName(tr("Start number"));
    startNumber_->setIntegerMode(true);
    startNumber_->setIntegerRange(0, 1'000'000'000);
    startNumber_->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    (void)numberingLeft->addRow(tr("Start number"), fieldRow(this, {startNumber_}));
    frameNumbers_ = check(this, QStringLiteral("exportSequenceFrameNumbers"), tr("Use the frame numbers"));
    (void)numberingLeft->addCheckRow(QString{}, *frameNumbers_, tr("Use the frame numbers"));
    padding_ = new kit::KValueField(this);
    padding_->setObjectName(QStringLiteral("exportPadding"));
    padding_->setAccessibleName(tr("Digits"));
    padding_->setIntegerMode(true);
    padding_->setIntegerRange(1, 20);
    padding_->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    (void)numberingRight->addRow(tr("Digits"), fieldRow(this, {padding_}));

    fillFormats();
    {
        const QSignalBlocker block(format_);
        const int index = format_->findData(qs(output.record.presetId));
        format_->setCurrentIndex(index);
        format_->setMutedValue(index < 0);
    }
    overwrite_->setCurrentIndex(
        std::max(0, overwrite_->findData(static_cast<int>(output.record.destination.overwrite))));
    import_->setChecked(output.record.importIntoAssets);
    showSettings(base_);

    connect(format_, &kit::KDropdown::activated, this, [this](int) { formatChosen(); });
    connect(savePreset_, &QAbstractButton::clicked, this, [this] { (void)promptPresetName(); });
    connect(normalizeLoudness_, &QAbstractButton::toggled, this, [this](bool) { updateLoudnessRows(base_.format); });
    connect(frameNumbers_, &QAbstractButton::toggled, this,
            [this](const bool on) { startNumber_->setEnabled(!on); });
    connect(display_, &kit::KDropdown::activated, this, [this](int) {
        // A view belongs to its display: keep it only when the new display offers it.
        auto settings = this->settings();
        const auto views = colours_.views(display_->currentData().toString());
        if (!settings.viewName.empty() &&
            std::ranges::find(views, QString::fromStdString(settings.viewName)) == views.end())
            settings.viewName.clear();
        fillColour(settings);
    });

    buttonBox()->setObjectName(QStringLiteral("exportOutputModuleButtons"));
    addButton(Role::Accept, tr("OK"))->setObjectName(QStringLiteral("exportOutputModuleOk"));
    addButton(Role::Reject, tr("Cancel"))->setObjectName(QStringLiteral("exportOutputModuleCancel"));
    setInitialFocus(format_);
}

const host::ExportPreset* OutputModuleDialog::preset(const QString& id) const {
    if (catalogue_ == nullptr)
        return nullptr;
    for (const auto& candidate : catalogue_->presets)
        if (qs(candidate.id) == id)
            return &candidate;
    return nullptr;
}

void OutputModuleDialog::fillFormats() {
    const QSignalBlocker block(format_);
    format_->clearItems();
    if (catalogue_ == nullptr)
        return;
    struct Group {
        host::ExportPresetCategory category;
        QString title;
    };
    // Video first, then image sequences, audio and the artist's own (ADR 0033, presets).
    const Group groups[] = {{host::ExportPresetCategory::Video, tr("Video")},
                            {host::ExportPresetCategory::VfxSequences, tr("Image sequences")},
                            {host::ExportPresetCategory::Audio, tr("Audio")},
                            {host::ExportPresetCategory::User, tr("My presets")}};
    for (const auto& group : groups) {
        bool header = false;
        for (const auto& candidate : catalogue_->presets) {
            if (candidate.category != group.category)
                continue;
            if (!header) {
                // A group title: shown, never chosen.
                format_->setItemEnabled(format_->addItem(group.title, QVariant{}), false);
                header = true;
            }
            // Every format is listed and chosen alike: the engine bundles its encoders.
            format_->setItemToolTip(format_->addItem(qs(candidate.displayName), qs(candidate.id)),
                                    qs(candidate.description));
        }
    }
}

void OutputModuleDialog::formatChosen() {
    const auto* chosen = preset(format_->currentData().toString());
    if (chosen == nullptr)
        return;
    // Another format starts from its preset; the output's own settings come back with its own.
    base_ = chosen->id == output_.record.presetId ? output_.record.settings : chosen->settings;
    format_->setMutedValue(false);
    main_->setTitle(quoted(qs(chosen->displayName)));
    showPresetRefusal({});
    showSettings(base_);
}

void OutputModuleDialog::showPresetRefusal(const QString& reason) {
    presetError_->setText(reason);
    presetError_->setVisible(!reason.isEmpty());
}

std::optional<QString> OutputModuleDialog::presetNameIssue(const QString& name) const {
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return tr("Enter a preset name.");
    // The catalogue's own limits: at most 256 UTF-8 bytes, no control characters.
    if (trimmed.toUtf8().size() > 256)
        return tr("A preset name is at most 256 UTF-8 bytes.");
    if (std::ranges::any_of(trimmed, [](const QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; }))
        return tr("A preset name cannot contain tabs or line breaks.");
    // Saving never replaces a preset: a name already in the Format list, built-in or the artist's
    // own, is refused rather than listed twice.
    if (catalogue_ != nullptr)
        for (const auto& candidate : catalogue_->presets)
            if (qs(candidate.displayName).compare(trimmed, Qt::CaseInsensitive) == 0)
                return tr("A preset with this name already exists.");
    return std::nullopt;
}

kit::KDialog* OutputModuleDialog::promptPresetName() {
    showPresetRefusal({});
    kit::KTextPromptSpec spec;
    spec.title = tr("Save as Preset");
    spec.objectName = QStringLiteral("exportSavePresetPrompt");
    spec.label = tr("Preset name");
    spec.acceptText = tr("Save");
    spec.validate = [this](const QString& name) { return presetNameIssue(name); };
    auto* prompt = kit::buildTextPrompt(*this, spec);
    auto* field = prompt->findChild<kit::KLineEdit*>(spec.objectName + QStringLiteral("Field"));
    QPointer<OutputModuleDialog> self(this);
    QPointer<kit::KLineEdit> text(field);
    prompt->open([self, text](const Role role) {
        if (self != nullptr && text != nullptr && role == Role::Accept)
            (void)self->saveAsPreset(text->text());
    });
    return prompt;
}

std::optional<QString> OutputModuleDialog::saveAsPreset(const QString& name) {
    const auto refuse = [this](const QString& reason) {
        showPresetRefusal(reason);
        return std::optional(reason);
    };
    if (presets_ == nullptr)
        return refuse(tr("Presets cannot be saved in this window."));
    if (const auto issue = presetNameIssue(name))
        return refuse(*issue);
    const auto trimmed = name.trimmed();
    const auto settings = this->settings();
    switch (presets_->saveUserPreset(trimmed.toStdString(), settings)) {
    case host::ExportUserPresetStatus::Accepted:
        break;
    case host::ExportUserPresetStatus::NameTaken:
        return refuse(tr("A preset with this name already exists."));
    case host::ExportUserPresetStatus::NameInvalid:
        return refuse(tr("Enter a preset name without tabs or line breaks."));
    case host::ExportUserPresetStatus::StoreUnavailable:
        return refuse(tr("Presets cannot be saved right now: Bloom could not read or write your "
                         "saved presets."));
    default:
        return refuse(tr("The preset could not be saved."));
    }
    // The catalogue lists the new preset at once: choose it, keeping every field as it is.
    catalogue_ = presets_->catalogue();
    const host::ExportPreset* saved = nullptr;
    if (catalogue_ != nullptr)
        for (const auto& candidate : catalogue_->presets)
            if (candidate.category == host::ExportPresetCategory::User && qs(candidate.displayName) == trimmed)
                saved = &candidate;
    fillFormats();
    if (saved == nullptr)
        return refuse(tr("The preset was saved but is not listed yet; choose it from My presets."));
    {
        const QSignalBlocker block(format_);
        format_->setCurrentIndex(format_->findData(qs(saved->id)));
    }
    format_->setMutedValue(false);
    base_ = settings;
    main_->setTitle(quoted(trimmed));
    showPresetRefusal({});
    return std::nullopt;
}

void OutputModuleDialog::fillColour(const document::ExportOutputSettings& settings) {
    const auto fillChoices = [](kit::KDropdown& field, const QString& first,
                                const std::vector<std::pair<QString, QString>>& choices,
                                const QString& current) {
        const QSignalBlocker block(field);
        field.clearItems();
        field.addItem(first, QString{});
        for (const auto& [label, id] : choices)
            field.addItem(label, id);
        int index = field.findData(current);
        if (index < 0) {
            // A name the configuration no longer offers stays visible rather than turning blank.
            index = field.addItem(current, current);
            field.setItemToolTip(index, tr("Not in this project's color setup"));
        }
        field.setCurrentIndex(index);
    };
    fillChoices(*colourSpace_, tr("Working color space"), colours_.colourSpaces,
                qs(settings.outputColorSpaceId));
    const auto display = qs(settings.displayName);
    std::vector<std::pair<QString, QString>> displays;
    for (const auto& name : colours_.displays())
        displays.emplace_back(ExportColourChoices::readable(name), name);
    std::vector<std::pair<QString, QString>> views;
    for (const auto& name : colours_.views(display))
        views.emplace_back(ExportColourChoices::readable(name), name);
    fillChoices(*display_, tr("Project default"), displays, display);
    fillChoices(*view_, tr("Project default"), views, qs(settings.viewName));
}

void OutputModuleDialog::showSettings(const document::ExportOutputSettings& settings) {
    refreshing_ = true;
    const auto format = settings.format;
    const bool image = isImage(format);
    const bool movie = isMovie(format);
    const auto& description = host::exportFormatDescription(format);
    const auto profiles = choicesOf(description.codecProfiles);
    fill(*profile_, profiles);
    select(*profile_, qs(settings.codecProfile.empty() ? std::string(description.defaultCodecProfile)
                                                       : settings.codecProfile));
    fill(*compression_, choicesOf(description.compressions));
    select(*compression_, qs(settings.compression.empty() ? std::string(description.defaultCompression)
                                                          : settings.compression));
    const bool sceneReferred = format == ExportFormat::OpenExr || format == ExportFormat::Dpx;
    const bool displayReferred = format == ExportFormat::Png || format == ExportFormat::Tiff || movie;
    qualityLeft_->setRowVisible(profileRow_, profiles.size() > 1);
    qualityLeft_->setRowVisible(bitrateRow_, takesBitrate(format));
    bitrate_->setIntegerValue(settings.videoBitrateKbps.value_or(
        static_cast<std::uint32_t>(defaultBitrateKbps(format))));
    qualityLeft_->setRowVisible(compressionRow_, !description.compressions.empty());
    qualityLeft_->setRowVisible(lookRow_, format != ExportFormat::Wav);
    qualityLeft_->setRowVisible(audioRow_, movie);
    qualityRight_->setRowVisible(colourSpaceRow_, sceneReferred);
    qualityRight_->setRowVisible(displayRow_, displayReferred);
    qualityRight_->setRowVisible(viewRow_, displayReferred);
    fillColour(settings);
    bakeLook_->setChecked(settings.bakeLook);
    includeAudio_->setChecked(settings.includeAudio);
    cachedFrames_->setChecked(settings.useCachedFrames);
    normalizeLoudness_->setChecked(settings.loudness.has_value());
    loudnessTarget_->setIntegerValue(std::llround(settings.loudness.value_or({}).targetLufs));
    truePeak_->setIntegerValue(std::llround(settings.loudness.value_or({}).truePeakDbtp));
    qualityLeft_->setRowVisible(normalizeLoudness_, movie || format == ExportFormat::Wav);
    updateLoudnessRows(format);
    numbering_->setVisible(image);
    frameNumbers_->setChecked(!settings.naming.startFrame.has_value());
    startNumber_->setIntegerValue(static_cast<std::int64_t>(settings.naming.startFrame.value_or(1)));
    startNumber_->setEnabled(settings.naming.startFrame.has_value());
    padding_->setIntegerValue(settings.naming.framePadding);

    refreshing_ = false;
}

void OutputModuleDialog::updateLoudnessRows(const document::ExportFormat format) {
    const bool applies = normalizeLoudness_->isChecked() && (isMovie(format) || format == ExportFormat::Wav);
    qualityLeft_->setRowVisible(loudnessTargetRow_, applies);
    qualityLeft_->setRowVisible(truePeakRow_, applies);
}

QString OutputModuleDialog::presetId() const {
    const auto id = format_->currentData().toString();
    return id.isEmpty() ? qs(output_.record.presetId) : id;
}

document::ExportOutputSettings OutputModuleDialog::settings() const {
    auto settings = base_;
    const auto format = settings.format;
    if (profile_->count() > 1)
        settings.codecProfile = profile_->currentData().toString().toStdString();
    if (takesBitrate(format)) {
        // An unchanged default stays the codec's default, never written out as a number.
        const auto kbps = bitrate_->integerValue();
        if (base_.videoBitrateKbps || kbps != defaultBitrateKbps(format))
            settings.videoBitrateKbps = static_cast<std::uint32_t>(kbps);
    }
    if (compression_->count() > 0)
        settings.compression = compression_->currentData().toString().toStdString();
    if (format == ExportFormat::OpenExr || format == ExportFormat::Dpx)
        settings.outputColorSpaceId = colourSpace_->currentData().toString().toStdString();
    if (format == ExportFormat::Png || format == ExportFormat::Tiff || isMovie(format)) {
        settings.displayName = display_->currentData().toString().toStdString();
        settings.viewName = view_->currentData().toString().toStdString();
    }
    if (format != ExportFormat::Wav)
        settings.bakeLook = bakeLook_->isChecked();
    if (isMovie(format))
        settings.includeAudio = includeAudio_->isChecked();
    settings.useCachedFrames = cachedFrames_->isChecked();
    if (isMovie(format) || format == ExportFormat::Wav) {
        if (!normalizeLoudness_->isChecked()) {
            settings.loudness.reset();
        } else {
            // An unchanged field keeps the saved value, which may be finer than a whole number.
            auto loudness = base_.loudness.value_or(document::ExportLoudness{});
            if (!base_.loudness || std::llround(loudness.targetLufs) != loudnessTarget_->integerValue())
                loudness.targetLufs = static_cast<double>(loudnessTarget_->integerValue());
            if (!base_.loudness || std::llround(loudness.truePeakDbtp) != truePeak_->integerValue())
                loudness.truePeakDbtp = static_cast<double>(truePeak_->integerValue());
            settings.loudness = loudness;
        }
    }
    if (isImage(format)) {
        settings.naming.startFrame =
            frameNumbers_->isChecked() ? std::nullopt
                                       : std::optional(static_cast<std::uint64_t>(startNumber_->integerValue()));
        settings.naming.framePadding = static_cast<std::uint32_t>(padding_->integerValue());
    }
    // An unchanged default stays the format's default, never written out as its token.
    const auto& description = host::exportFormatDescription(format);
    if (base_.codecProfile.empty() && settings.codecProfile == description.defaultCodecProfile)
        settings.codecProfile.clear();
    if (base_.compression.empty() && settings.compression == description.defaultCompression)
        settings.compression.clear();
    return settings;
}

std::optional<ExportOutputEdit> OutputModuleDialog::edit() const {
    ExportOutputEdit change;
    bool any = false;
    const auto id = presetId().toStdString();
    const auto settings = this->settings();
    if (id != output_.record.presetId) {
        change.presetId = id;
        change.settings = settings;
        any = true;
    } else if (settings != output_.record.settings) {
        change.settings = settings;
        any = true;
    }
    const auto policy = static_cast<document::ExportOverwritePolicy>(overwrite_->currentData().toInt());
    if (policy != output_.record.destination.overwrite) {
        auto destination = output_.record.destination;
        destination.overwrite = policy;
        change.destination = destination;
        any = true;
    }
    if (import_->isChecked() != output_.record.importIntoAssets) {
        change.importIntoAssets = import_->isChecked();
        any = true;
    }
    return any ? std::optional(change) : std::nullopt;
}

bool OutputModuleDialog::shows(QWidget* const field) const {
    // A hidden row or group hides its field, before and after the dialog is shown.
    return field->isVisibleTo(this);
}

} // namespace bloom::ui::exportpanel

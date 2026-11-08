#pragma once

// Output Module, opened from an output's "Output Module" link (a compositor's output module
// settings, with only what an export output carries). Grouped like a compositor's:
//
//   “ProRes 422 HQ”  Format, If a file is there       | Post-render, Cached frames
//   Quality          the format's profile or compression,| its colour space, or display and view
//                    the look and audio
//   Numbering        start number (image sequences)    | digits
//
// Only the settings that apply to the chosen format show, and every format is offered alike.
// Choosing another format takes that preset's settings. OK writes every change as one undoable edit; Cancel changes nothing.
//
// Save as preset (the button beside Format) names the settings the fields describe and saves them
// as the artist's own preset through the queue adapter's catalogue. It is a per-user preference,
// outside Undo and the project: Cancel keeps the preset but leaves the output alone. The new
// preset is listed under My presets and chosen in Format, so OK gives the output that preset.

#include "export_colour_choices.hpp"

#include <bloom/host/export_queue.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <QPointer>

#include <functional>
#include <optional>

namespace bloom::ui::kit {
class KButton;
class KCheckBox;
class KDialog;
class KDropdown;
class KIconButton;
class KLabel;
class KSection;
class KValueField;
} // namespace bloom::ui::kit

namespace bloom::ui::exportpanel {

class OutputModuleDialog final : public kit::KDialog {
    Q_OBJECT

  public:
    // `presets` saves user presets (Save as preset); without it the button is not offered.
    OutputModuleDialog(const host::ExportOutputView& output,
                       std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue,
                       ExportColourChoices colours, QWidget& owner,
                       ExportQueueAdapter* presets = nullptr);

    // Every change the fields make, as one edit; nullopt when nothing changed.
    [[nodiscard]] std::optional<ExportOutputEdit> edit() const;
    // The settings the fields describe.
    [[nodiscard]] document::ExportOutputSettings settings() const;
    [[nodiscard]] QString presetId() const;

    // Save as preset: opens the name prompt (asynchronous, over this dialog) and returns it.
    kit::KDialog* promptPresetName();
    // Why `name` cannot be a new preset's name (empty, too long, a line break, or a name the
    // Format list already has, ignoring case); nullopt when it can.
    [[nodiscard]] std::optional<QString> presetNameIssue(const QString& name) const;
    // Saves the current settings as the preset `name` and chooses it in Format. Returns the reason
    // when it could not be saved (also shown under Format).
    std::optional<QString> saveAsPreset(const QString& name);

    // Test seams.
    [[nodiscard]] kit::KDropdown* format() const noexcept { return format_; }
    [[nodiscard]] kit::KDropdown* overwrite() const noexcept { return overwrite_; }
    [[nodiscard]] kit::KDropdown* profile() const noexcept { return profile_; }
    [[nodiscard]] kit::KDropdown* compression() const noexcept { return compression_; }
    [[nodiscard]] kit::KDropdown* colourSpace() const noexcept { return colourSpace_; }
    [[nodiscard]] kit::KDropdown* display() const noexcept { return display_; }
    [[nodiscard]] kit::KDropdown* view() const noexcept { return view_; }
    [[nodiscard]] kit::KCheckBox* bakeLook() const noexcept { return bakeLook_; }
    [[nodiscard]] kit::KCheckBox* includeAudio() const noexcept { return includeAudio_; }
    [[nodiscard]] kit::KCheckBox* normalizeLoudness() const noexcept { return normalizeLoudness_; }
    [[nodiscard]] kit::KCheckBox* cachedFrames() const noexcept { return cachedFrames_; }
    [[nodiscard]] kit::KCheckBox* importIntoAssets() const noexcept { return import_; }
    [[nodiscard]] kit::KValueField* startNumber() const noexcept { return startNumber_; }
    [[nodiscard]] kit::KCheckBox* frameNumbers() const noexcept { return frameNumbers_; }
    [[nodiscard]] kit::KValueField* padding() const noexcept { return padding_; }
    [[nodiscard]] kit::KValueField* bitrate() const noexcept { return bitrate_; }
    [[nodiscard]] QWidget* bitrateRow() const noexcept { return bitrateRow_; }
    [[nodiscard]] kit::KSection* mainSection() const noexcept { return main_; }
    [[nodiscard]] kit::KIconButton* savePresetButton() const noexcept { return savePreset_; }
    [[nodiscard]] kit::KLabel* presetError() const noexcept { return presetError_; }
    // Whether the row holding `field` shows.
    [[nodiscard]] bool shows(QWidget* field) const;

  private:
    void fillFormats();
    void showSettings(const document::ExportOutputSettings& settings);
    void updateLoudnessRows(document::ExportFormat format);
    void fillColour(const document::ExportOutputSettings& settings);
    void formatChosen();
    void showPresetRefusal(const QString& reason);
    [[nodiscard]] const host::ExportPreset* preset(const QString& id) const;

    host::ExportOutputView output_;
    std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue_;
    ExportColourChoices colours_;
    QPointer<ExportQueueAdapter> presets_;
    // The settings the dialog started from for the current format (the output's own, or the chosen
    // preset's after a format change); the fields override what applies.
    document::ExportOutputSettings base_;
    kit::KSection* main_ = nullptr;
    kit::KSection* quality_ = nullptr;
    kit::KSection* numbering_ = nullptr;
    kit::KForm* mainLeft_ = nullptr;
    kit::KForm* qualityLeft_ = nullptr;
    kit::KForm* qualityRight_ = nullptr;
    kit::KDropdown* format_ = nullptr;
    kit::KIconButton* savePreset_ = nullptr;
    kit::KLabel* presetError_ = nullptr;
    kit::KDropdown* overwrite_ = nullptr;
    kit::KCheckBox* import_ = nullptr;
    kit::KCheckBox* cachedFrames_ = nullptr;
    kit::KDropdown* profile_ = nullptr;
    kit::KDropdown* compression_ = nullptr;
    kit::KDropdown* colourSpace_ = nullptr;
    kit::KDropdown* display_ = nullptr;
    kit::KDropdown* view_ = nullptr;
    kit::KCheckBox* bakeLook_ = nullptr;
    kit::KCheckBox* includeAudio_ = nullptr;
    kit::KCheckBox* normalizeLoudness_ = nullptr;
    kit::KValueField* loudnessTarget_ = nullptr;
    kit::KValueField* truePeak_ = nullptr;
    QWidget* loudnessTargetRow_ = nullptr;
    QWidget* truePeakRow_ = nullptr;
    kit::KValueField* startNumber_ = nullptr;
    kit::KCheckBox* frameNumbers_ = nullptr;
    kit::KValueField* padding_ = nullptr;
    kit::KValueField* bitrate_ = nullptr;
    QWidget* bitrateRow_ = nullptr;
    QWidget* profileRow_ = nullptr;
    QWidget* compressionRow_ = nullptr;
    QWidget* colourSpaceRow_ = nullptr;
    QWidget* displayRow_ = nullptr;
    QWidget* viewRow_ = nullptr;
    QWidget* lookRow_ = nullptr;
    QWidget* audioRow_ = nullptr;
    bool refreshing_ = false;
};

} // namespace bloom::ui::exportpanel

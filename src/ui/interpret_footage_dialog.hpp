#pragma once

// Interpret Footage (#381): the one dialog for an asset's interpretation, after After Effects. It
// edits the interpretation of the asset it opened on and reports only the members the artist
// changed, so applying it to a multi-selection changes exactly those members on every selected
// footage item. The caller turns the change into one SetAssetInterpretation command.

#include <bloom/commands/footage_interpretation_operations.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <QString>

#include <optional>
#include <string>
#include <vector>

class QLineEdit;

namespace bloom::ui {
namespace kit {
class KCheckBox;
class KColorChip;
class KDropdown;
class KLabel;
class KValueField;
} // namespace kit

// One input colour choice the Color Management dropdown offers.
struct InterpretColorChoice final {
    QString label;
    // "auto", "raw", "linear", "srgb" or "id:<OCIO id>".
    QString data;
};

class InterpretFootageDialog final : public kit::KDialog {
    Q_OBJECT
  public:
    // `asset` is the asset the dialog shows; `count` the footage items the change will apply to
    // (the title names it when more than one); `kinds` the kinds in the selection, which decide the
    // rows shown. `resolvedColor` is the space Auto resolves to for `asset`, shown beside Auto.
    InterpretFootageDialog(const document::AssetRecord& asset, std::size_t count,
                           std::vector<document::AssetKind> kinds,
                           std::vector<InterpretColorChoice> colors, QString resolvedColor,
                           QWidget& owner);

    // Replaces the colour choices (the OCIO catalogue arrives asynchronously).
    void setColorChoices(std::vector<InterpretColorChoice> colors, const QString& status);
    // The members the fields now differ from the asset in, or nullopt while a field is invalid.
    [[nodiscard]] std::optional<commands::AssetInterpretationChange> change() const;
    // The plain reason the dialog cannot be accepted; empty when it can.
    [[nodiscard]] QString issue() const;

    // The fields, for tests.
    [[nodiscard]] kit::KDropdown* rateMode() const noexcept { return rateMode_; }
    [[nodiscard]] QLineEdit* rate() const noexcept { return rate_; }
    [[nodiscard]] kit::KDropdown* timecodeMode() const noexcept { return timecodeMode_; }
    [[nodiscard]] QLineEdit* timecode() const noexcept { return timecode_; }
    [[nodiscard]] kit::KDropdown* alpha() const noexcept { return alpha_; }
    [[nodiscard]] kit::KCheckBox* invertAlpha() const noexcept { return invert_; }
    [[nodiscard]] kit::KColorChip* matte() const noexcept { return matte_; }
    [[nodiscard]] kit::KDropdown* fieldOrder() const noexcept { return fieldOrder_; }
    [[nodiscard]] kit::KCheckBox* deinterlace() const noexcept { return deinterlace_; }
    [[nodiscard]] kit::KDropdown* pixelAspect() const noexcept { return pixelAspect_; }
    [[nodiscard]] kit::KValueField* loop() const noexcept { return loop_; }
    [[nodiscard]] kit::KDropdown* missingFrames() const noexcept { return missing_; }
    [[nodiscard]] kit::KDropdown* color() const noexcept { return color_; }

  private:
    [[nodiscard]] std::optional<document::FrameRate> parsedRate() const;
    [[nodiscard]] document::FrameRate timecodeRate() const;
    [[nodiscard]] std::optional<document::AssetInterpretation> interpretation() const;
    void refresh();

    document::AssetRecord asset_;
    bool timed_ = false;
    bool sequence_ = false;
    QLineEdit* rate_ = nullptr;
    kit::KDropdown* rateMode_ = nullptr;
    kit::KDropdown* timecodeMode_ = nullptr;
    QLineEdit* timecode_ = nullptr;
    kit::KDropdown* alpha_ = nullptr;
    kit::KCheckBox* invert_ = nullptr;
    kit::KColorChip* matte_ = nullptr;
    kit::KDropdown* fieldOrder_ = nullptr;
    kit::KCheckBox* deinterlace_ = nullptr;
    kit::KDropdown* pixelAspect_ = nullptr;
    kit::KValueField* loop_ = nullptr;
    kit::KDropdown* missing_ = nullptr;
    kit::KDropdown* color_ = nullptr;
    kit::KLabel* colorStatus_ = nullptr;
    QWidget* rateRow_ = nullptr;
    QWidget* timecodeRow_ = nullptr;
    QWidget* matteRow_ = nullptr;
    QString initialColor_;
    QString resolvedColor_;
};

// The input colour choice data of an interpretation ("auto", "raw", "id:<id>", ...).
[[nodiscard]] QString interpretColorData(const document::AssetInterpretation& interpretation);

// "23.976" -> 24000/1001: the NTSC decimals Interpret Footage names map to their exact rationals;
// any other decimal or "n/d" is exact as typed.
[[nodiscard]] std::optional<document::FrameRate> parseFootageFrameRate(const QString& text);
[[nodiscard]] QString footageFrameRateText(document::FrameRate rate);

} // namespace bloom::ui

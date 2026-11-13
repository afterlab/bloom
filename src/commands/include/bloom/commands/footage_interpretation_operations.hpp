#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/document/asset.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Interpret Footage (#381).
namespace bloom::commands {

// The members an Interpret Footage edit sets. An absent member is left as it is on every asset,
// so one change can apply only what the artist changed to a mixed multi-selection. A member an
// asset's kind does not have (a frame rate on a still, missing frames on a video) is skipped for
// that asset rather than refusing the edit.
struct AssetInterpretationChange final {
    std::optional<std::optional<document::FrameRate>> frameRate;
    std::optional<std::optional<document::CompositionTimecode>> startTimecode;
    std::optional<document::AssetAlphaMode> alphaMode;
    std::optional<bool> invertAlpha;
    std::optional<std::array<double, 3>> matteColor;
    std::optional<document::AssetFieldOrder> fieldOrder;
    std::optional<bool> deinterlace;
    std::optional<std::optional<core::PixelAspectRatio>> pixelAspect;
    std::optional<std::uint32_t> loopCount;
    std::optional<document::AssetMissingFrames> missingFrames;
    // #718: the asset's override of "Keep local copies of source media".
    std::optional<document::AssetLocalCopies> localCopies;
    // A still's duration (absent inside: the composition's length).
    std::optional<std::optional<core::RationalTime>> stillDuration;
    // Input color: the legacy choice and the exact OCIO id, with SetAssetInputColor's rule (an id
    // uses Auto in the legacy field).
    std::optional<std::pair<document::AssetColorSpace, std::string>> inputColor;

    // Every member of `interpretation`: what Paste Interpretation applies.
    [[nodiscard]] static AssetInterpretationChange
    everything(const document::AssetInterpretation& interpretation);
    [[nodiscard]] bool empty() const noexcept;
    // `interpretation` with every member this change sets that `kind` has.
    [[nodiscard]] document::AssetInterpretation
    appliedTo(document::AssetKind kind, document::AssetInterpretation interpretation) const;
    friend bool operator==(const AssetInterpretationChange&,
                           const AssetInterpretationChange&) = default;
};

// One undoable edit of the interpretation of one or more assets: a single history entry however
// many assets it changes. Refused when an asset is missing, when no listed asset is footage (an
// image, a sequence or a video), or when a value is out of range. A layer whose out point ends
// where its footage ended follows the new length (a conformed rate or a new loop count), so the
// layer still shows the whole footage.
class SetAssetInterpretation final : public Operation {
  public:
    SetAssetInterpretation(std::vector<document::AssetId> assets, AssetInterpretationChange change)
        : assets_(std::move(assets)), change_(std::move(change)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.set-interpretation";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] const std::vector<document::AssetId>& assets() const noexcept { return assets_; }
    [[nodiscard]] const AssetInterpretationChange& change() const noexcept { return change_; }

  private:
    std::vector<document::AssetId> assets_;
    AssetInterpretationChange change_;
};

// True for the kinds Interpret Footage applies to: images, sequences and videos.
[[nodiscard]] bool isInterpretableFootage(document::AssetKind kind) noexcept;

} // namespace bloom::commands

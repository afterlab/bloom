// MCP parity (#389) for Interpret Footage (#381): the Assets panel's Interpret Footage dialog and
// Paste Interpretation are bloom.asset.set-interpretation; query kind=assets reads every member.

#include <bloom/commands/footage_interpretation_operations.hpp>

#include "operation_registry_parity.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting::detail::parity {
namespace {

using document::AssetId;

const std::string kSetInterpretation =
    R"(Sets how footage is interpreted, as OK in the Assets panel's Interpret Footage dialog does: )"
    R"(only the members given change, on every listed image, sequence or video, in ONE undo step. )"
    R"(A member a kind does not have is skipped for that asset (a still has no frame rate, a )"
    R"(video no missing frames). frameRate: a sequence's assumed rate or a video's conformed )"
    R"(rate, as "24", "23.976" (24000/1001), "29.97" (30000/1001) or "n/d"; "" restores the )"
    R"(file's rate (a sequence then advances one member per composition frame). startTime: the )"
    R"(start timecode override as an exact time (seconds, or [numerator, denominator]), with )"
    R"(dropFrame; sourceTimecode: true restores the source's own. alpha: "guess" (the file's )"
    R"(declared association), "ignore", "straight" or )"
    R"("premultiplied" (matted with matteColor, three numbers 0-1 in the file's encoding). )"
    R"(invertAlpha, deinterlace: true or false. fieldOrder: "file", "progressive", "upper" or )"
    R"("lower". pixelAspect: "n/d" (for example "10/11", "2/1") or "file". loop: 1-1000 passes. )"
    R"(missingFrames: "hold" or "nothing". legacy and inputColorSpaceId: the input color as )"
    R"(bloom.asset.set-input-color takes it ("raw" is Preserve RGB, "linear" Interpret as Linear). )"
    R"(A layer whose out point ended with its footage follows the new length. )"
    R"(Errors: "Asset does not exist"; "Interpret Footage applies to images, sequences and )"
    R"(videos" when no listed asset is footage; a value out of range names its member. )"
    R"(Undo: one step. MCP example: {"op":"bloom.asset.set-interpretation","args":{"assets":[3],)"
    R"("frameRate":"23.976","loop":2,"startTime":3600}}.)";

template <typename Enum, std::size_t Size>
[[nodiscard]] std::optional<Enum> token(const std::string& text,
                                        const std::array<std::pair<const char*, Enum>, Size>& map) {
    for (const auto& [name, value] : map)
        if (text == name)
            return value;
    return std::nullopt;
}

} // namespace

void addFootageOperations(std::vector<OperationDescriptor>& descriptors) {
    add(descriptors, "bloom.asset.set-interpretation", kSetInterpretation,
        {need("assets", ValueKind::Array, ValueArray{Value(StableId{1})},
              "Array of asset ids. The footage to interpret."),
         maybe("frameRate", ValueKind::String, "23.976", "Text. Assumed or conformed rate."),
         maybe("startTime", ValueKind::Integer, std::int64_t{3600},
               "Exact time. The start timecode override (3600 is 01:00:00:00)."),
         maybe("dropFrame", ValueKind::Boolean, false,
               "Boolean. Label the override with drop-frame timecode (29.97 and 59.94)."),
         maybe("sourceTimecode", ValueKind::Boolean, true,
               "Boolean. true restores the source's own start timecode."),
         maybe("alpha", ValueKind::String, "straight", "guess, ignore, straight or premultiplied."),
         maybe("invertAlpha", ValueKind::Boolean, false, "Boolean. Invert the alpha channel."),
         maybe("matteColor", ValueKind::Array, ValueArray{Value(0.0), Value(0.0), Value(0.0)},
               "Three numbers 0-1. The premultiplied matte color."),
         maybe("fieldOrder", ValueKind::String, "upper", "file, progressive, upper or lower."),
         maybe("deinterlace", ValueKind::Boolean, false, "Boolean. Deinterlace the fields."),
         maybe("pixelAspect", ValueKind::String, "1/1", "Text. n/d, or file."),
         maybe("loop", ValueKind::Integer, std::int64_t{1},
               "Integer 1-1000. Passes of the footage."),
         maybe("missingFrames", ValueKind::String, "hold", "hold or nothing."),
         maybe("legacy", ValueKind::String, "auto", "auto, srgb, linear or raw."),
         maybe("inputColorSpaceId", ValueKind::String, "",
               "An exact color-space id in the selected OCIO config.")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            auto assets = read.ids<AssetId>("assets");
            const auto rate = read.text("frameRate");
            const auto start = read.time("startTime");
            const auto drop = read.boolean("dropFrame");
            const auto sourceTimecode = read.boolean("sourceTimecode");
            const auto alpha = read.text("alpha");
            const auto invert = read.boolean("invertAlpha");
            const auto matte = read.vector("matteColor");
            const auto fields = read.text("fieldOrder");
            const auto deinterlace = read.boolean("deinterlace");
            const auto aspect = read.text("pixelAspect");
            const auto loop = read.integer("loop");
            const auto missing = read.text("missingFrames");
            const auto legacy = read.text("legacy");
            const auto inputId = read.text("inputColorSpaceId");
            if (read.failed())
                return read.error();
            commands::AssetInterpretationChange change;
            if (rate) {
                if (rate->empty() || *rate == "file")
                    change.frameRate = std::optional<document::FrameRate>{};
                else if (const auto parsed = document::parseFootageFrameRate(*rate))
                    change.frameRate = parsed;
                else
                    return argumentFailure(op, "frameRate", "expects 24, 23.976 or n/d");
            }
            if (sourceTimecode == true) {
                change.startTimecode = std::optional<document::CompositionTimecode>{};
            } else if (start) {
                if (*start < core::RationalTime{})
                    return argumentFailure(op, "startTime", "expects a time from 0");
                change.startTimecode = document::CompositionTimecode{*start, drop.value_or(false)};
            }
            if (alpha) {
                using document::AssetAlphaMode;
                const auto mode = token<AssetAlphaMode, 4>(
                    *alpha, {{{"guess", AssetAlphaMode::Guess},
                              {"ignore", AssetAlphaMode::Ignore},
                              {"straight", AssetAlphaMode::Straight},
                              {"premultiplied", AssetAlphaMode::Premultiplied}}});
                if (!mode)
                    return argumentFailure(op, "alpha",
                                           "expects guess, ignore, straight or premultiplied");
                change.alphaMode = mode;
            }
            change.invertAlpha = invert;
            if (matte) {
                const auto* color = std::get_if<document::Vec3d>(&*matte);
                if (color == nullptr)
                    return argumentFailure(op, "matteColor", "expects three numbers");
                change.matteColor = std::array<double, 3>{color->x, color->y, color->z};
            }
            if (fields) {
                using document::AssetFieldOrder;
                const auto order = token<AssetFieldOrder, 4>(
                    *fields, {{{"file", AssetFieldOrder::File},
                               {"progressive", AssetFieldOrder::Progressive},
                               {"upper", AssetFieldOrder::UpperFirst},
                               {"lower", AssetFieldOrder::LowerFirst}}});
                if (!order)
                    return argumentFailure(op, "fieldOrder",
                                           "expects file, progressive, upper or lower");
                change.fieldOrder = order;
            }
            change.deinterlace = deinterlace;
            if (aspect) {
                if (*aspect == "file") {
                    change.pixelAspect = std::optional<core::PixelAspectRatio>{};
                } else {
                    const auto ratio = document::parseFootageFrameRate(*aspect);
                    const auto pixel = ratio ? core::PixelAspectRatio::create(ratio->numerator(),
                                                                              ratio->denominator())
                                             : std::nullopt;
                    if (!pixel)
                        return argumentFailure(op, "pixelAspect", "expects n/d or file");
                    change.pixelAspect = pixel;
                }
            }
            if (loop) {
                if (*loop < 1 || *loop > document::kMaxAssetLoopCount)
                    return argumentFailure(op, "loop", "expects 1 to 1000");
                change.loopCount = static_cast<std::uint32_t>(*loop);
            }
            if (missing) {
                if (*missing == "hold")
                    change.missingFrames = document::AssetMissingFrames::HoldPrevious;
                else if (*missing == "nothing")
                    change.missingFrames = document::AssetMissingFrames::Nothing;
                else
                    return argumentFailure(op, "missingFrames", "expects hold or nothing");
            }
            if (legacy || inputId) {
                using document::AssetColorSpace;
                const auto choice = token<AssetColorSpace, 4>(legacy.value_or("auto"),
                                                              {{{"auto", AssetColorSpace::Auto},
                                                                {"srgb", AssetColorSpace::Srgb},
                                                                {"linear", AssetColorSpace::Linear},
                                                                {"raw", AssetColorSpace::Raw}}});
                if (!choice)
                    return argumentFailure(op, "legacy", "expects auto, srgb, linear or raw");
                change.inputColor = std::pair{*choice, inputId.value_or(std::string{})};
            }
            return made(std::make_unique<commands::SetAssetInterpretation>(std::move(assets),
                                                                           std::move(change)));
        });
}

} // namespace bloom::scripting::detail::parity

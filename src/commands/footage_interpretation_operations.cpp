#include <bloom/commands/footage_interpretation_operations.hpp>

#include <bloom/document/document.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <limits>

namespace bloom::commands {
namespace {
using document::AssetKind;

[[nodiscard]] bool timed(const AssetKind kind) noexcept {
    return kind == AssetKind::Sequence || kind == AssetKind::Video;
}

// a * b, or nullopt on int64 overflow (portable: MSVC has no 128-bit integer).
[[nodiscard]] std::optional<std::int64_t> multiplied(const std::int64_t a, const std::int64_t b) {
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (a == 0 || b == 0)
        return std::int64_t{0};
    if (a < 0 || b < 0 || a > limit / b)
        return std::nullopt;
    return a * b;
}

// The out point a layer made from footage of `length` ends at (AddImageLayer's rule): the first
// composition frame at or after the footage end on the layer's own clock, or zero (the whole
// composition) when that is not before the composition end.
[[nodiscard]] std::optional<core::RationalTime>
fittedOutPoint(const document::Composition& composition, const document::LayerTiming& timing,
               const core::RationalTime length) {
    const auto rate = composition.format().frameRate();
    if (timing.startTime < core::RationalTime{})
        return std::nullopt;
    // end = startTime + length / speed = (s.n * l.d * v.n + l.n * s.d * v.d) / (s.d * l.d * v.n)
    const auto a = multiplied(timing.startTime.numerator(), length.denominator());
    const auto first = a ? multiplied(*a, timing.speed.numerator()) : std::nullopt;
    const auto b = multiplied(length.numerator(), timing.startTime.denominator());
    const auto second = b ? multiplied(*b, timing.speed.denominator()) : std::nullopt;
    const auto c = multiplied(timing.startTime.denominator(), length.denominator());
    const auto denominator = c ? multiplied(*c, timing.speed.numerator()) : std::nullopt;
    if (!first || !second || !denominator ||
        *first > std::numeric_limits<std::int64_t>::max() - *second)
        return std::nullopt;
    const auto end = core::RationalTime::create(*first + *second, *denominator);
    if (!end)
        return std::nullopt;
    // Round up to the composition frame grid, as AddImageLayer does.
    const auto scaledNumerator = multiplied(end->numerator(), rate.numerator());
    const auto scaledDenominator = multiplied(end->denominator(), rate.denominator());
    if (!scaledNumerator || !scaledDenominator ||
        *scaledNumerator > std::numeric_limits<std::int64_t>::max() - *scaledDenominator)
        return std::nullopt;
    const auto frames = (*scaledNumerator + *scaledDenominator - 1) / *scaledDenominator;
    const auto frameEnd = multiplied(frames, rate.denominator());
    const auto snapped =
        frameEnd ? core::RationalTime::create(*frameEnd, rate.numerator()) : std::nullopt;
    if (!snapped || *snapped <= core::RationalTime{})
        return std::nullopt;
    return *snapped < composition.duration() ? *snapped : core::RationalTime{};
}

// Layers whose out point is where their footage ended follow its new length.
void refitLayers(document::Project& project, const document::AssetRecord& before,
                 const document::AssetRecord& after) {
    std::vector<document::CompositionId> compositions;
    for (const auto& composition : project.compositions())
        compositions.push_back(composition.id());
    for (const auto id : compositions) {
        auto* composition = project.findComposition(id);
        const auto rate = composition->format().frameRate();
        const auto oldLength = document::footageDuration(before, rate);
        const auto newLength = document::footageDuration(after, rate);
        if (!oldLength || !newLength || *oldLength == *newLength)
            continue;
        std::vector<document::LayerId> layers;
        for (const auto& layer : composition->graph().layerOutputs()) {
            const auto source = document::layerSource(*composition, layer);
            const auto* asset = source ? std::get_if<document::AssetId>(&*source) : nullptr;
            if (asset != nullptr && *asset == after.id &&
                std::holds_alternative<document::SourceLayerExtent>(layer.timing.contentExtent))
                layers.push_back(layer.layerId);
        }
        for (const auto layerId : layers) {
            auto* layer = composition->graph().findLayer(layerId);
            const auto fittedBefore = fittedOutPoint(*composition, layer->timing, *oldLength);
            const auto fittedAfter = fittedOutPoint(*composition, layer->timing, *newLength);
            if (fittedBefore && fittedAfter && layer->outPoint == *fittedBefore &&
                (*fittedAfter == core::RationalTime{} || *fittedAfter > layer->inPoint))
                layer->outPoint = *fittedAfter;
        }
    }
}
} // namespace

bool isInterpretableFootage(const AssetKind kind) noexcept {
    return kind == AssetKind::Image || timed(kind);
}

AssetInterpretationChange
AssetInterpretationChange::everything(const document::AssetInterpretation& value) {
    AssetInterpretationChange change;
    change.frameRate = value.frameRate;
    change.startTimecode = value.startTimecode;
    change.alphaMode = value.alphaMode;
    change.invertAlpha = value.invertAlpha;
    change.matteColor = value.matteColor;
    change.fieldOrder = value.fieldOrder;
    change.deinterlace = value.deinterlace;
    change.pixelAspect = value.pixelAspect;
    change.loopCount = value.loopCount;
    change.missingFrames = value.missingFrames;
    change.stillDuration = value.stillDuration;
    change.inputColor = std::pair{value.colorSpace, value.inputColorSpaceId};
    return change;
}

bool AssetInterpretationChange::empty() const noexcept {
    return *this == AssetInterpretationChange{};
}

document::AssetInterpretation
AssetInterpretationChange::appliedTo(const AssetKind kind,
                                     document::AssetInterpretation value) const {
    if (!isInterpretableFootage(kind))
        return value;
    if (timed(kind)) {
        if (frameRate)
            value.frameRate = *frameRate;
        if (startTimecode)
            value.startTimecode = *startTimecode;
        if (loopCount)
            value.loopCount = *loopCount;
        if (fieldOrder)
            value.fieldOrder = *fieldOrder;
        if (deinterlace)
            value.deinterlace = *deinterlace;
    }
    if (kind == AssetKind::Sequence && missingFrames)
        value.missingFrames = *missingFrames;
    if (kind == AssetKind::Image && stillDuration)
        value.stillDuration = *stillDuration;
    if (alphaMode)
        value.alphaMode = *alphaMode;
    if (invertAlpha)
        value.invertAlpha = *invertAlpha;
    if (matteColor)
        value.matteColor = *matteColor;
    if (pixelAspect)
        value.pixelAspect = *pixelAspect;
    if (inputColor) {
        value.colorSpace = inputColor->first;
        value.inputColorSpaceId = inputColor->second;
    }
    return value;
}

OperationResult SetAssetInterpretation::apply(document::Draft& draft) const {
    auto& project = draft.project();
    if (assets_.empty() || change_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Choose footage and an interpretation to apply");
    if (change_.inputColor && (change_.inputColor->first > document::AssetColorSpace::Raw ||
                               (change_.inputColor->first != document::AssetColorSpace::Auto &&
                                !change_.inputColor->second.empty())))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Choose one input color interpretation");
    {
        // Out-of-range values are refused whatever the targets' kinds.
        for (const auto kind : {AssetKind::Sequence, AssetKind::Image}) {
            const auto probe = change_.appliedTo(kind, document::AssetInterpretation{});
            if (const auto issue = document::footageInterpretationIssue(probe); !issue.empty())
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 std::string(issue));
        }
    }
    std::vector<document::AssetRecord> updated;
    bool anyFootage = false;
    for (const auto id : assets_) {
        const auto* asset = project.findAsset(id);
        if (asset == nullptr)
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "Asset does not exist");
        if (std::ranges::any_of(updated, [id](const auto& record) { return record.id == id; }))
            continue;
        if (!isInterpretableFootage(asset->kind))
            continue;
        anyFootage = true;
        auto record = *asset;
        record.interpretation = change_.appliedTo(asset->kind, asset->interpretation);
        if (const auto result = record.validate(); !result.ok())
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             result.issues().front().message);
        if (record.interpretation != asset->interpretation)
            updated.push_back(std::move(record));
    }
    if (!anyFootage)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Interpret Footage applies to images, sequences and videos");
    if (updated.empty())
        return OperationResult::noChange();
    for (auto& record : updated) {
        auto* asset = project.findAsset(record.id);
        const auto before = *asset;
        asset->interpretation = std::move(record.interpretation);
        refitLayers(project, before, *asset);
    }
    return OperationResult::applied();
}

} // namespace bloom::commands

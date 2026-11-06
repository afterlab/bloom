#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/field_parameters.hpp>
#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>

#include <bloom/document/layer_masks.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {
namespace {

using Kind = ParameterPresentationKind;

constexpr double kLowest = std::numeric_limits<double>::lowest();
constexpr double kHighest = std::numeric_limits<double>::max();
// The smallest positive double: the hard minimum of a schema that admits any value above zero.
constexpr double kAboveZero = std::numeric_limits<double>::denorm_min();
constexpr std::string_view kPixels = "px";
constexpr std::string_view kPercent = "%";
constexpr std::string_view kDegrees = "\xc2\xb0";
constexpr std::string_view kSeconds = "s";
constexpr std::string_view kFrames = "f";
// #501: stops of exposure (or of a power), and a multiplier.
constexpr std::string_view kStops = "st";
constexpr std::string_view kTimes = "\xc3\x97";

struct Entry {
    std::string_view schemaKey;
    ParameterPresentation presentation;
};

// A continuous number. Ranges are in stored units; step and decimals in displayed ones.
ParameterPresentation number(const double minimum, const double maximum,
                                       const double softMinimum, const double softMaximum,
                                       const double step, const int decimals,
                                       const std::string_view unit = {},
                                       const double displayScale = 1.0) {
    return {Kind::Number, minimum,  maximum,          softMinimum, softMaximum,
            step,         decimals, std::string(unit), displayScale, true};
}

ParameterPresentation whole(const double minimum, const double maximum,
                                      const double softMinimum, const double softMaximum,
                                      const std::string_view unit = {}) {
    return {Kind::Integer, minimum, maximum, softMinimum, softMaximum, 1.0, 0, std::string(unit),
            1.0,           true};
}

ParameterPresentation choice() {
    ParameterPresentation presentation;
    presentation.kind = Kind::Choice;
    presentation.decimals = 0;
    presentation.named = true;
    return presentation;
}

// A 0..1 factor shown as one: a blend, a mix, a matte multiplier.
ParameterPresentation factor() { return number(0.0, 1.0, 0.0, 1.0, 0.01, 3); }
// Pixels with no upper bound beyond the schema's own.
ParameterPresentation pixels(const double minimum, const double maximum, const double softMaximum) {
    return number(minimum, maximum, minimum < 0.0 ? -softMaximum : minimum, softMaximum, 1.0, 2,
                  kPixels);
}
// A colour channel: straight RGBA, any finite value (HDR and signed colour stay authorable), with
// the 0..1 display range as its comfortable span.
ParameterPresentation channel() { return number(kLowest, kHighest, 0.0, 1.0, 0.01, 3); }

// #340: the Levels values (#494: and the Grade values), generated per channel so the schema list
// cannot drift from the definitions (#493 Curves curves are point lists drawn by the curve graph,
// not numbers). Levels values are unbounded in practice (a Raw data pass may span any range) with
// the comfortable 0..1 span; gamma is its own bounded factor.
const std::vector<Entry>& colorGradeTable() {
    // The schema keys are owned by this static list for the life of the process.
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> generated;
        for (std::size_t channel = 0; channel < kGradeChannelCount; ++channel) {
            for (std::size_t field = 0; field < kLevelsFieldCount; ++field)
                generated.push_back(levelsSchemaKey(channel, field));
        }
        return generated;
    }();
    // #494 Grade: the wheels in kGradeWheels order, each master, red, green, blue.
    static const std::vector<std::string> gradeWheelKeys = [] {
        std::vector<std::string> generated;
        for (const auto wheel : kGradeWheels)
            for (std::size_t component = 0; component < kGradeWheelComponentCount; ++component)
                generated.push_back(gradeWheelSchemaKey(wheel, component));
        return generated;
    }();
    static const std::vector<std::string> gradePrimaryKeys = [] {
        std::vector<std::string> generated;
        for (const auto primary : kGradePrimaries)
            generated.push_back(gradePrimarySchemaKey(primary));
        return generated;
    }();
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> generated;
        for (const auto& key : keys)
            generated.push_back(
                {key, isLevelsGammaSchemaKey(key)
                          ? number(kMinimumLevelsGamma, kMaximumLevelsGamma, 0.1, 4.0, 0.01, 2)
                          : number(-kMaximumGradeValue, kMaximumGradeValue, 0.0, 1.0, 0.01, 3)});
        generated.push_back(
            {kHueSaturationHueSchemaKey,
             number(-kMaximumGradeValue, kMaximumGradeValue, -180.0, 180.0, 1.0, 1, kDegrees)});
        generated.push_back({kHueSaturationSaturationSchemaKey,
                             number(0.0, kMaximumGradeSaturation, 0.0, 2.0, 0.01, 2, kTimes)});
        // #494 Grade: the hard range is the schema domain; the soft span is the comfortable one.
        for (std::size_t index = 0; index < gradeWheelKeys.size(); ++index) {
            const auto wheel = kGradeWheels[index / kGradeWheelComponentCount];
            const auto domain = gradeWheelDomain(wheel);
            // #501: a gain is a multiplier, a gamma is in stops of its power, lift and offset add
            // signal units.
            const auto presentation =
                wheel == GradeWheel::Gain
                    ? number(domain.minimum, domain.maximum, 0.0, 2.0, 0.01, 2, kTimes)
                : wheel == GradeWheel::Gamma
                    ? number(domain.minimum, domain.maximum, -1.0, 1.0, 0.01, 2, kStops)
                    : number(domain.minimum, domain.maximum, -0.5, 0.5, 0.001, 3);
            generated.push_back({gradeWheelKeys[index], presentation});
        }
        for (std::size_t index = 0; index < gradePrimaryKeys.size(); ++index) {
            const auto primary = kGradePrimaries[index];
            const auto domain = gradePrimaryDomain(primary);
            ParameterPresentation presentation;
            switch (primary) {
            case GradePrimary::Temperature:
            case GradePrimary::Tint:
                presentation = number(domain.minimum, domain.maximum, -100.0, 100.0, 1.0, 1);
                break;
            case GradePrimary::Contrast:
            case GradePrimary::Saturation:
                presentation = number(domain.minimum, domain.maximum, 0.0, 2.0, 0.01, 2, kTimes);
                break;
            case GradePrimary::Pivot:
                presentation = number(domain.minimum, domain.maximum, 0.01, 1.0, 0.001, 3);
                break;
            case GradePrimary::Exposure:
                presentation = number(domain.minimum, domain.maximum, -4.0, 4.0, 0.1, 2, kStops);
                break;
            }
            generated.push_back({gradePrimaryKeys[index], presentation});
        }
        return generated;
    }();
    return entries;
}

const std::vector<Entry>& table() {
    static const std::vector<Entry> entries{
        // Layer Output transform and compositing.
        {kPositionParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kAnchorParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kScaleParameterSchemaKey, number(kLowest, kHighest, 0.0, 2.0, 1.0, 2, kPercent, 100.0)},
        {kRotationParameterSchemaKey, number(kLowest, kHighest, -360.0, 360.0, 1.0, 2, kDegrees)},
        {kOpacityParameterSchemaKey, number(0.0, 1.0, 0.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kTimeRemapParameterSchemaKey, number(kLowest, kHighest, 0.0, 10.0, 0.01, 3, kSeconds)},
        {kBlendModeParameterSchemaKey, choice()},
        // #480: the finite value a Shuffle or Pass writes for a NaN or infinite sample (a far
        // depth, for one), within the Float32 range a gathered sample holds.
        {"bloom.channel-shuffle.non-finite",
         number(-3.4028234663852886e38, 3.4028234663852886e38, 0.0, 1000.0, 1.0, 3)},
        {"bloom.channel-pass.non-finite",
         number(-3.4028234663852886e38, 3.4028234663852886e38, 0.0, 1000.0, 1.0, 3)},
        // Sources.
        {kSolidColorParameterSchemaKey, channel()},
        {kSolidWidthParameterSchemaKey, pixels(1.0, kHighest, 4096.0)},
        {kSolidHeightParameterSchemaKey, pixels(1.0, kHighest, 4096.0)},
        {kCheckerCellWidthParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 512.0, kPixels)},
        {kCheckerCellHeightParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 512.0, kPixels)},
        {kCheckerWidthParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 4096.0, kPixels)},
        {kCheckerHeightParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 4096.0, kPixels)},
        {kGradientStartParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kGradientEndParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kGradientRepeatParameterSchemaKey, choice()},
        {"bloom.shape.size", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.shape.line-start", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.shape.line-end", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.shape.corner-radius", pixels(0.0, kHighest, 512.0)},
        {"bloom.shape.inner-ratio", factor()},
        {"bloom.shape.stroke-width", pixels(0.0, kHighest, 64.0)},
        {"bloom.shape.points", whole(3.0, 64.0, 3.0, 64.0)},
        {"bloom.shape.fill-color", channel()},
        {"bloom.shape.stroke-color", channel()},
        {"bloom.shape.kind", choice()},
        {"bloom.shape.fill-rule", choice()},
        {"bloom.shape.stroke-align", choice()},
        {"bloom.shape.stroke-cap", choice()},
        {"bloom.shape.stroke-join", choice()},
        // Text.
        {kTextSizeParameterSchemaKey,
         number(kAboveZero, kMaximumTextSizePixels, 1.0, 400.0, 1.0, 1, kPixels)},
        {kTextLineHeightParameterSchemaKey,
         number(kAboveZero, kHighest, 0.5, 3.0, 1.0, 1, kPercent, 100.0)},
        {kTextLetterSpacingParameterSchemaKey, pixels(kLowest, kHighest, 100.0)},
        {kTextBoxParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kTextColorParameterSchemaKey, channel()},
        {kTextAlignmentParameterSchemaKey, choice()},
        {kTextVerticalAlignmentParameterSchemaKey, choice()},
        {kTextAnchorModeParameterSchemaKey, choice()},
        {kTextOverflowParameterSchemaKey, choice()},
        // Media sources.
        {"bloom.image.start-frame", whole(-1000000000.0, 1000000000.0, 0.0, 1000.0, kFrames)},
        {"bloom.video.start-frame", whole(-1000000000.0, 1000000000.0, 0.0, 1000.0, kFrames)},
        {"bloom.audio.start-frame", whole(-1000000000.0, 1000000000.0, 0.0, 1000.0, kFrames)},
        {"bloom.image.loop-mode", choice()},
        {"bloom.video.loop-mode", choice()},
        {"bloom.image.color-space", choice()},
        {"bloom.video.color-space", choice()},
        {kVideoAudioStreamParameterSchemaKey, whole(0.0, 255.0, 0.0, 16.0)},
        {kAudioLevelParameterSchemaKey, number(0.0, 2.0, 0.0, 2.0, 1.0, 1, kPercent, 100.0)},
        {"bloom.composition-source.composition", choice()},
        {"bloom.composition-source.loop-mode", choice()},
        {"bloom.composition-source.time-offset",
         number(kLowest, kHighest, -10.0, 10.0, 0.01, 3, kSeconds)},
        {"bloom.composition-source.time-scale", number(kLowest, kHighest, 0.0, 4.0, 0.01, 3)},
        {"bloom.data.block", choice()},
        // Image effects.
        {"bloom.blur.radius", pixels(0.0, kMaximumBlurRadiusPixels, 64.0)},
        {"bloom.blur.edge", choice()},
        {"bloom.luma-key.low",
         number(-kMaximumLumaKeyThreshold, kMaximumLumaKeyThreshold, 0.0, 1.0, 0.01, 3)},
        {"bloom.luma-key.high",
         number(-kMaximumLumaKeyThreshold, kMaximumLumaKeyThreshold, 0.0, 1.0, 0.01, 3)},
        // #339 Drop Shadow, Glow and Light Wrap.
        {kDropShadowColorSchemaKey, channel()},
        {kDropShadowOpacitySchemaKey, number(0.0, 1.0, 0.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kDropShadowDirectionSchemaKey,
         number(-kMaximumDropShadowDirection, kMaximumDropShadowDirection, 0.0, 360.0, 1.0, 1,
                kDegrees)},
        {kDropShadowDistanceSchemaKey, pixels(0.0, kMaximumDropShadowDistance, 100.0)},
        {kDropShadowSoftnessSchemaKey, pixels(0.0, kMaximumBlurRadiusPixels, 64.0)},
        {kGlowThresholdSchemaKey,
         number(-kMaximumGlowThreshold, kMaximumGlowThreshold, 0.0, 4.0, 0.01, 3)},
        {kGlowKneeSchemaKey, number(0.0, kMaximumGlowThreshold, 0.0, 2.0, 0.01, 3)},
        {kGlowIntensitySchemaKey, number(0.0, kMaximumGlowIntensity, 0.0, 4.0, 0.01, 3)},
        {kGlowSizeSchemaKey, pixels(0.0, kMaximumBlurRadiusPixels, 256.0)},
        {kGlowFalloffSchemaKey, factor()},
        {kGlowTintSchemaKey, channel()},
        {kGlowOctavesSchemaKey,
         whole(static_cast<double>(kMinimumGlowOctaves), static_cast<double>(kMaximumGlowOctaves),
               static_cast<double>(kMinimumGlowOctaves), static_cast<double>(kMaximumGlowOctaves))},
        // #506 Glow modes and shared controls.
        {kGlowModeSchemaKey, choice()},
        {kGlowExponentSchemaKey, number(0.0, kMaximumGlowExponent, 1.0, 4.0, 0.01, 2)},
        {kGlowRadiusRedSchemaKey, factor()},
        {kGlowRadiusGreenSchemaKey, factor()},
        {kGlowRadiusBlueSchemaKey, factor()},
        {kGlowPointsSchemaKey,
         whole(static_cast<double>(kMinimumGlowPoints), static_cast<double>(kMaximumGlowPoints),
               static_cast<double>(kMinimumGlowPoints), 8.0)},
        {kGlowAngleSchemaKey,
         number(-kMaximumDropShadowDirection, kMaximumDropShadowDirection, 0.0, 360.0, 1.0, 1,
                kDegrees)},
        {kGlowLengthSchemaKey, pixels(0.0, kMaximumGlowStreakLength, 512.0)},
        {kGlowChromaticSchemaKey, factor()},
        {kGlowDirtAmountSchemaKey, number(0.0, kMaximumGlowIntensity, 0.0, 4.0, 0.01, 3)},
        {kGlowRollOffSchemaKey, number(0.0, kMaximumGlowRollOff, 0.0, 2.0, 0.01, 3)},
        {kLightWrapRadiusSchemaKey, pixels(0.0, kMaximumBlurRadiusPixels, 64.0)},
        {kLightWrapAmountSchemaKey, factor()},
        {"bloom.exposure.stops", number(kLowest, kHighest, -4.0, 4.0, 0.1, 2, kStops)},
        {"bloom.offset.amount", number(kLowest, kHighest, -1.0, 1.0, 0.01, 3)},
        {"bloom.multiply.factor", number(kLowest, kHighest, 0.0, 2.0, 0.01, 2, kTimes)},
        {"bloom.image-mix.factor", factor()},
        {"bloom.multiply-alpha.factor", factor()},
        {"bloom.multiply-alpha.channel", choice()},
        // #503 Alpha Association.
        {kAlphaAssociationModeSchemaKey, choice()},
        {kAlphaAssociationBackgroundSchemaKey, channel()},
        // #504 Colorize: the weights read as percentages, like the amount's blend.
        {kColorizeModeSchemaKey, choice()},
        {kColorizeAmountSchemaKey, number(0.0, 1.0, 0.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kColorizeColorSchemaKey, channel()},
        {kColorizeShadowsSchemaKey, channel()},
        {kColorizeMidtonesSchemaKey, channel()},
        {kColorizeHighlightsSchemaKey, channel()},
        {kColorizeMidpointSchemaKey, number(0.01, 0.99, 0.01, 0.99, 0.001, 3)},
        {kColorizeSourceSchemaKey, choice()},
        {kColorizeCycleSchemaKey, choice()},
        {kColorizeRepetitionsSchemaKey, number(0.0, 100.0, 0.0, 8.0, 0.01, 2, kTimes)},
        {kColorizePhaseSchemaKey, number(-1000.0, 1000.0, 0.0, 1.0, 0.01, 3)},
        {kColorizeRedsSchemaKey, number(-2.0, 3.0, -2.0, 3.0, 1.0, 1, kPercent, 100.0)},
        {kColorizeYellowsSchemaKey, number(-2.0, 3.0, -2.0, 3.0, 1.0, 1, kPercent, 100.0)},
        {kColorizeGreensSchemaKey, number(-2.0, 3.0, -2.0, 3.0, 1.0, 1, kPercent, 100.0)},
        {kColorizeCyansSchemaKey, number(-2.0, 3.0, -2.0, 3.0, 1.0, 1, kPercent, 100.0)},
        {kColorizeBluesSchemaKey, number(-2.0, 3.0, -2.0, 3.0, 1.0, 1, kPercent, 100.0)},
        {kColorizeMagentasSchemaKey, number(-2.0, 3.0, -2.0, 3.0, 1.0, 1, kPercent, 100.0)},
        // #505 Vignette: the shares read as percentages; the exposure in stops; the centre an
        // offset in pixels from the frame's centre.
        {kVignetteShapeSchemaKey, choice()},
        {kVignetteModeSchemaKey, choice()},
        {kVignetteAmountSchemaKey, number(-1.0, 1.0, -1.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kVignetteExposureSchemaKey, number(-10.0, 10.0, -4.0, 4.0, 0.1, 2, kStops)},
        {kVignetteMidpointSchemaKey, number(0.0, 1.0, 0.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kVignetteRoundnessSchemaKey, number(-1.0, 1.0, -1.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kVignetteFeatherSchemaKey, number(0.0, 1.0, 0.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kVignetteHighlightProtectionSchemaKey,
         number(0.0, 1.0, 0.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kVignetteCenterSchemaKey,
         pixels(-kMaximumVignetteCenter, kMaximumVignetteCenter, 4096.0)},
        {"bloom.set-alpha.channel", choice()},
        // #383 the matte workflow's selectors.
        {kEffectMaskChannelSchemaKey, choice()},
        {kImageToMaskChannelSchemaKey, choice()},
        {kMaskCombineModeSchemaKey, choice()},
        // #479 Combine Channels.
        {"bloom.combine-channels.source", choice()},
        {"bloom.combine-channels.alpha-mode", choice()},
        {"bloom.crop.left", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.crop.top", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.crop.right", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.crop.bottom", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.left", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.top", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.right", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.bottom", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.transform-2d.translate", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.transform-2d.pivot", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.transform-2d.scale", number(kLowest, kHighest, 0.0, 2.0, 1.0, 2, kPercent, 100.0)},
        {"bloom.transform-2d.rotation", number(kLowest, kHighest, -360.0, 360.0, 1.0, 2, kDegrees)},
        {"bloom.rotate-90.quarter-turns",
         whole(static_cast<double>(std::numeric_limits<std::int64_t>::min()),
               static_cast<double>(std::numeric_limits<std::int64_t>::max()), -3.0, 3.0)},
        // Masks.
        {kLayerMaskFeatherSchemaKey, pixels(0.0, kMaximumLayerMaskFeatherPixels, 100.0)},
        {kLayerMaskExpansionSchemaKey,
         pixels(-kMaximumLayerMaskExpansionPixels, kMaximumLayerMaskExpansionPixels, 100.0)},
    };
    return entries;
}

// Schemas that are selectors in every editor (the value library's operations, the colour
// pipeline's references, the channel nodes' modes). Named so the registry test can tell a choice
// from a forgotten number.
constexpr std::array kChoiceSchemas{
    std::string_view("bloom.boolean.operation"),
    std::string_view("bloom.channel-shuffle.mode"),
    std::string_view("bloom.channel-pass.mode"),
    std::string_view("bloom.image-to-channel-set.encoding"),
    std::string_view("bloom.image-preview.display"),
    std::string_view("bloom.compare.operation"),
    std::string_view("bloom.convert.rounding-mode"),
    std::string_view("bloom.convert.radix"),
    std::string_view("bloom.integer.operation"),
    std::string_view("bloom.map-range.interpolation"),
    std::string_view("bloom.math.operation"),
    std::string_view("bloom.ocio-file.direction"),
    std::string_view("bloom.ocio-file.interpolation"),
    std::string_view("bloom.ocio-file.lut"),
    std::string_view("bloom.resize-fit.filter"),
    std::string_view("bloom.resize-fit.policy"),
    std::string_view("bloom.string.case"),
    std::string_view("bloom.transform-2d.extent"),
    std::string_view("bloom.transform-2d.filter"),
    std::string_view("bloom.string.pad-side"),
    std::string_view("bloom.vector-math.operation"),
    std::string_view("bloom.vector-reduce.operation"),
};

ParameterPresentation fallback(const ParameterValueKind kind) {
    switch (kind) {
    case ParameterValueKind::Float64:
    case ParameterValueKind::Vec2d:
    case ParameterValueKind::Vec3d:
        return {Kind::Number, kLowest, kHighest, -100.0, 100.0, 0.1, 3, {}, 1.0, false};
    case ParameterValueKind::Color4d: {
        auto presentation = channel();
        presentation.named = false;
        return presentation;
    }
    case ParameterValueKind::Integer:
        return {Kind::Integer,
                static_cast<double>(std::numeric_limits<std::int64_t>::min()),
                static_cast<double>(std::numeric_limits<std::int64_t>::max()),
                -100.0,
                100.0,
                1.0,
                0,
                {},
                1.0,
                false};
    case ParameterValueKind::Boolean:
    case ParameterValueKind::String:
    case ParameterValueKind::Path:
    case ParameterValueKind::Gradient:
        break;
    }
    ParameterPresentation other;
    other.kind = Kind::Other;
    return other;
}

} // namespace

namespace {

// #525 Fields: one presentation per parameter role, from the Fields table's own domain.
std::optional<ParameterPresentation> fieldPresentation(const std::string_view schemaKey) {
    const auto* spec = fieldParameterSpec(schemaKey);
    if (spec == nullptr)
        return std::nullopt;
    const auto role = spec->role;
    const auto low = spec->minimum;
    const auto high = spec->maximum;
    switch (spec->kind) {
    case FieldParameterKind::Choice:
        return choice();
    case FieldParameterKind::Integer:
        return whole(low, high, low, std::min(high, 64.0));
    case FieldParameterKind::Seed:
        return whole(-9.0e18, 9.0e18, 0.0, 100.0);
    case FieldParameterKind::Boolean:
    case FieldParameterKind::Curve:
    case FieldParameterKind::Gradient: {
        ParameterPresentation other;
        other.kind = Kind::Other;
        other.named = true;
        return other;
    }
    case FieldParameterKind::Scalar:
    case FieldParameterKind::Vector2:
        break;
    }
    const auto is = [role](const std::initializer_list<std::string_view> names) {
        return std::ranges::find(names, role) != names.end();
    };
    if (is({"rotation", "octaveRotation", "angle"}))
        return number(low, high, -360.0, 360.0, 1.0, 1, kDegrees);
    // Shares and factors, shown as stored (1 is the whole), the same in a card and in Properties.
    if (is({"gain", "jitter", "width", "innerRatio", "bias", "contrast"}))
        return number(low, high, low, std::min(high, is({"contrast"}) ? 4.0 : 1.0), 0.01, 3);
    if (role == "loopLength")
        return number(low, high, 0.0, 30.0, 0.1, 2, kSeconds);
    if (role == "speed")
        return number(low, high, -4.0, 4.0, 0.01, 2, "/s");
    if (is({"octaves", "lacunarity", "ridgeOffset", "ridgeGain"}))
        return number(low, high, low, std::min(high, 10.0), 0.05, 2);
    if (is({"evolution", "phase", "cycle", "brightness", "valueA", "valueB", "valueT", "valueX",
            "valueY", "valueZ", "valueW", "strength", "inLow", "inHigh", "outLow", "outHigh",
            "rampLow", "rampHigh"}))
        return number(low, high, std::max(low, -4.0), std::min(high, 4.0), 0.01, 3);
    if (role == "scale")
        return number(low, high, -4.0, 4.0, 0.01, 3);
    if (role == "arrowScale")
        return number(low, high, 0.0, 100.0, 0.5, 1, kPixels);
    return pixels(low, high, std::min(high, low < 0.0 ? 2048.0 : 1024.0));
}

// #483 Z Defocus and Depth Fog: one presentation per parameter, from the depth table itself.
std::optional<ParameterPresentation> depthPresentation(const std::string_view schemaKey) {
    const auto* spec = depthParameterSpec(schemaKey);
    if (spec == nullptr)
        return std::nullopt;
    switch (spec->kind) {
    case DepthParameterKind::Choice:
        return choice();
    case DepthParameterKind::Integer:
        return whole(spec->minimum, spec->maximum, spec->softMinimum, spec->softMaximum,
                     spec->unit);
    case DepthParameterKind::Boolean:
    case DepthParameterKind::Curve:
    case DepthParameterKind::Text: {
        ParameterPresentation other;
        other.kind = Kind::Other;
        other.named = true;
        return other;
    }
    case DepthParameterKind::Color:
        return number(spec->minimum, spec->maximum, 0.0, 1.0, 0.01, 3);
    case DepthParameterKind::Scalar:
    case DepthParameterKind::Vector2:
        break;
    }
    return number(spec->minimum, spec->maximum, spec->softMinimum, spec->softMaximum, spec->step,
                  spec->decimals, spec->unit);
}

// #507 Lens Flare: one presentation per parameter, from its table.
std::optional<ParameterPresentation> lensFlarePresentation(const std::string_view schemaKey) {
    const auto* spec = lensFlareParameterSpec(schemaKey);
    if (spec == nullptr)
        return std::nullopt;
    switch (spec->kind) {
    case LensFlareParameterKind::Choice:
        return choice();
    case LensFlareParameterKind::Integer:
        return whole(spec->minimum, spec->maximum, spec->softMinimum, spec->softMaximum,
                     spec->unit);
    case LensFlareParameterKind::Color:
        return number(spec->minimum, spec->maximum, 0.0, 1.0, 0.01, 3);
    case LensFlareParameterKind::Scalar:
    case LensFlareParameterKind::Vector2:
        break;
    }
    return number(spec->minimum, spec->maximum, spec->softMinimum, spec->softMaximum, spec->step,
                  spec->decimals, spec->unit);
}

// #486 Film Grain: one presentation per parameter, from its table.
std::optional<ParameterPresentation> filmGrainPresentation(const std::string_view schemaKey) {
    const auto* spec = filmGrainParameterSpec(schemaKey);
    if (spec == nullptr)
        return std::nullopt;
    switch (spec->kind) {
    case FilmGrainParameterKind::Choice:
        return choice();
    case FilmGrainParameterKind::Integer:
        return whole(spec->minimum, spec->maximum, spec->minimum, 9999.0, spec->unit);
    case FilmGrainParameterKind::Scalar:
        break;
    }
    return number(spec->minimum, spec->maximum, spec->softMinimum, spec->softMaximum, spec->step,
                  spec->decimals, spec->unit, spec->displayScale);
}

} // namespace

ParameterPresentation parameterPresentation(const std::string_view schemaKey,
                                            const ParameterValueKind kind,
                                            const std::string_view role) {
    if (auto field = fieldPresentation(schemaKey))
        return *field;
    if (auto depth = depthPresentation(schemaKey))
        return *depth;
    if (auto flare = lensFlarePresentation(schemaKey))
        return *flare;
    if (auto grain = filmGrainPresentation(schemaKey))
        return *grain;
    for (const auto& entry : table())
        if (entry.schemaKey == schemaKey)
            return entry.presentation;
    for (const auto& entry : colorGradeTable())
        if (entry.schemaKey == schemaKey)
            return entry.presentation;
    for (const auto name : kChoiceSchemas)
        if (name == schemaKey)
            return choice();
    if (kind == ParameterValueKind::Boolean || kind == ParameterValueKind::String ||
        kind == ParameterValueKind::Path || kind == ParameterValueKind::Gradient) {
        auto other = fallback(kind);
        other.named = true;
        return other;
    }
    // The value library: literals and operands carry no meaning beyond their kind, except where
    // the role says what the number is for.
    if (schemaKey == kScalarOperandParameterSchemaKey ||
        schemaKey == kScalarValueParameterSchemaKey ||
        schemaKey == kVector2OperandParameterSchemaKey ||
        schemaKey == kVector2ValueParameterSchemaKey ||
        schemaKey == kVector3OperandParameterSchemaKey ||
        schemaKey == kVector3ValueParameterSchemaKey) {
        auto presentation = fallback(kind);
        presentation.named = true;
        if (role == kMixFactorPortName) {
            presentation.softMinimum = 0.0;
            presentation.softMaximum = 1.0;
            presentation.step = 0.01;
        }
        return presentation;
    }
    if (schemaKey == kIntegerOperandParameterSchemaKey ||
        schemaKey == kIntegerValueParameterSchemaKey) {
        auto presentation = fallback(ParameterValueKind::Integer);
        presentation.named = true;
        return presentation;
    }
    if (schemaKey == kColorOperandParameterSchemaKey || schemaKey == kColorValueParameterSchemaKey)
        return channel();
    if (schemaKey == kCompareEpsilonParameterSchemaKey)
        return number(0.0, kHighest, 0.0, 0.01, 0.0001, 6);
    if (schemaKey == kLayerMaskOpacitySchemaKey)
        return factor();
    if (schemaKey.starts_with("bloom.resize-fit.")) {
        // Width, height and the pixel aspect pair: whole pixels in [1, 32768].
        return whole(1.0, 32768.0, 1.0, 4096.0,
                     schemaKey.ends_with("width") || schemaKey.ends_with("height") ? kPixels
                                                                                   : std::string_view{});
    }
    return fallback(kind);
}

ParameterPresentation parameterPresentation(const ParameterDefinition& definition) {
    return parameterPresentation(definition.schemaKey, definition.valueKind, definition.role);
}

ParameterPresentation componentPresentation(ParameterPresentation presentation,
                                            const ParameterValueKind kind,
                                            const std::size_t component) {
    constexpr std::size_t kAlpha = 3;
    if (kind == ParameterValueKind::Color4d && component == kAlpha) {
        presentation.minimum = 0.0;
        presentation.maximum = 1.0;
        presentation.softMinimum = 0.0;
        presentation.softMaximum = 1.0;
    }
    return presentation;
}

} // namespace bloom::document

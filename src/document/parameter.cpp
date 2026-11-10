#include <bloom/document/transform_2d_nodes.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/font_id.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/document/lens_flare_definition.hpp>
#include <bloom/document/film_grain_definition.hpp>
#include <bloom/document/codec_degrade_definition.hpp>
#include <bloom/document/auto_exposure_definition.hpp>
#include <bloom/document/audio_effect_definition.hpp>
#include <bloom/document/lens_distortion_definition.hpp>
#include <bloom/document/rolling_shutter_definition.hpp>
#include <bloom/document/light_rays_nodes.hpp>
#include <bloom/document/rolling_shutter_nodes.hpp>
#include <bloom/document/warp_definition.hpp>
#include <bloom/document/chromatic_aberration_definition.hpp>
#include <bloom/document/test_card_definition.hpp>
#include <bloom/document/four_color_gradient_definition.hpp>
#include <bloom/document/light_rays_definition.hpp>
#include <bloom/document/image_math_nodes.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/denoise_definition.hpp>
#include <bloom/document/sharpen_definition.hpp>
#include <bloom/document/field_nodes.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/shape.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/image_channel_nodes.hpp>
#include <bloom/document/image_preview.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/resample_path_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

[[nodiscard]] bool isGenericallyValidSource(const bloom::document::ParameterSource& source) {
    return std::visit(
        [](const auto& valueSource) {
            using Source = std::decay_t<decltype(valueSource)>;
            if constexpr (std::is_same_v<Source, bloom::document::ConstantValueSource>) {
                if (const auto* path = std::get_if<bloom::document::PathValue>(&valueSource.value))
                    return path->isValid();
                if (const auto* gradient =
                        std::get_if<bloom::document::GradientValue>(&valueSource.value))
                    return gradient->isValid();
                if (const auto* value = std::get_if<double>(&valueSource.value)) {
                    return std::isfinite(*value);
                }
                if (const auto* value = std::get_if<bloom::document::Vec2d>(&valueSource.value)) {
                    return std::isfinite(value->x) && std::isfinite(value->y);
                }
                if (const auto* value = std::get_if<bloom::document::Vec3d>(&valueSource.value)) {
                    return std::isfinite(value->x) && std::isfinite(value->y) &&
                           std::isfinite(value->z);
                }
                if (const auto* value = std::get_if<bloom::core::Color4d>(&valueSource.value)) {
                    return value->isValid();
                }
                if (const auto* value = std::get_if<std::string>(&valueSource.value)) {
                    return bloom::core::isValidUtf8(*value);
                }
                return true;
            } else if constexpr (std::is_same_v<Source, bloom::document::AnimationCurveSource>) {
                if (!valueSource.curveId.isValid()) {
                    return false;
                }
                if (!valueSource.defaultValue.has_value()) {
                    return true;
                }
                const bloom::document::ConstantValueSource fallback{*valueSource.defaultValue};
                return isGenericallyValidSource(bloom::document::ParameterSource{fallback});
            } else {
                // A driver names a node and one of its output ports. Whether that node EXISTS and
                // whether its kind fits belongs to CanonicalGraph::validate(), which can see the
                // graph; a ParameterStore can only check that the reference is well formed.
                return valueSource.sourceNodeId.isValid() &&
                       bloom::document::isValidStructuralText(valueSource.outputPort);
            }
        },
        source);
}

// The value-graph schemas (task S7). Split out of constantMatchesSchema() rather than appended to
// it because the two halves answer different questions: the layer schemas above each bound a value
// to the picture it draws (an opacity is in [0, 1], a text size fits the rasterizer), while these
// bound a value to the TYPE its socket carries. Every numeric one here is
// finite-and-otherwise-free, because a value graph's whole purpose is arithmetic the artist
// controls; the only real domains are the closed selector mappings and a non-negative tolerance.
[[nodiscard]] bool
valueGraphConstantMatchesSchema(const std::string_view schemaKey,
                                const bloom::document::ConstantValueSource& constant) noexcept {
    using namespace bloom::document;
    const auto finiteScalar = [&] {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value);
    };
    const auto integer = [&] { return std::holds_alternative<std::int64_t>(constant.value); };
    if (schemaKey == kScalarValueParameterSchemaKey ||
        schemaKey == kScalarOperandParameterSchemaKey ||
        schemaKey == kStampRotationParameterSchemaKey || schemaKey == kResampleGapSchemaKey ||
        schemaKey == kResampleDensitySchemaKey || schemaKey == kResamplePercentageSchemaKey ||
        schemaKey == kResampleStartOffsetSchemaKey) {
        return finiteScalar();
    }
    if (schemaKey == kIntegerValueParameterSchemaKey ||
        schemaKey == kIntegerOperandParameterSchemaKey || schemaKey == kResampleCountSchemaKey) {
        return integer();
    }
    if (schemaKey == kDataBlockParameterSchemaKey) {
        return integer();
    }
    if (schemaKey == kBooleanValueParameterSchemaKey ||
        schemaKey == kBooleanOperandParameterSchemaKey ||
        schemaKey == kClampResultParameterSchemaKey) {
        return std::holds_alternative<bool>(constant.value);
    }
    if (schemaKey == kVector2ValueParameterSchemaKey ||
        schemaKey == kVector2OperandParameterSchemaKey) {
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y);
    }
    if (schemaKey == kVector3ValueParameterSchemaKey ||
        schemaKey == kVector3OperandParameterSchemaKey) {
        const auto* value = std::get_if<Vec3d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y) &&
               std::isfinite(value->z);
    }
    if (schemaKey == kColorValueParameterSchemaKey ||
        schemaKey == kColorOperandParameterSchemaKey) {
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid();
    }
    if (schemaKey == kStringValueParameterSchemaKey ||
        schemaKey == kStringOperandParameterSchemaKey) {
        return std::holds_alternative<std::string>(constant.value);
    }
    // The selectors. An integer naming no implemented operation is refused rather than folded to
    // the default, exactly as an unknown blend mode is: computing a different operation than the
    // document asked for would be a silent miscomputation.
    if (schemaKey == kScalarOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && scalarOperationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kVectorOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && vectorOperationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kVectorReductionParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && vectorReductionFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kRangeInterpolationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && rangeInterpolationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kCompareOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && compareOperationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kOscillatorWaveParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kOscillatorWaves, *stored).has_value();
    }
    if (schemaKey == kLoopValueModeParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kLoopModes, *stored).has_value();
    }
    if (schemaKey == kRoundingModeParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kRoundingModes, *stored).has_value();
    }
    if (schemaKey == kTrimModeParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kTrimModes, *stored).has_value();
    }
    if (schemaKey == kOffsetJoinParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kOffsetJoins, *stored).has_value();
    }
    if (schemaKey == kPathCombineParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kPathCombines, *stored).has_value();
    }
    if (schemaKey == kStrokeCapParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kStrokeCaps, *stored).has_value();
    }
    // #509: a node id, or 0 for the composition's first readable node.
    if (isPathSourceParameterSchemaKey(schemaKey)) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && *stored >= 0;
    }
    if (schemaKey == kStrokeStampParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kStrokeStamps, *stored).has_value();
    }
    if (schemaKey == kStampAlignParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kStampAligns, *stored).has_value();
    }
    if (schemaKey == kResampleModeParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kResampleModes, *stored).has_value();
    }
    if (schemaKey == kResampleOutputParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kResampleOutputs, *stored).has_value();
    }
    // #344: a path operand holds any valid path value, as a constant or a driver.
    if (schemaKey == kPathOperandParameterSchemaKey) {
        const auto* path = std::get_if<PathValue>(&constant.value);
        return path != nullptr && path->isValid();
    }
    if (schemaKey == kIntegerOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr &&
               selectorFromStoredValue(kIntegerOperations, *stored).has_value();
    }
    if (schemaKey == kBooleanOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr &&
               selectorFromStoredValue(kBooleanOperations, *stored).has_value();
    }
    if (schemaKey == kStringCaseParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kStringCaseModes, *stored).has_value();
    }
    if (schemaKey == kStringPadSideParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kStringPadSides, *stored).has_value();
    }
    if (schemaKey == kNumberRadixParameterSchemaKey) {
        // The radix itself, not an index: anything core::parseInteger() can actually read, so a
        // document may carry base 36 even though the card offers four bases.
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && isValidStoredRadix(*stored);
    }
    if (schemaKey == kCompareEpsilonParameterSchemaKey) {
        // A negative tolerance is not a tolerance, and an infinite one makes every comparison true.
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) && *value >= 0.0;
    }
    // An unregistered schema key carries whatever it carries: this function is the gate for the
    // schemas Bloom OWNS, and an extension's key is not one of them.
    return true;
}

[[nodiscard]] bool
constantMatchesSchema(const std::string_view schemaKey,
                      const bloom::document::ConstantValueSource& constant) noexcept {
    using namespace bloom::document;
    if (schemaKey.starts_with("bloom.shape."))
        return shapeConstantMatchesSchema(schemaKey, constant.value);
    // ADR 0028 mask schemas (layer masks and the Mask node).
    if (schemaKey == kLayerMaskPathSchemaKey) {
        const auto* path = std::get_if<PathValue>(&constant.value);
        return path != nullptr && path->isValid() && path->isSimple();
    }
    // #589: the literal Path node holds any valid path value.
    if (schemaKey == kPathValueParameterSchemaKey) {
        const auto* path = std::get_if<PathValue>(&constant.value);
        return path != nullptr && path->isValid();
    }
    if (schemaKey == kLayerMaskInvertedSchemaKey)
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == kLayerMaskFeatherSchemaKey || schemaKey == kLayerMaskExpansionSchemaKey ||
        schemaKey == kLayerMaskOpacitySchemaKey) {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (schemaKey == kSolidWidthParameterSchemaKey || schemaKey == kSolidHeightParameterSchemaKey) {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (schemaKey == kSolidColorParameterSchemaKey ||
        schemaKey == kGradientStartColorParameterSchemaKey ||
        schemaKey == kGradientEndColorParameterSchemaKey) {
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid();
    }
    if (schemaKey == kCheckerCellWidthParameterSchemaKey ||
        schemaKey == kCheckerCellHeightParameterSchemaKey ||
        schemaKey == kCheckerWidthParameterSchemaKey ||
        schemaKey == kCheckerHeightParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 1 && *value <= 16777216;
    }
    if (schemaKey == kGradientStartParameterSchemaKey ||
        schemaKey == kGradientEndParameterSchemaKey || schemaKey == kPositionParameterSchemaKey ||
        schemaKey == kAnchorParameterSchemaKey || schemaKey == kScaleParameterSchemaKey) {
        // One rule for all three Vec2d transform values: finite, otherwise unbounded. Scale is
        // deliberately NOT confined to positive numbers -- a negative factor mirrors the axis and
        // zero collapses the layer, both of which evaluation renders rather than refuses -- and
        // anchor is deliberately not confined to the layer's own box, so an off-layer pivot stays
        // authorable.
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y);
    }
    if (schemaKey == kTimeRemapParameterSchemaKey) {
        // Seconds of content time; any finite value (the out-of-extent modes govern the rest).
        const auto* seconds = std::get_if<double>(&constant.value);
        return seconds != nullptr && std::isfinite(*seconds);
    }
    if (schemaKey == kRotationParameterSchemaKey) {
        // Degrees, finite and unbounded: an animated rotation must be able to wind past 360 and
        // below zero, so only a non-finite value is rejected.
        const auto* rotation = std::get_if<double>(&constant.value);
        return rotation != nullptr && std::isfinite(*rotation);
    }
    if (schemaKey == kOpacityParameterSchemaKey) {
        const auto* opacity = std::get_if<double>(&constant.value);
        return opacity != nullptr && std::isfinite(*opacity) && *opacity >= 0.0 && *opacity <= 1.0;
    }
    if (schemaKey == kBlendModeParameterSchemaKey) {
        // A blend mode is a stored integer under core::BlendMode's closed mapping. An integer that
        // names no implemented mode is refused rather than folded to Normal: drawing a different
        // mode than the document asked for would be a silent misrender, and a document from a newer
        // build belongs in the "cannot be interpreted" path, not in a guess.
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && bloom::core::blendModeFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kGradientRepeatParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 && *value <= 2;
    }
    if (schemaKey == kAudioLevelParameterSchemaKey) {
        const auto* level = std::get_if<double>(&constant.value);
        return level != nullptr && std::isfinite(*level) &&
               isScalarWithinSchemaDomain(schemaKey, *level);
    }
    if (schemaKey == "bloom.composition-source.composition" ||
        schemaKey == "bloom.composition-source.loop-mode") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 &&
               (schemaKey == "bloom.composition-source.composition" || *value <= 2);
    }
    if (schemaKey == "bloom.node-group.group") {
        // #342: a group definition's composition id; zero names none.
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0;
    }
    if (schemaKey == "bloom.composition-source.time-offset" ||
        schemaKey == "bloom.composition-source.time-scale") {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value);
    }
    if (isCanvasMarginSchemaKey(schemaKey)) {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) &&
               isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (schemaKey == "bloom.blur.radius" || schemaKey == "bloom.blur.radius-y") {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && *value >= 0.0 &&
               *value <= kMaximumBlurRadiusPixels;
    }
    if (schemaKey == "bloom.blur.separate" || schemaKey == "bloom.blur.per-channel")
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == "bloom.blur.red-scale" || schemaKey == "bloom.blur.green-scale" ||
        schemaKey == "bloom.blur.blue-scale" || schemaKey == "bloom.blur.alpha-scale" ||
        schemaKey == "bloom.transform-2d.opacity") {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && *value >= 0.0 && *value <= 1.0;
    }
    if (schemaKey == "bloom.transform-2d.skew") {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && std::abs(*value) <= kMaximumTransformSkew;
    }
    if (schemaKey == "bloom.transform-2d.skew-axis") {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value);
    }
    if (schemaKey == "bloom.blur.edge") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 && *value <= 1;
    }
    if (schemaKey == "bloom.exposure.stops" || schemaKey == "bloom.offset.amount" ||
        schemaKey == "bloom.multiply.factor") {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value);
    }
    if (schemaKey == "bloom.image-mix.factor" || schemaKey == "bloom.multiply-alpha.factor") {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) && *value >= 0.0 && *value <= 1.0;
    }
    // #479: the closed source channel (R, G, B, A, Luma) and alpha mode enumerations.
    if (schemaKey == kCombineChannelsSourceSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value < kImageChannelSourceCount;
    }
    if (schemaKey == kCombineChannelsAlphaModeSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr &&
               (*value == kCombineChannelsPremultiplied || *value == kCombineChannelsStraight);
    }
    if (schemaKey == "bloom.set-alpha.channel" || schemaKey == "bloom.multiply-alpha.channel" ||
        schemaKey == kEffectMaskChannelSchemaKey || schemaKey == kImageToMaskChannelSchemaKey) {
        // The closed matte selector (render::MatteSelection): 0-4 A/R/G/B/Luma, 5-9 inverted.
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value <= 9;
    }
    // #383 Mask Combine: Add, Subtract, Intersect, Difference.
    if (schemaKey == kMaskCombineModeSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value < kMaskCombineModeCount;
    }
    if (schemaKey == "bloom.ocio-cst.from" || schemaKey == "bloom.ocio-cst.to" ||
        schemaKey == "bloom.ocio-file.process-space") {
        const auto* value = std::get_if<std::string>(&constant.value);
        return value && value->size() <= 256 && value->find('\0') == std::string::npos;
    }
    if (schemaKey == "bloom.flip.horizontal" || schemaKey == "bloom.flip.vertical" ||
        schemaKey == "bloom.luma-key.invert" || schemaKey == kLevelsClampSchemaKey ||
        schemaKey == kCurvesClampSchemaKey || schemaKey == kHueSaturationColorizeSchemaKey)
        return std::holds_alternative<bool>(constant.value);
    // #340: every Levels value, the hue and the saturation are finite within their schema domain.
    // #494: so is every Grade wheel value and primary.
    // #493: a Curves curve is a point list within its curve's domain (grade_curves.hpp).
    if (isLevelsValueSchemaKey(schemaKey) || schemaKey == kHueSaturationHueSchemaKey ||
        schemaKey == kHueSaturationSaturationSchemaKey ||
        schemaKey == kHueSaturationVibranceSchemaKey || isGradeValueSchemaKey(schemaKey)) {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) &&
               isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (isGradeCurveSchemaKey(schemaKey)) {
        const auto* value = std::get_if<PathValue>(&constant.value);
        return value != nullptr && isGradeCurveValueValid(schemaKey, *value);
    }
    if (schemaKey == "bloom.luma-key.low" || schemaKey == "bloom.luma-key.high") {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) &&
               isScalarWithinSchemaDomain(schemaKey, *value);
    }
    // #503 Alpha Association: the closed mode, the white switch and a finite straight background
    // colour whose RGB stays within +-1e6 (its alpha is unused).
    if (schemaKey == kAlphaAssociationModeSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= kAlphaAssociationUnmult &&
               *value <= kAlphaAssociationRemoveMatting;
    }
    if (schemaKey == kAlphaAssociationWhiteSchemaKey)
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == kAlphaAssociationBackgroundSchemaKey) {
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid() &&
               std::abs(color->red) <= kMaximumAlphaAssociationBackground &&
               std::abs(color->green) <= kMaximumAlphaAssociationBackground &&
               std::abs(color->blue) <= kMaximumAlphaAssociationBackground;
    }
    // #504 Colorize: finite scalars within their domains, finite straight colours whose RGB stays
    // within +-1e6 (their alpha is unused) and the closed mode, source and cycle.
    if (isColorizeScalarSchemaKey(schemaKey)) {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) &&
               isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (isColorizeColorSchemaKey(schemaKey)) {
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid() &&
               std::abs(color->red) <= kMaximumColorizeColor &&
               std::abs(color->green) <= kMaximumColorizeColor &&
               std::abs(color->blue) <= kMaximumColorizeColor;
    }
    if (const auto domain = colorizeIntegerDomain(schemaKey)) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= domain->minimum && *value <= domain->maximum;
    }
    // #541: Gradient Map's gradient, each stop's RGB within +-1e6 like the colours; the
    // generators' colours and Glow's tint gradient, any finite straight colour like the colour
    // pairs they replace.
    if (isGradientAnimatableSchemaKey(schemaKey)) {
        const auto* gradient = std::get_if<GradientValue>(&constant.value);
        return gradient != nullptr && (isColorizeGradientSchemaKey(schemaKey)
                                           ? isBoundedGradient(*gradient)
                                           : gradient->isValid());
    }
    // #525 Fields: every parameter within its document domain (field_nodes.hpp).
    if (fieldParameterSpec(schemaKey) != nullptr)
        return isFieldParameterValueValid(schemaKey, constant.value);
    // #483 Z Defocus and Depth Fog: every parameter within its document domain.
    if (depthParameterSpec(schemaKey) != nullptr)
        return isDepthParameterValueValid(schemaKey, constant.value);
    // #507 Lens Flare: every parameter within its document domain.
    if (lensFlareParameterSpec(schemaKey) != nullptr)
        return isLensFlareParameterValueValid(schemaKey, constant.value);
    // #486 Film Grain: every parameter within its document domain.
    if (filmGrainParameterSpec(schemaKey) != nullptr)
        return isFilmGrainParameterValueValid(schemaKey, constant.value);
    // #486 Lens Distortion: every parameter and the centre within their document domains.
    if (lensDistortionParameterSpec(schemaKey) != nullptr ||
        schemaKey == kLensDistortionCenterSchemaKey)
        return isLensDistortionParameterValueValid(schemaKey, constant.value);
    // #486 Chromatic Aberration: the amount and the centre within their document domains.
    if (isChromaticAberrationScalarSchemaKey(schemaKey) ||
        schemaKey == kChromaticAberrationCenterSchemaKey)
        return isChromaticAberrationParameterValueValid(schemaKey, constant.value);
    // #587 Test Card: every parameter within its document domain.
    if (testCardParameterSpec(schemaKey) != nullptr)
        return isTestCardParameterValueValid(schemaKey, constant.value);
    // Four-Color Gradient: every parameter within its document domain.
    if (schemaKey.starts_with("bloom.four-color-gradient."))
        return isFourColorGradientParameterValueValid(schemaKey, constant.value);
    // #508 Light Rays: every parameter within its document domain.
    if (schemaKey.starts_with("bloom.light-rays."))
        return isLightRaysParameterValueValid(schemaKey, constant.value);
    // #568 Rolling Shutter: every parameter within its document domain.
    if (rollingShutterParameterSpec(schemaKey) != nullptr ||
        schemaKey == kRollingShutterCameraOffsetSchemaKey ||
        schemaKey == kRollingShutterCameraRollSchemaKey)
        return isRollingShutterParameterValueValid(schemaKey, constant.value);
    // #348 Corner Pin, ST Map and Displace: every parameter and corner within its document domain.
    if (warpParameterSpec(schemaKey) != nullptr || isWarpPointSchemaKey(schemaKey))
        return isWarpParameterValueValid(schemaKey, constant.value);
    // #563 Codec Degrade: every parameter within its document domain.
    if (codecDegradeParameterSpec(schemaKey) != nullptr)
        return isCodecDegradeParameterValueValid(schemaKey, constant.value);
    // #569 audio effects: every parameter within its document domain.
    if (audioEffectParameterSpec(schemaKey) != nullptr)
        return isAudioEffectParameterValueValid(schemaKey, constant.value);
    // #566 Auto Exposure: every parameter within its document domain.
    if (autoExposureParameterSpec(schemaKey) != nullptr)
        return isAutoExposureParameterValueValid(schemaKey, constant.value);
    // #481 Image Math, Image Map Range and Image Clamp: every parameter within its domain.
    if (imagePixelParameterSpec(schemaKey) != nullptr)
        return isImagePixelParameterValueValid(schemaKey, constant.value);
    // #518 Tone Map: every parameter within its document domain.
    if (colorCleanupParameterSpec(schemaKey) != nullptr)
        return isColorCleanupParameterValueValid(schemaKey, constant.value);
    // #484 the motion-blur nodes and Transform 2D's motion blur.
    if (motionParameterSpec(schemaKey) != nullptr)
        return isMotionParameterValueValid(schemaKey, constant.value);
    // #514 Denoise & Smooth: the closed mode and frame count, radius and strength in domain.
    if (schemaKey.starts_with("bloom.denoise."))
        return isDenoiseParameterValueValid(schemaKey, constant.value);
    // #486 Sharpen: every parameter within its document domain.
    if (sharpenParameterSpec(schemaKey) != nullptr)
        return isSharpenParameterValueValid(schemaKey, constant.value);
    // #505 Vignette: finite scalars within their domains, a finite centre offset within +-1e6
    // pixels and the closed shape and mode.
    if (isVignetteScalarSchemaKey(schemaKey)) {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) &&
               isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (schemaKey == kVignetteCenterSchemaKey) {
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y) &&
               std::abs(value->x) <= kMaximumVignetteCenter &&
               std::abs(value->y) <= kMaximumVignetteCenter;
    }
    if (const auto domain = vignetteIntegerDomain(schemaKey)) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= domain->minimum && *value <= domain->maximum;
    }
    // #339 Drop Shadow, Glow and Light Wrap.
    if (isRecipeScalarSchemaKey(schemaKey)) {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) &&
               isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (isRecipeColorSchemaKey(schemaKey)) {
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid();
    }
    if (isRecipeBooleanSchemaKey(schemaKey))
        return std::holds_alternative<bool>(constant.value);
    // Glow's octave count, mode (#506) and ray count (#506).
    if (const auto domain = recipeIntegerDomain(schemaKey)) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= domain->minimum && *value <= domain->maximum;
    }
    if (schemaKey == "bloom.rotate-90.quarter-turns")
        return std::holds_alternative<std::int64_t>(constant.value);
    if (schemaKey == "bloom.transform-2d.translate" || schemaKey == "bloom.transform-2d.pivot" ||
        schemaKey == "bloom.transform-2d.scale") {
        // Finite, otherwise unbounded, like the Layer Output transform: a negative scale mirrors
        // and a zero scale collapses the image to no pixels rather than being refused.
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y);
    }
    if (schemaKey == "bloom.transform-2d.rotation") {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value);
    }
    if (schemaKey == "bloom.transform-2d.extent" || schemaKey == "bloom.transform-2d.filter") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 &&
               *value <= (schemaKey == "bloom.transform-2d.extent" ? 2 : 1);
    }
    if (schemaKey.starts_with("bloom.resize-fit.")) {
        if (schemaKey == "bloom.resize-fit.bypass")
            return std::holds_alternative<bool>(constant.value);
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        if (!value)
            return false;
        if (schemaKey == "bloom.resize-fit.filter")
            return *value == 0 || *value == 1;
        if (schemaKey == "bloom.resize-fit.policy")
            return *value >= 0 && *value <= 1;
        return *value >= 1 && *value <= 32768;
    }
    if (schemaKey == "bloom.ocio-effect.bypass" || schemaKey == "bloom.ocio-effect.look")
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == "bloom.ocio-file.lut" || schemaKey == "bloom.ocio-file.interpolation" ||
        schemaKey == "bloom.ocio-file.direction") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 &&
               (schemaKey == "bloom.ocio-file.lut" ||
                *value <= (schemaKey == "bloom.ocio-file.interpolation" ? 2 : 1));
    }
    if (isChannelParameterSchemaKey(schemaKey))
        return isChannelParameterValueValid(schemaKey, constant.value);
    if (schemaKey == kImagePreviewDisplayParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value <= kMaximumImagePreviewDisplay;
    }
    if (schemaKey == "bloom.image.asset")
        return std::holds_alternative<std::string>(constant.value);
    if (schemaKey == kImageInputColorSpaceIdParameterSchemaKey ||
        schemaKey == kVideoInputColorSpaceIdParameterSchemaKey) {
        const auto* value = std::get_if<std::string>(&constant.value);
        return value != nullptr && bloom::core::isValidUtf8(*value) && value->size() <= 256 &&
               value->find('\0') == std::string::npos;
    }
    if (schemaKey == "bloom.image.premultiply")
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == "bloom.image.start-frame" || schemaKey == "bloom.image.loop-mode" ||
        schemaKey == "bloom.image.color-space") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value &&
               (schemaKey == "bloom.image.start-frame"
                    ? (*value >= -1000000000 && *value <= 1000000000)
                    : (*value >= 0 && *value <= (schemaKey == "bloom.image.loop-mode" ? 2 : 3)));
    }
    if (schemaKey == "bloom.video.asset")
        return std::holds_alternative<std::string>(constant.value);
    if (schemaKey == kVideoAudioStreamParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 && *value < 256;
    }
    if (schemaKey == "bloom.video.start-frame" || schemaKey == "bloom.video.loop-mode" ||
        schemaKey == "bloom.video.color-space") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value &&
               (schemaKey == "bloom.video.start-frame"
                    ? (*value >= -1000000000 && *value <= 1000000000)
                    : (*value >= 0 && *value <= (schemaKey == "bloom.video.loop-mode" ? 2 : 3)));
    }
    if (schemaKey == kTextAlignmentParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 && *value <= 2;
    }
    if (schemaKey == kTextFontParameterSchemaKey) {
        // A text font is a font ID (docs/architecture/fonts.md). Older documents' asset ids,
        // references and face numbers are converted to one when the project opens.
        const auto* text = std::get_if<std::string>(&constant.value);
        return text != nullptr && decodeFontId(*text).has_value();
    }
    if (schemaKey == kTextBoxParameterSchemaKey) {
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y) &&
               value->x >= 0.0 && value->y >= 0.0 && value->x <= 1'000'000.0 &&
               value->y <= 1'000'000.0 &&
               ((value->x == 0.0 && value->y == 0.0) || (value->x > 0.0 && value->y > 0.0));
    }
    if (schemaKey == kTextWrapParameterSchemaKey)
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == kTextVerticalAlignmentParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value <= 2;
    }
    if (schemaKey == kTextAnchorModeParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value <= kTextAnchorModeFollowsAlignment;
    }
    if (schemaKey == kTextOverflowParameterSchemaKey) {
        // Overflow is Clip (0) or Grow (1); the renderer rejects anything above Grow.
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value <= 1;
    }
    if (schemaKey == kTextLineHeightParameterSchemaKey ||
        schemaKey == kTextLetterSpacingParameterSchemaKey) {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && isScalarWithinSchemaDomain(schemaKey, *value);
    }
    if (schemaKey == kTextParameterSchemaKey) {
        return std::holds_alternative<std::string>(constant.value);
    }
    if (schemaKey == kTextSizeParameterSchemaKey) {
        const auto* size = std::get_if<double>(&constant.value);
        return size != nullptr && std::isfinite(*size) && *size > 0.0 &&
               *size <= kMaximumTextSizePixels;
    }
    if (schemaKey == kTextColorParameterSchemaKey) {
        // Exactly the solid color rule: a finite straight RGBA authoring value with alpha in [0,
        // 1], unbounded in RGB so an HDR or negative channel survives a round trip.
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid();
    }
    return valueGraphConstantMatchesSchema(schemaKey, constant);
}

[[nodiscard]] bool isValidSourceForSchema(const std::string_view schemaKey,
                                          const bloom::document::ParameterSource& source) {
    if ((schemaKey.starts_with("bloom.ocio-") || schemaKey.starts_with("bloom.resize-fit.") ||
         schemaKey == "bloom.transform-2d.extent" || schemaKey == "bloom.transform-2d.filter" ||
         schemaKey == bloom::document::kImagePreviewDisplayParameterSchemaKey ||
         bloom::document::isRecipeIntegerSchemaKey(schemaKey) ||
         schemaKey == bloom::document::kAlphaAssociationModeSchemaKey ||
         schemaKey == bloom::document::kAlphaAssociationWhiteSchemaKey ||
         bloom::document::isColorizeIntegerSchemaKey(schemaKey) ||
         bloom::document::isVignetteIntegerSchemaKey(schemaKey) ||
         bloom::document::isFieldConstantSchemaKey(schemaKey) ||
         bloom::document::isDepthConstantSchemaKey(schemaKey) ||
         bloom::document::isLensFlareConstantSchemaKey(schemaKey) ||
         bloom::document::isFilmGrainConstantSchemaKey(schemaKey) ||
         bloom::document::isCodecDegradeConstantSchemaKey(schemaKey) ||
         bloom::document::isAutoExposureConstantSchemaKey(schemaKey) ||
         bloom::document::isAudioEffectConstantSchemaKey(schemaKey) ||
         bloom::document::isLensDistortionConstantSchemaKey(schemaKey) ||
         bloom::document::isRollingShutterConstantSchemaKey(schemaKey) ||
         bloom::document::isWarpConstantSchemaKey(schemaKey) ||
         bloom::document::isTestCardConstantSchemaKey(schemaKey) ||
         bloom::document::isLightRaysConstantSchemaKey(schemaKey) ||
         bloom::document::isMotionConstantSchemaKey(schemaKey) ||
         bloom::document::isImagePixelConstantSchemaKey(schemaKey) ||
         bloom::document::isColorCleanupConstantSchemaKey(schemaKey) ||
         bloom::document::isRecipeBooleanSchemaKey(schemaKey)) &&
        !std::holds_alternative<bloom::document::ConstantValueSource>(source))
        return false;
    // #589: a shape's own path is a constant, or the Path socket's driver. Keys live on the Path
    // node.
    if ((schemaKey == "bloom.shape.path" ||
         schemaKey == bloom::document::kPathOperandParameterSchemaKey) &&
        std::holds_alternative<bloom::document::AnimationCurveSource>(source))
        return false;
    // #543: a stop link holds its driver and nothing else.
    if (bloom::document::isGradientStopSchemaKey(schemaKey))
        return std::holds_alternative<bloom::document::DriverBindingSource>(source) &&
               isGenericallyValidSource(source);
    if (!isGenericallyValidSource(source)) {
        return false;
    }
    if (const auto* constant = std::get_if<bloom::document::ConstantValueSource>(&source)) {
        return constantMatchesSchema(schemaKey, *constant);
    }

    if (const auto* animation = std::get_if<bloom::document::AnimationCurveSource>(&source)) {
        if (!animation->defaultValue.has_value()) {
            return true;
        }
        return constantMatchesSchema(
            schemaKey, bloom::document::ConstantValueSource{*animation->defaultValue});
    }

    // Composition validation owns typed curve resolution because a ParameterStore cannot inspect
    // its composition's curve store. Driver evaluation remains deferred.
    return true;
}

} // namespace

namespace bloom::document {

bool isVec2AnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == "bloom.shape.size" || schemaKey == "bloom.shape.line-start" ||
           schemaKey == "bloom.shape.line-end" || schemaKey == kPositionParameterSchemaKey ||
           schemaKey == kAnchorParameterSchemaKey || schemaKey == kScaleParameterSchemaKey ||
           schemaKey == kGradientStartParameterSchemaKey ||
           schemaKey == kGradientEndParameterSchemaKey ||
           schemaKey == kVector2ValueParameterSchemaKey ||
           schemaKey == "bloom.transform-2d.translate" || schemaKey == "bloom.transform-2d.pivot" ||
           schemaKey == "bloom.transform-2d.scale" || schemaKey == kVignetteCenterSchemaKey ||
           schemaKey == kLensDistortionCenterSchemaKey ||
           schemaKey == kChromaticAberrationCenterSchemaKey ||
           schemaKey == kLightRaysCenterSchemaKey || isFourColorGradientPointSchemaKey(schemaKey) ||
           schemaKey == kRollingShutterCameraOffsetSchemaKey ||
           isWarpPointSchemaKey(schemaKey) ||
           isFieldVec2AnimatableSchemaKey(schemaKey) || isDepthVec2AnimatableSchemaKey(schemaKey) ||
           isLensFlareVec2SchemaKey(schemaKey) || isMotionVec2AnimatableSchemaKey(schemaKey);
}

bool isScalarAnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == "bloom.shape.stroke-width" || schemaKey == kOpacityParameterSchemaKey ||
           schemaKey == kRotationParameterSchemaKey || schemaKey == kTextSizeParameterSchemaKey ||
           schemaKey == kTimeRemapParameterSchemaKey ||
           schemaKey == kScalarValueParameterSchemaKey ||
           schemaKey == kSolidWidthParameterSchemaKey ||
           schemaKey == kSolidHeightParameterSchemaKey ||
           schemaKey == kTextLineHeightParameterSchemaKey ||
           schemaKey == kTextLetterSpacingParameterSchemaKey ||
           schemaKey == kAudioLevelParameterSchemaKey ||
           schemaKey == "bloom.composition-source.time-offset" ||
           schemaKey == "bloom.composition-source.time-scale" ||
           schemaKey == "bloom.exposure.stops" || schemaKey == "bloom.offset.amount" ||
           schemaKey == "bloom.multiply.factor" || schemaKey == "bloom.image-mix.factor" ||
           schemaKey == "bloom.crop.left" || schemaKey == "bloom.crop.top" ||
           schemaKey == "bloom.crop.right" || schemaKey == "bloom.crop.bottom" ||
           schemaKey == "bloom.pad.left" || schemaKey == "bloom.pad.top" ||
           schemaKey == "bloom.pad.right" || schemaKey == "bloom.pad.bottom" ||
           schemaKey == "bloom.multiply-alpha.factor" || schemaKey == "bloom.blur.radius" ||
           schemaKey == "bloom.transform-2d.rotation" || schemaKey == "bloom.transform-2d.skew" ||
           schemaKey == "bloom.transform-2d.skew-axis" ||
           schemaKey == "bloom.transform-2d.opacity" || schemaKey == "bloom.blur.radius-y" ||
           isFourColorGradientScalarSchemaKey(schemaKey) ||
           schemaKey == "bloom.blur.red-scale" || schemaKey == "bloom.blur.green-scale" ||
           schemaKey == "bloom.blur.blue-scale" || schemaKey == "bloom.blur.alpha-scale" ||
           schemaKey == "bloom.luma-key.low" ||
           schemaKey == "bloom.luma-key.high" || isLevelsValueSchemaKey(schemaKey) ||
           schemaKey == kHueSaturationHueSchemaKey ||
           schemaKey == kHueSaturationSaturationSchemaKey ||
           schemaKey == kHueSaturationVibranceSchemaKey || isGradeValueSchemaKey(schemaKey) ||
           isRecipeScalarSchemaKey(schemaKey) || isColorizeScalarSchemaKey(schemaKey) ||
           isVignetteScalarSchemaKey(schemaKey) || isFieldScalarAnimatableSchemaKey(schemaKey) ||
           isDepthScalarAnimatableSchemaKey(schemaKey) || isLensFlareScalarSchemaKey(schemaKey) ||
           isFilmGrainScalarSchemaKey(schemaKey) || isCodecDegradeScalarSchemaKey(schemaKey) ||
           isAutoExposureScalarSchemaKey(schemaKey) || isAudioEffectScalarSchemaKey(schemaKey) ||
           isLensDistortionScalarSchemaKey(schemaKey) ||
           isChromaticAberrationScalarSchemaKey(schemaKey) ||
           isLightRaysScalarSchemaKey(schemaKey) ||
           isRollingShutterScalarSchemaKey(schemaKey) || isWarpScalarSchemaKey(schemaKey) ||
           sharpenParameterSpec(schemaKey) != nullptr ||
           schemaKey == kDenoiseRadiusSchemaKey || schemaKey == kDenoiseStrengthSchemaKey ||
           isMotionScalarAnimatableSchemaKey(schemaKey) || isImagePixelScalarSchemaKey(schemaKey) ||
           isColorCleanupScalarSchemaKey(schemaKey) ||
           schemaKey == kLayerMaskFeatherSchemaKey ||
           schemaKey == kLayerMaskExpansionSchemaKey || schemaKey == kLayerMaskOpacitySchemaKey;
}

bool isColor4AnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == "bloom.shape.fill-color" || schemaKey == "bloom.shape.stroke-color" ||
           schemaKey == kSolidColorParameterSchemaKey ||
           schemaKey == kGradientStartColorParameterSchemaKey ||
           schemaKey == kGradientEndColorParameterSchemaKey ||
           schemaKey == kTextColorParameterSchemaKey ||
           schemaKey == kColorValueParameterSchemaKey || isRecipeColorSchemaKey(schemaKey) ||
           isColorizeColorSchemaKey(schemaKey) || isDepthColorAnimatableSchemaKey(schemaKey) ||
           isLensFlareColorSchemaKey(schemaKey) || isImagePixelColorSchemaKey(schemaKey) ||
           isFourColorGradientColorSchemaKey(schemaKey);
}

bool isAnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return isVec2AnimatableSchemaKey(schemaKey) || isVec3AnimatableSchemaKey(schemaKey) ||
           isScalarAnimatableSchemaKey(schemaKey) || isColor4AnimatableSchemaKey(schemaKey) ||
           isPathAnimatableSchemaKey(schemaKey) || isGradientAnimatableSchemaKey(schemaKey);
}

bool isScalarWithinSchemaDomain(const std::string_view schemaKey,
                                                        const double value) noexcept {
    if (isCanvasMarginSchemaKey(schemaKey))
        return value >= 0.0 && value <= 16777216.0;
    if (schemaKey == "bloom.blur.radius" || schemaKey == "bloom.blur.radius-y")
        return value >= 0.0 && value <= kMaximumBlurRadiusPixels;
    if (schemaKey == "bloom.blur.red-scale" || schemaKey == "bloom.blur.green-scale" ||
        schemaKey == "bloom.blur.blue-scale" || schemaKey == "bloom.blur.alpha-scale" ||
        schemaKey == "bloom.transform-2d.opacity")
        return value >= 0.0 && value <= 1.0;
    if (schemaKey == "bloom.transform-2d.skew")
        return std::abs(value) <= kMaximumTransformSkew;
    if (isFourColorGradientScalarSchemaKey(schemaKey))
        return isFourColorGradientBlendWithinDomain(value);
    if (schemaKey == "bloom.luma-key.low" || schemaKey == "bloom.luma-key.high")
        return value >= -kMaximumLumaKeyThreshold && value <= kMaximumLumaKeyThreshold;
    if (isLevelsGammaSchemaKey(schemaKey))
        return value >= kMinimumLevelsGamma && value <= kMaximumLevelsGamma;
    if (isLevelsValueSchemaKey(schemaKey) || schemaKey == kHueSaturationHueSchemaKey)
        return value >= -kMaximumGradeValue && value <= kMaximumGradeValue;
    if (schemaKey == kHueSaturationSaturationSchemaKey)
        return value >= 0.0 && value <= kMaximumGradeSaturation;
    if (schemaKey == kHueSaturationVibranceSchemaKey)
        return value >= -kMaximumVibrance && value <= kMaximumVibrance;
    // #494 Grade: each wheel value and primary within its own domain (color_grade_nodes.hpp).
    if (const auto grade = gradeValueDomain(schemaKey))
        return value >= grade->minimum && value <= grade->maximum;
    // #339 Drop Shadow, Glow and Light Wrap (compositing_recipe_nodes.hpp).
    if (const auto recipe = recipeScalarDomain(schemaKey))
        return value >= recipe->minimum && value <= recipe->maximum;
    // #504 Colorize (colorize_nodes.hpp).
    if (const auto colorize = colorizeScalarDomain(schemaKey))
        return value >= colorize->minimum && value <= colorize->maximum;
    // #505 Vignette (vignette_nodes.hpp).
    if (const auto vignette = vignetteScalarDomain(schemaKey))
        return value >= vignette->minimum && value <= vignette->maximum;
    // #525 Fields (field_parameters.hpp).
    if (fieldParameterSpec(schemaKey) != nullptr)
        return isFieldScalarWithinDomain(schemaKey, value);
    // #483 Z Defocus and Depth Fog (depth_effect_parameters.hpp).
    if (depthParameterSpec(schemaKey) != nullptr)
        return isDepthScalarWithinDomain(schemaKey, value);
    // #507 Lens Flare (lens_flare_nodes.hpp).
    if (lensFlareParameterSpec(schemaKey) != nullptr)
        return isLensFlareScalarWithinDomain(schemaKey, value);
    // #486 Film Grain (film_grain_nodes.hpp).
    if (filmGrainParameterSpec(schemaKey) != nullptr)
        return isFilmGrainScalarWithinDomain(schemaKey, value);
    // #486 Lens Distortion (lens_distortion_nodes.hpp).
    if (lensDistortionParameterSpec(schemaKey) != nullptr)
        return isLensDistortionScalarWithinDomain(schemaKey, value);
    // #486 Chromatic Aberration (chromatic_aberration_nodes.hpp).
    if (isChromaticAberrationScalarSchemaKey(schemaKey))
        return isChromaticAberrationScalarWithinDomain(schemaKey, value);
    // #587 Test Card (test_card_nodes.hpp).
    if (testCardParameterSpec(schemaKey) != nullptr)
        return isTestCardScalarWithinDomain(schemaKey, value);
    // #508 Light Rays (light_rays_nodes.hpp).
    if (isLightRaysScalarSchemaKey(schemaKey))
        return isLightRaysScalarWithinDomain(schemaKey, value);
    // #568 Rolling Shutter (rolling_shutter_nodes.hpp).
    if (rollingShutterParameterSpec(schemaKey) != nullptr ||
        schemaKey == kRollingShutterCameraRollSchemaKey)
        return isRollingShutterScalarWithinDomain(schemaKey, value);
    // #348 Corner Pin, ST Map and Displace (warp_nodes.hpp).
    if (warpParameterSpec(schemaKey) != nullptr)
        return isWarpScalarWithinDomain(schemaKey, value);
    // #563 Codec Degrade (codec_degrade_nodes.hpp).
    if (codecDegradeParameterSpec(schemaKey) != nullptr)
        return isCodecDegradeScalarWithinDomain(schemaKey, value);
    // #569 audio effects (audio_effect_nodes.hpp).
    if (audioEffectParameterSpec(schemaKey) != nullptr)
        return isAudioEffectScalarWithinDomain(schemaKey, value);
    // #566 Auto Exposure (auto_exposure_nodes.hpp).
    if (autoExposureParameterSpec(schemaKey) != nullptr)
        return isAutoExposureScalarWithinDomain(schemaKey, value);
    // #481 Image Math, Image Map Range and Image Clamp (image_math_parameters.hpp).
    if (imagePixelParameterSpec(schemaKey) != nullptr)
        return isImagePixelScalarWithinDomain(schemaKey, value);
    // #518 Tone Map (color_cleanup_nodes.hpp).
    if (colorCleanupParameterSpec(schemaKey) != nullptr)
        return isColorCleanupScalarWithinDomain(schemaKey, value);
    // #484 the motion-blur nodes (motion_blur_parameters.hpp).
    if (motionParameterSpec(schemaKey) != nullptr)
        return isMotionScalarWithinDomain(schemaKey, value);
    // #486 Sharpen (sharpen_nodes.hpp).
    if (sharpenParameterSpec(schemaKey) != nullptr)
        return isSharpenScalarWithinDomain(schemaKey, value);
    // #514 Denoise & Smooth (denoise_nodes.hpp).
    if (schemaKey.starts_with("bloom.denoise."))
        return isDenoiseScalarWithinDomain(schemaKey, value);
    if (schemaKey == kLayerMaskFeatherSchemaKey)
        return value >= 0.0 && value <= kMaximumLayerMaskFeatherPixels;
    if (schemaKey == kLayerMaskExpansionSchemaKey)
        return value >= -kMaximumLayerMaskExpansionPixels &&
               value <= kMaximumLayerMaskExpansionPixels;
    if (schemaKey == "bloom.shape.stroke-width")
        return value >= 0.0;
    if (schemaKey == kSolidWidthParameterSchemaKey || schemaKey == kSolidHeightParameterSchemaKey)
        return value >= 1.0;
    if (schemaKey == kAudioLevelParameterSchemaKey)
        return value >= 0.0 && value <= 2.0;
    // #480: a Shuffle's or Pass's non-finite substitute lands in a Float32 sample.
    if (schemaKey == "bloom.channel-shuffle.non-finite" ||
        schemaKey == "bloom.channel-pass.non-finite")
        return value >= -3.4028234663852886e38 && value <= 3.4028234663852886e38;
    if (schemaKey == kTextLineHeightParameterSchemaKey)
        return value > 0.0;
    if (hasUnitDomainSchemaKey(schemaKey)) {
        return value >= 0.0 && value <= 1.0;
    }
    if (hasTextSizeDomainSchemaKey(schemaKey)) {
        return value > 0.0 && value <= kMaximumTextSizePixels;
    }
    return true;
}


PathAnchorMode inferPathAnchorMode(const PathAnchor& anchor) noexcept {
    if (!anchor.inHandle && !anchor.outHandle)
        return PathAnchorMode::Corner;
    if (!anchor.inHandle || !anchor.outHandle)
        return PathAnchorMode::Free;
    const double ix = anchor.inHandle->x - anchor.point.x;
    const double iy = anchor.inHandle->y - anchor.point.y;
    const double ox = anchor.outHandle->x - anchor.point.x;
    const double oy = anchor.outHandle->y - anchor.point.y;
    const double inLength = std::hypot(ix, iy);
    const double outLength = std::hypot(ox, oy);
    if (inLength == 0.0 || outLength == 0.0)
        return PathAnchorMode::Free;
    // Collinear and opposed, within a relative tolerance that absorbs the rounding of edits.
    constexpr double kTolerance = 1e-9;
    const double cross = ix * oy - iy * ox;
    const double dot = ix * ox + iy * oy;
    if (std::fabs(cross) > kTolerance * inLength * outLength || dot >= 0.0)
        return PathAnchorMode::Free;
    return std::fabs(inLength - outLength) <= kTolerance * std::max(inLength, outLength)
               ? PathAnchorMode::Mirrored
               : PathAnchorMode::Aligned;
}

std::string_view pathAnchorModeName(const PathAnchorMode mode) noexcept {
    switch (mode) {
    case PathAnchorMode::Corner:
        return "corner";
    case PathAnchorMode::Mirrored:
        return "mirrored";
    case PathAnchorMode::Aligned:
        return "aligned";
    case PathAnchorMode::Free:
        return "free";
    case PathAnchorMode::Auto:
        return "auto";
    }
    return "corner";
}

std::optional<PathAnchorMode> pathAnchorModeFromName(const std::string_view name) noexcept {
    for (const auto mode : {PathAnchorMode::Corner, PathAnchorMode::Mirrored,
                            PathAnchorMode::Aligned, PathAnchorMode::Free, PathAnchorMode::Auto})
        if (pathAnchorModeName(mode) == name)
            return mode;
    return std::nullopt;
}

std::string_view pathFillRuleName(const PathFillRule rule) noexcept {
    switch (rule) {
    case PathFillRule::Inherit:
        return "inherit";
    case PathFillRule::NonZero:
        return "nonZero";
    case PathFillRule::EvenOdd:
        return "evenOdd";
    }
    return "inherit";
}

std::optional<PathFillRule> pathFillRuleFromName(const std::string_view name) noexcept {
    for (const auto rule : {PathFillRule::Inherit, PathFillRule::NonZero, PathFillRule::EvenOdd})
        if (pathFillRuleName(rule) == name)
            return rule;
    return std::nullopt;
}

PathValue PathValue::fromContour(PathContour contour) {
    PathValue value;
    if (!contour.anchors.empty() || contour.closed)
        value.contours.push_back(std::move(contour));
    return value;
}

PathValue PathValue::fromContour(std::vector<PathAnchor> anchors, const bool closed) {
    return fromContour(PathContour{std::move(anchors), closed});
}

const PathContour& PathValue::firstContour() const noexcept {
    static const PathContour kEmpty;
    return contours.empty() ? kEmpty : contours.front();
}

PathContour& PathValue::editableContour() {
    if (contours.empty())
        contours.emplace_back();
    return contours.front();
}

bool PathValue::isSimple() const noexcept {
    return contours.size() <= 1 && fillRule == PathFillRule::Inherit && groups.empty();
}

std::size_t PathValue::anchorCount() const noexcept {
    std::size_t count = 0;
    for (const auto& contour : contours)
        count += contour.anchors.size();
    return count;
}

bool PathValue::isValid() const noexcept {
    const auto finite = [](const Vec2d point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    if (contours.size() > kMaximumPathContours || groups.size() > kMaximumPathGroups ||
        (fillRule != PathFillRule::Inherit && fillRule != PathFillRule::NonZero &&
         fillRule != PathFillRule::EvenOdd))
        return false;
    std::size_t total = 0;
    for (const auto& contour : contours) {
        total += contour.anchors.size();
        if (contour.anchors.size() > kMaximumPathAnchors || total > kMaximumPathValueAnchors ||
            !std::ranges::all_of(contour.anchors, [&](const PathAnchor& anchor) {
                return finite(anchor.point) && (!anchor.inHandle || finite(*anchor.inHandle)) &&
                       (!anchor.outHandle || finite(*anchor.outHandle));
            }))
            return false;
    }
    // Ascending, in range and not overlapping; keys present, bounded, well formed and unique.
    std::uint64_t next = 0;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const auto& group = groups[index];
        if (group.key.empty() || group.key.size() > kMaximumPathGroupKeyBytes ||
            !core::isValidUtf8(group.key) || group.contourCount == 0 || group.firstContour < next ||
            static_cast<std::uint64_t>(group.firstContour) + group.contourCount > contours.size() ||
            (group.color && !group.color->isValid()))
            return false;
        next = static_cast<std::uint64_t>(group.firstContour) + group.contourCount;
    }
    if (groups.size() < 2)
        return true;
    try {
        std::vector<std::string_view> keys;
        keys.reserve(groups.size());
        for (const auto& group : groups)
            keys.emplace_back(group.key);
        std::ranges::sort(keys);
        return std::ranges::adjacent_find(keys) == keys.end();
    } catch (...) {
        return false;
    }
}

std::string pathGroupKeyFromSpan(const std::uint64_t begin, const std::uint64_t end) {
    return "span:" + std::to_string(begin) + ":" + std::to_string(end);
}

bool GradientValue::isValid() const noexcept {
    return !stops.empty() && stops.size() <= kMaximumGradientStops &&
           isGradientInterpolation(static_cast<std::uint8_t>(modes.interpolation)) &&
           isGradientColorMode(static_cast<std::uint8_t>(modes.colorMode)) &&
           isGradientHueDirection(static_cast<std::uint8_t>(modes.hueDirection)) &&
           std::ranges::all_of(stops, [](const GradientStop& stop) {
               return std::isfinite(stop.position) && stop.position >= 0.0 &&
                      stop.position <= 1.0 && stop.color.isValid();
           });
}

GradientValue defaultColorizeGradient() {
    return {{{0.0, core::Color4d{0.0, 0.0, 0.0, 1.0}},
             {0.18, core::Color4d{0.24, 0.17, 0.1, 1.0}},
             {1.0, core::Color4d{1.0, 1.0, 1.0, 1.0}}}};
}

GradientValue defaultGeneratorGradient() {
    return {{{0.0, core::Color4d{0.0, 0.0, 0.0, 1.0}}, {1.0, core::Color4d{1.0, 1.0, 1.0, 1.0}}}};
}

GradientValue defaultGlowTintGradient() {
    return {{{0.0, core::Color4d{1.0, 1.0, 1.0, 1.0}}, {1.0, core::Color4d{1.0, 0.5, 0.25, 1.0}}}};
}

bool isBoundedGradient(const GradientValue& value) noexcept {
    return value.isValid() && std::ranges::all_of(value.stops, [](const GradientStop& stop) {
               return std::abs(stop.color.red) <= kMaximumColorizeColor &&
                      std::abs(stop.color.green) <= kMaximumColorizeColor &&
                      std::abs(stop.color.blue) <= kMaximumColorizeColor;
           });
}

std::string gradientStopPortName(const std::string_view gradientRole, const std::uint32_t stop,
                                 const GradientStopComponent component) {
    return std::string(gradientRole) + ".stops[" + std::to_string(stop) + "]." +
           (component == GradientStopComponent::Color ? "color" : "position");
}

std::optional<GradientStopPort> parseGradientStopPort(const std::string_view port) {
    constexpr std::string_view marker = ".stops[";
    const auto at = port.rfind(marker);
    if (at == std::string_view::npos || at == 0)
        return std::nullopt;
    const auto digits = port.substr(at + marker.size());
    const auto close = digits.find(']');
    if (close == std::string_view::npos || close == 0 || close > 2 ||
        (close > 1 && digits.front() == '0'))
        return std::nullopt;
    std::uint32_t stop = 0;
    for (const char digit : digits.substr(0, close)) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        stop = stop * 10U + static_cast<std::uint32_t>(digit - '0');
    }
    if (stop >= kMaximumGradientStops)
        return std::nullopt;
    const auto suffix = digits.substr(close + 1);
    GradientStopComponent component{};
    if (suffix == ".color")
        component = GradientStopComponent::Color;
    else if (suffix == ".position")
        component = GradientStopComponent::Position;
    else
        return std::nullopt;
    return GradientStopPort{std::string(port.substr(0, at)), stop, component};
}

std::optional<GradientValue>
applyGradientStopValues(GradientValue value, const std::span<const GradientStopValue> values) {
    for (const auto& driven : values) {
        if (driven.stop >= value.stops.size())
            continue;
        auto& stop = value.stops[driven.stop];
        if (const auto* color = std::get_if<core::Color4d>(&driven.value)) {
            stop.color = *color;
        } else {
            const auto position = std::get<double>(driven.value);
            if (!std::isfinite(position))
                return std::nullopt;
            stop.position = std::clamp(position, 0.0, 1.0);
        }
    }
    if (!value.isValid())
        return std::nullopt;
    return value;
}

std::vector<GradientStop> sortedGradientStops(const GradientValue& value) {
    auto stops = value.stops;
    std::ranges::stable_sort(stops, {}, &GradientStop::position);
    return stops;
}

core::Color4d gradientColorAt(const GradientValue& value, const double position) {
    const auto stops = sortedGradientStops(value);
    if (stops.empty())
        return {};
    if (!value.modes.isLinearRgb()) {
        // #546: the shared definition, in binary64 with the spans taken from the positions.
        const auto at = [&](const std::size_t index) {
            const auto& stop = stops[index];
            double inverse = 0.0;
            if (index + 1 < stops.size()) {
                const double span = stops[index + 1].position - stop.position;
                inverse = span > 0.0 ? 1.0 / span : 0.0;
            }
            return gradient_math::Stop<double>{
                stop.position,
                std::isfinite(inverse) ? inverse : 0.0,
                {stop.color.red, stop.color.green, stop.color.blue, stop.color.alpha}};
        };
        const double firstPosition = stops.front().position;
        const double lastSpan = 1.0 - stops.back().position;
        const double leadIn = firstPosition > 0.0 ? 1.0 / firstPosition : 0.0;
        const double leadOut = lastSpan > 0.0 ? 1.0 / lastSpan : 0.0;
        const auto color = gradient_math::sample<double>(
            stops.size(), at, std::isfinite(leadIn) ? leadIn : 0.0,
            std::isfinite(leadOut) ? leadOut : 0.0, std::clamp(position, 0.0, 1.0), value.modes);
        return {color[0], color[1], color[2], color[3]};
    }
    if (!(position > stops.front().position))
        return stops.front().color;
    if (position >= stops.back().position)
        return stops.back().color;
    // The last stop at or before the position; the next one is strictly after it.
    std::size_t index = 0;
    while (index + 2 < stops.size() && stops[index + 1].position <= position)
        ++index;
    const auto& from = stops[index];
    const auto& to = stops[index + 1];
    const double u = (position - from.position) / (to.position - from.position);
    const auto mix = [u](const double a, const double b) { return a + u * (b - a); };
    return {mix(from.color.red, to.color.red), mix(from.color.green, to.color.green),
            mix(from.color.blue, to.color.blue), mix(from.color.alpha, to.color.alpha)};
}

const ParameterRecord* ParameterStore::find(const ParameterId id) const noexcept {
    if (sorted_) {
        const auto iterator = std::lower_bound(
            records_.begin(), records_.end(), id,
            [](const ParameterRecord& record, const ParameterId wanted) { return record.id < wanted; });
        return iterator != records_.end() && iterator->id == id ? &*iterator : nullptr;
    }
    const auto iterator = std::find_if(records_.begin(), records_.end(),
                                       [id](const auto& record) { return record.id == id; });
    return iterator == records_.end() ? nullptr : &*iterator;
}

bool ParameterStore::insert(ParameterRecord record) {
    if (!record.id.isValid() || !isValidStructuralText(record.schemaKey) ||
        !isValidSourceForSchema(record.schemaKey, record.source) || find(record.id) != nullptr) {
        return false;
    }

    if (!records_.empty() && !(records_.back().id < record.id))
        sorted_ = false;
    records_.push_back(std::move(record));
    return true;
}

bool ParameterStore::erase(const ParameterId id) {
    const auto iterator = std::find_if(records_.begin(), records_.end(),
                                       [id](const auto& record) { return record.id == id; });
    if (iterator == records_.end()) {
        return false;
    }

    records_.erase(iterator);
    return true;
}

bool ParameterStore::setSource(const ParameterId id, ParameterSource source) {
    const auto record = std::find_if(records_.begin(), records_.end(),
                                     [id](const auto& item) { return item.id == id; });
    if (record == records_.end() || !isValidSourceForSchema(record->schemaKey, source)) {
        return false;
    }

    record->source = std::move(source);
    return true;
}

ValidationResult ParameterStore::validate() const {
    ValidationResult result;
    std::unordered_set<ParameterId> ids;

    for (const auto& record : records_) {
        const auto path = "parameters[" + std::to_string(record.id.value()) + "]";
        if (!record.id.isValid()) {
            result.add(ValidationCode::InvalidId, path, "Parameter ID must not be zero");
        } else if (!ids.insert(record.id).second) {
            result.add(ValidationCode::DuplicateId, path, "Parameter ID is duplicated");
        }
        validateStructuralText(record.schemaKey, path + ".schemaKey", "Parameter schema key",
                               result);
        if (!isValidSourceForSchema(record.schemaKey, record.source)) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "Parameter source does not satisfy its schema");
        }
    }

    return result;
}

} // namespace bloom::document

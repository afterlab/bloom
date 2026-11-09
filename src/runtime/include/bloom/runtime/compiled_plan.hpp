#pragma once

#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/codec_degrade_nodes.hpp>
#include <bloom/document/auto_exposure_nodes.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/render/embedded_fonts.hpp>
#include <bloom/render/image_coverage.hpp>
#include <bloom/runtime/channel_expression.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>
#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/operation_index.hpp>

#include <array>
#include <bloom/document/asset.hpp>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {

struct AudioAnalysisSource;

// A compiled plan never crosses a process or persistence boundary: it is compiled in-process from a
// document snapshot and retained only by frames of that same process. What DOES cross such a
// boundary is ProcessFrameIdentity, and the two numbers it carries for this are
// kCpuCompositionEvaluatorSemanticsVersion and render::kCpuImagePrimitiveSemanticsVersion -- both
// bumped by the text path, because both describe pixels a cached or exported frame may already
// hold. Bump this one when the plan's own grammar changes in a way an existing plan value could
// misrepresent (a field's meaning changing, not a new alternative appearing).
//
// Task S5 bumped it 1 -> 2 for exactly that reason: CompiledSolid::color, CompiledText::size, and
// CompiledText::color stopped being resolved constants and became CompiledColorParameter/
// CompiledScalarParameter operands, because those three schemas are now animatable. A version-1
// plan's `color` field was a Color4d; a version-2 plan's is a parameter that may index a curve
// table, so the same field position means something different -- a field's meaning changing, not a
// new alternative appearing. TEXT-2 bumps this number to 5 because CompiledText now carries
// font-reference and box-layout grammar. Both numbers below also enter ProcessFrameIdentity and
// therefore every cached/exported frame digest (src/output/process_frame_semantic_identity.cpp),
// which is why the identity goldens were re-derived in the same change.
// COMP-SRC advances 5 -> 6: plans now own a table of nested composition plans,
// indexed by source operations with composition-time mapping operands.
// COLOR-3 freezes the reusable image-effect input/kernel grammar as plan semantics 8.
// ADR 0024 advances 8 -> 9: operand sampling and media selection are relative to an operation's
// time clock, and Layer Output ranges are in placement-clock time.
inline constexpr std::uint32_t kCompiledCompositionPlanSemanticsVersion = 10;
// Task S5 bumped this 1 -> 2: KeyframeInterpolation gained EaseInOut, so sampling can now produce a
// value no version-1 sampler could, and the Color4 curve table added a third sampled value kind.
// Easing equations advance it 2 -> 3: KeyframeInterpolation gained Robert Penner's ten easing
// equations and keys carry their parameters, so a sampler can now produce values no version-2
// sampler could. Every version-2 key (Hold, Linear, Bezier) samples bit-for-bit as before.
inline constexpr std::uint32_t kAnimationSamplingSemanticsVersion = 3;

// Each of the three typed operands gained ONE more alternative in task S7: a value-graph output,
// for a parameter a driver binding supplies per frame. A new alternative appearing is deliberately
// not a plan-semantics change -- no existing plan value means anything different than it did, and
// the field at this position is still "where this parameter's value comes from" -- which is the
// same reasoning the semantics-version comment at the top of this file already states.
// CompiledScalarParameter lives in compiled_composition_source.hpp (#480: a channel source carries
// the Composition Source mappings it is reached through).

struct CompiledVec2Parameter final {
    document::ParameterId id;
    std::variant<document::Vec2d, Vec2CurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledVec2Parameter&, const CompiledVec2Parameter&) = default;
};

struct CompiledVec3Parameter final {
    document::ParameterId id;
    std::variant<document::Vec3d, Vec3CurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledVec3Parameter&, const CompiledVec3Parameter&) = default;
};

struct CompiledColorParameter final {
    document::ParameterId id;
    std::variant<core::Color4d, Color4CurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledColorParameter&, const CompiledColorParameter&) = default;
};

// Content rectangles use pixel-edge coordinates in full-resolution local space.
// Bounds are evaluated from the plan's source operands and transformed input bounds at frame time.
struct ContentBounds final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    // The horizontal coordinate a Layer Output places at its position instead of the centre of
    // these bounds: the edge or centre of a text block whose anchor follows its alignment. Absent
    // for every other content, which keeps the bounds centre.
    std::optional<double> pivotX = std::nullopt;
    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    [[nodiscard]] document::Vec2d centre() const noexcept {
        return {(left + right) / 2.0, (top + bottom) / 2.0};
    }
    [[nodiscard]] document::Vec2d pivot() const noexcept {
        const auto middle = centre();
        return {pivotX.value_or(middle.x), middle.y};
    }
    friend bool operator==(const ContentBounds&, const ContentBounds&) = default;
};
struct EvaluatedOperationBounds final {
    ContentBounds local;
    ContentBounds output;
    document::LayerId layerId;
    std::array<document::Vec2d, 4> polygon{};
    document::Vec2d anchor;
    friend bool operator==(const EvaluatedOperationBounds&,
                           const EvaluatedOperationBounds&) = default;
};

// A lowered solid source. Its colour is a typed operand rather than a resolved constant (task S5):
// the solid colour schema is animatable now, so the value is either a constant or an index into the
// plan's Color4 curve table -- exactly the shape a Layer Output transform operand already had.
// #541: a gradient parameter is a constant or an inline Gradient curve.
// #543: a gradient stop's colour (a Color output) or position (a Scalar output) this frame.
struct CompiledGradientStopDriver final {
    document::ParameterId id;
    std::uint32_t stop = 0;
    document::GradientStopComponent component = document::GradientStopComponent::Color;
    ValueOutputIndex value = ValueOutputIndex::fromRaw(0);
    friend bool operator==(const CompiledGradientStopDriver&,
                           const CompiledGradientStopDriver&) = default;
};

struct CompiledGradientParameter final {
    document::ParameterId id;
    std::variant<document::GradientValue, CompiledGradientCurve> source;
    // #543: the stops a value node drives, applied over the sampled gradient in this order.
    std::vector<CompiledGradientStopDriver> drivers{};
    friend bool operator==(const CompiledGradientParameter&,
                           const CompiledGradientParameter&) = default;
};

// Linear and radial generators share the Solid source's explicit extent and pixel-window path.
// Their colours interpolate as straight scene-linear RGBA between the stops of `stops` (#541), then
// premultiply once for storage. The Solid's own colour is unused (a transparent constant).
struct CompiledGradient final {
    bool radial = false;
    CompiledGradientParameter stops;
    CompiledVec2Parameter start;
    CompiledVec2Parameter end;
    document::ParameterId repeatId;
    std::int64_t repeat = 0; // 0 clamp, 1 repeat, 2 mirror
    friend bool operator==(const CompiledGradient&, const CompiledGradient&) = default;
};

struct CompiledSolid {
    CompiledSolid() = default;
    CompiledSolid(document::NodeId nodeId, CompiledColorParameter authoredColor,
                  CompiledScalarParameter authoredWidth, CompiledScalarParameter authoredHeight,
                  std::optional<CompiledGradient> authoredGradient = {})
        : sourceNodeId(nodeId), color(std::move(authoredColor)), width(std::move(authoredWidth)),
          height(std::move(authoredHeight)), gradient(std::move(authoredGradient)) {}
    document::NodeId sourceNodeId;
    CompiledColorParameter color;
    CompiledScalarParameter width;
    CompiledScalarParameter height;
    std::optional<CompiledGradient> gradient;

    friend bool operator==(const CompiledSolid&, const CompiledSolid&) = default;
};

// Checker colours are straight authored values; cell and canvas dimensions are in authored
// pixels. The reference and resident paths share a 4x4 sample grid at output resolution.
struct CompiledChecker final {
    document::NodeId sourceNodeId;
    CompiledColorParameter colorA;
    CompiledColorParameter colorB;
    std::uint32_t cellWidth = 64;
    std::uint32_t cellHeight = 64;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    friend bool operator==(const CompiledChecker&, const CompiledChecker&) = default;
};

// A lowered text source. Size and colour became typed operands in task S5, when both schemas
// became animatable. Content still never INTERPOLATES -- a String has no midpoint and no command in
// the surface can put it on a curve -- but since task DRIVE-1 it can VARY, because a driver binding
// can hand it a new String every frame; the two are different questions, and only the first is
// about curves. Each value carries its own parameter identity so a diagnostic can name the exact
// parameter that failed, exactly as CompiledSolid does.
struct CompiledTextLayout {
    CompiledTextLayout() = default;
    CompiledTextLayout(document::ParameterId alignmentParameterId,
                       const std::int64_t alignmentValue, CompiledScalarParameter lineHeightValue,
                       CompiledScalarParameter letterSpacingValue,
                       std::optional<ValueOutputIndex> drivenAlignmentValue = {})
        : alignmentId(alignmentParameterId), alignment(alignmentValue), lineHeight(lineHeightValue),
          letterSpacing(letterSpacingValue), drivenAlignment(drivenAlignmentValue) {}

    document::ParameterId alignmentId;
    std::int64_t alignment = 0;
    CompiledScalarParameter lineHeight;
    CompiledScalarParameter letterSpacing;
    // Task DRIVE-1. The value-graph output this alignment is driven by, when it is driven at all.
    // See CompiledText::drivenContent for why a kind that cannot interpolate carries its driver
    // beside its authored value rather than as a third alternative of a typed operand.
    std::optional<ValueOutputIndex> drivenAlignment{};
    document::ParameterId boxId;
    document::Vec2d box{};
    document::ParameterId wrapId;
    bool wrap = false;
    document::ParameterId verticalAlignmentId;
    std::int64_t verticalAlignment = 0;
    document::ParameterId anchorModeId;
    std::int64_t anchorMode = 0;
    document::ParameterId overflowId;
    std::int64_t overflow = 0;
    friend bool operator==(const CompiledTextLayout&, const CompiledTextLayout&) = default;
};

// The local horizontal coordinate a Layer Output places at its position for a text whose anchor
// follows its alignment: the left edge, centre or right edge of the text block, which is the
// layout origin for point text (the layout puts the anchor at x = 0) and the authored box for boxed
// text. Absent for the stored anchor modes that predate this one, whose layer keeps the centre of
// the content bounds, so a project saved before it renders where it always did. Author-space
// units, like ContentBounds.
[[nodiscard]] inline std::optional<double>
textBlockPivotX(const CompiledTextLayout& layout, const std::int64_t alignment) noexcept {
    if (layout.anchorMode != document::kTextAnchorModeFollowsAlignment)
        return std::nullopt;
    const double extent = layout.box.x > 0.0 ? layout.box.x : 0.0;
    return alignment == 1 ? extent / 2.0 : alignment == 2 ? extent : 0.0;
}

class CompiledCompositionPlan;

// CompiledCompositionTimeMapping and CompiledCompositionSource: compiled_composition_source.hpp.

// Image effects publish finite premultiplied RGBA32F. Pointwise colour kernels preserve windows
// and alpha while operating on straight RGB; neighborhood kernels declare their own associated-
// RGBA and window/halo policy. Canvas and spatial kernels can change image windows. Identity and
// bypass share the input storage exactly.
struct IdentityImageKernel final {
    friend bool operator==(const IdentityImageKernel&, const IdentityImageKernel&) = default;
};
struct CstKernel final {
    std::string fromId;
    std::string toId;
    friend bool operator==(const CstKernel&, const CstKernel&) = default;
};
struct FileTransformKernel final {
    document::AssetId lutAssetId;
    std::int64_t interpolation = 0;
    std::int64_t direction = 0;
    std::string processSpaceId;
    std::optional<document::AssetRecord> asset;
    friend bool operator==(const FileTransformKernel&, const FileTransformKernel&) = default;
};
// Scene-linear, straight-RGB affine effects. The sampled parameter is absent for Invert.
// Every mode preserves alpha and all image windows; no display-referred clamp is implied.
enum class PointwiseImageEffect : std::uint8_t { Exposure, Offset, Multiply, Invert };
struct PointwiseImageKernel final {
    PointwiseImageEffect effect = PointwiseImageEffect::Invert;
    std::optional<CompiledScalarParameter> amount;
    friend bool operator==(const PointwiseImageKernel&, const PointwiseImageKernel&) = default;
};
enum class ImageCombineMode : std::uint8_t { Mix, SetAlpha, MultiplyAlpha };
struct ImageCombineKernel final {
    ImageCombineMode mode = ImageCombineMode::Mix;
    std::optional<CompiledScalarParameter> factor;
    render::ImageCoverageChannel channel = render::ImageCoverageChannel::Alpha;
    std::optional<ValueOutputIndex> drivenChannel = std::nullopt;
    // #361: the constant matte selector's invert half (render::MatteSelection). A driven selector
    // carries its own invert in the sampled stored value, so this stays false beside a driver.
    // Appended with a false default: no existing plan value changes meaning.
    bool invert = false;
    friend bool operator==(const ImageCombineKernel&, const ImageCombineKernel&) = default;
};
enum class CanvasImageEffect : std::uint8_t { Crop, Pad };
struct CanvasImageKernel final {
    CanvasImageEffect effect = CanvasImageEffect::Crop;
    std::array<CompiledScalarParameter, 4> margins;
    friend bool operator==(const CanvasImageKernel&, const CanvasImageKernel&) = default;
};
// Exact permutations of the stored pixel lattice, with no colour/alpha arithmetic. Odd turns
// exchange data-window extents at its top-left; the composition display canvas/PAR are retained.
struct DiscreteImageKernel final {
    bool horizontal = false;
    bool vertical = false;
    std::uint8_t quarterTurns = 0; // clockwise, canonical 0..3
    [[nodiscard]] bool identity() const noexcept {
        return !horizontal && !vertical && quarterTurns == 0;
    }
    friend bool operator==(const DiscreteImageKernel&, const DiscreteImageKernel&) = default;
};
// The resample filters of Resize, Fit and Transform 2D. Bilinear (0) interpolates the four taps
// around the sample point, shared with Layer Output. Nearest (1, #480) copies the one tap whose
// centre is nearest the sample (the upper tap on an exact half), so data, ID and Cryptomatte
// images keep their stored values exactly.
inline constexpr std::int64_t kResampleFilterBilinear = 0;
inline constexpr std::int64_t kResampleFilterNearest = 1;
// Constant output geometry. Resize stretches; Fit uses a physical-aspect-preserving fit/fill.
struct ResizeFitKernel final {
    std::int64_t width = 1920;
    std::int64_t height = 1080;
    std::int64_t pixelAspectNumerator = 1;
    std::int64_t pixelAspectDenominator = 1;
    std::int64_t filter = 0;
    // 0 = Resize (stretch), 1 = Fit (contain), 2 = Fill (cover).
    std::int64_t policy = 0;
    friend bool operator==(const ResizeFitKernel&, const ResizeFitKernel&) = default;
};
// #336 standalone Transform 2D. One inverse-mapped premultiplied bilinear resample with the Layer
// Output vocabulary: translate, pivot (an offset from the centre of the input's content bounds),
// per-axis scale and clockwise rotation in degrees, all animatable or driven. Extent chooses the
// output data window: the full transformed support, clipped to the input data window, or clipped
// to the input display window. `filter` is kResampleFilterBilinear or kResampleFilterNearest.
enum class Transform2DExtent : std::uint8_t { Transformed, Input, Display };
struct Transform2DKernel final {
    CompiledVec2Parameter translate;
    CompiledVec2Parameter pivot;
    CompiledVec2Parameter scale;
    CompiledScalarParameter rotation;
    Transform2DExtent extent = Transform2DExtent::Transformed;
    std::int64_t filter = 0;
    friend bool operator==(const Transform2DKernel&, const Transform2DKernel&) = default;
};
// Neighborhood filters operate on associated RGBA, including coverage. Radius is finite full-
// resolution pixels in [0,256]; Gaussian sigma is radius/3 and support is ceil(radius).
// Erode and Dilate (#338 Matte Erode/Dilate) are the same separable neighborhood as a morphology:
// a square of whole taps whose outermost taps weigh a fractional radius's fraction, reduced by
// minimum (Erode) or maximum (Dilate) instead of a weighted sum. They are Coverage only.
enum class SeparableBlurKind : std::uint8_t { Gaussian, Box, Erode, Dilate };
enum class BlurEdgeMode : std::uint8_t { Transparent, Clamp };
// Rgba filters every associated channel (Gaussian Blur, Box Blur). Coverage (#337 Matte Blur)
// filters only alpha and writes the result to every channel, the ADR 0028 Mask transport.
enum class BlurChannels : std::uint8_t { Rgba, Coverage };
struct SeparableBlurKernel final {
    SeparableBlurKind kind = SeparableBlurKind::Gaussian;
    CompiledScalarParameter radius;
    BlurEdgeMode edge = BlurEdgeMode::Transparent;
    BlurChannels channels = BlurChannels::Rgba;
    friend bool operator==(const SeparableBlurKernel&, const SeparableBlurKernel&) = default;
};
// ADR 0028 layer masks. A path parameter is a constant or an inline Path curve.
struct CompiledPathParameter final {
    document::ParameterId id;
    std::variant<document::PathValue, CompiledPathCurve> source;
    friend bool operator==(const CompiledPathParameter&, const CompiledPathParameter&) = default;
};
struct CompiledLayerMask final {
    document::LayerMaskId id;
    document::LayerMaskMode mode = document::LayerMaskMode::Add;
    bool inverted = false;
    CompiledPathParameter path;
    CompiledScalarParameter feather;
    CompiledScalarParameter expansion;
    CompiledScalarParameter opacity;
    friend bool operator==(const CompiledLayerMask&, const CompiledLayerMask&) = default;
};
// Multiplies the input, in premultiplied space, by the masks' accumulated coverage over the input's
// own pixel space (the layer's content space). The data window is unchanged. Every math detail is
// in docs/decisions/0028-layer-masks-and-track-mattes.md section 2.
struct LayerMaskKernel final {
    std::vector<CompiledLayerMask> masks;
    friend bool operator==(const LayerMaskKernel&, const LayerMaskKernel&) = default;
};
// #338 Luma Key: an Image to an ADR 0028 Mask. The Rec. 709 luma of the straight colour is ramped
// from `low` (zero) to `high` (full) and scaled by the pixel's alpha, so a transparent pixel has no
// coverage under either polarity; `invert` reverses the ramp. `high <= low` is a hard key at `low`.
struct LumaKeyKernel final {
    CompiledScalarParameter low;
    CompiledScalarParameter high;
    bool invert = false;
    friend bool operator==(const LumaKeyKernel&, const LumaKeyKernel&) = default;
};
// #340 Levels, Curves and Hue/Saturation: per-channel grades with no colour transform, so a Raw
// data pass is graded exactly as stored. Channels are the RGB master (applied to red, green and
// blue first), then red, green, blue and alpha. Levels and Curves work on straight colour and
// re-associate by the graded alpha; Hue/Saturation is a linear RGB matrix over associated colour.
// The prepared arithmetic is in color_grade_image_effect.hpp (CPU reference and GPU program).
struct LevelsChannelParameters final {
    CompiledScalarParameter inputBlack;
    CompiledScalarParameter inputWhite;
    CompiledScalarParameter gamma;
    CompiledScalarParameter outputBlack;
    CompiledScalarParameter outputWhite;
    friend bool operator==(const LevelsChannelParameters&,
                           const LevelsChannelParameters&) = default;
};
struct LevelsKernel final {
    std::array<LevelsChannelParameters, 5> channels;
    // Limits each channel's result to [0, 1]. Alpha is always within [0, 1].
    bool clamp = false;
    friend bool operator==(const LevelsKernel&, const LevelsKernel&) = default;
};
struct CurvesKernel final {
    // #493: the nine curves in document::kGradeCurves order (master, red, green, blue, alpha, Hue
    // vs Hue, Hue vs Saturation, Hue vs Luma, Luma vs Saturation), each a point list held as a
    // constant Path or a whole-value Path curve (bloom/runtime/grade_curve.hpp).
    std::array<CompiledPathParameter, 9> curves;
    bool clamp = false;
    friend bool operator==(const CurvesKernel&, const CurvesKernel&) = default;
};
struct HueSaturationKernel final {
    CompiledScalarParameter hue;        // degrees
    CompiledScalarParameter saturation; // factor, 1 = unchanged
    friend bool operator==(const HueSaturationKernel&, const HueSaturationKernel&) = default;
};
// #494 Grade: Resolve-style primaries on straight colour with alpha unchanged and no colour
// transform. `values` are the 22 operands in document::kGradeWheels order (each wheel's master,
// red, green, blue), then the document::kGradePrimaries order (temperature, tint, contrast, pivot,
// saturation, exposure). The formulas are in color_grade_image_effect.hpp.
struct GradeKernel final {
    std::array<CompiledScalarParameter, 22> values;
    friend bool operator==(const GradeKernel&, const GradeKernel&) = default;
};
// #479 Separate Channels and Combine Channels move STORED channel values (render::
// storedImageChannel) between images with no colour transform, no division by alpha and no clamp
// except alpha's own [0, 1], so a Raw data pass and HDR or signed values travel unchanged.
// A slot of a Channel Select reads one input channel or a constant.
enum class ImageChannelSource : std::uint8_t { Red, Green, Blue, Alpha, Luma, Zero, One };
// One input. Output component k (red, green, blue, alpha) is sources[k] of the input pixel; alpha
// is limited to [0, 1] and `premultiply` then multiplies red, green and blue by it. A Separate
// Channels output is {c, c, c, One}; a Combine Channels starts from one of these on its first
// connected input. The identity mapping without premultiply aliases its input.
struct ChannelSelectKernel final {
    std::array<ImageChannelSource, 4> sources{ImageChannelSource::Red, ImageChannelSource::Green,
                                              ImageChannelSource::Blue, ImageChannelSource::Alpha};
    bool premultiply = false;
    friend bool operator==(const ChannelSelectKernel&, const ChannelSelectKernel&) = default;
};
// Two inputs: the base (`input`, which owns every window) with component `slot` (0 red .. 3 alpha)
// set to the `source` channel of `secondaryInput` at the same global pixel (zero outside its data
// window, or when it has no image). Alpha is limited to [0, 1]; `premultiply` as above. `source`
// is never Zero or One (an unconnected Combine Channels input lowers to a Channel Select constant).
struct ChannelCombineKernel final {
    std::uint8_t slot = 0;
    ImageChannelSource source = ImageChannelSource::Red;
    bool premultiply = false;
    friend bool operator==(const ChannelCombineKernel&, const ChannelCombineKernel&) = default;
};
// #506 Glow Streak: one pass of a directional (ray) blur over associated RGBA. Output pixel c is
// the normalized sum of four taps along the ray, tap i read at c - i * spacing * (cos a, sin a)
// (a = `angle`, degrees clockwise from screen right on the y-down canvas) with weight decay^i, each
// tap a bilinear sample of the input (transparent outside its data window), so light streaks away
// from its source along +a. The data window grows by the reach in that direction. Chained passes
// with spacings 4^p multiply into one long exponential streak (directional_blur_image_effect.hpp).
struct DirectionalBlurKernel final {
    CompiledScalarParameter angle;
    CompiledScalarParameter spacing; // full-resolution pixels between taps, [0, 1024]
    double decay = 1.0;              // each tap's weight over the one before it, (0, 1]
    friend bool operator==(const DirectionalBlurKernel&, const DirectionalBlurKernel&) = default;
};
// #506 Glow highlight roll-off: associated colour c becomes c / (1 + amount * max(L, 0)), L its
// Rec. 709 luma, alpha unchanged: one hue-preserving curve that compresses bright glow towards
// 1 / amount and leaves dim glow nearly unchanged. Amount 0 is the identity. A grade kind
// (color_grade_image_effect.hpp).
struct HighlightRollOffKernel final {
    CompiledScalarParameter amount;
    friend bool operator==(const HighlightRollOffKernel&, const HighlightRollOffKernel&) = default;
};
// #503 Alpha Association over premultiplied (process) RGBA, alpha always in [0, 1]: Unmult takes
// alpha from the image over black (its brightest channel) or, with `white`, over white; Premultiply
// multiplies RGB by alpha; Unpremultiply divides it above document::kAlphaAssociationEpsilon;
// Remove Color Matting subtracts (1 - alpha) * background. The background is used by Remove Color
// Matting only (the compiler lowers a constant black, with no identity, for the other modes). A
// grade kind (color_grade_image_effect.hpp); docs/architecture/image-filters.md has the formulas.
enum class AlphaAssociationMode : std::uint8_t {
    Unmult,
    Premultiply,
    Unpremultiply,
    RemoveMatting
};
struct AlphaAssociationKernel final {
    AlphaAssociationMode mode = AlphaAssociationMode::Unmult;
    bool white = false;
    CompiledColorParameter background{{}, core::Color4d{0.0, 0.0, 0.0, 1.0}};
    friend bool operator==(const AlphaAssociationKernel&, const AlphaAssociationKernel&) = default;
};
// #504 Colorize over the straight colour of premultiplied (process) RGBA, alpha unchanged: Fill
// replaces the colour with `color`; Tint maps Rec. 709 luma L linearly from `shadows` to
// `highlights` (unclamped, so HDR extends the line); Tritone and Gradient Map take a position t (L,
// or with Gradient Map the `source` channel, times `repetitions` plus `phase`, clamped, repeated or
// mirrored into [0, 1]) through the ramp shadows, midtones at `midpoint`, highlights; Black & White
// gives min + (mid - min) * secondary weight + (max - mid) * primary weight. `amount` mixes the
// result with the input's straight colour. The compiler lowers only the operands the mode reads;
// the others are constants without an identity. A grade kind (color_grade_image_effect.hpp);
// docs/architecture/image-filters.md, "Colorize", has the formulas.
enum class ColorizeMode : std::uint8_t { Fill, Tint, Tritone, GradientMap, BlackAndWhite };
enum class ColorizeSource : std::uint8_t { Luma, Red, Green, Blue, Alpha };
enum class ColorizeCycle : std::uint8_t { Clamp, Repeat, Mirror };
struct ColorizeKernel final {
    ColorizeMode mode = ColorizeMode::Tint;
    ColorizeSource source = ColorizeSource::Luma;
    ColorizeCycle cycle = ColorizeCycle::Clamp;
    CompiledScalarParameter amount{{}, 1.0};
    CompiledColorParameter color{{}, core::Color4d{1.0, 1.0, 1.0, 1.0}};
    CompiledColorParameter shadows{{}, core::Color4d{0.0, 0.0, 0.0, 1.0}};
    CompiledColorParameter midtones{{}, core::Color4d{0.24, 0.17, 0.1, 1.0}};
    CompiledColorParameter highlights{{}, core::Color4d{1.0, 1.0, 1.0, 1.0}};
    CompiledScalarParameter midpoint{{}, 0.18};
    CompiledScalarParameter repetitions{{}, 1.0};
    CompiledScalarParameter phase{{}, 0.0};
    // Reds, yellows, greens, cyans, blues, magentas.
    std::array<CompiledScalarParameter, 6> weights{
        CompiledScalarParameter{{}, 0.4}, CompiledScalarParameter{{}, 0.6},
        CompiledScalarParameter{{}, 0.4}, CompiledScalarParameter{{}, 0.6},
        CompiledScalarParameter{{}, 0.2}, CompiledScalarParameter{{}, 0.8}};
    // #541: Gradient Map's gradient. Lowered only for a Gradient Map whose node binds one; a node
    // made before the gradient existed maps through the shadows, midtones, highlights ramp.
    std::optional<CompiledGradientParameter> gradient;
    friend bool operator==(const ColorizeKernel&, const ColorizeKernel&) = default;
};
// #505 Vignette over premultiplied (process) RGBA, alpha unchanged: every pixel's colour is scaled
// by a gain from a mask of its distance from the vignette's centre, measured in the half-axes of
// the input's display window (so the falloff follows the frame at any resolution). Multiply gives
// 1 - amount * mask, Exposure 2^(exposure * mask); `highlightProtection` moves the gain back
// towards 1 for straight luma from 0.18 up to 1.0. `center` is an offset in full-resolution pixels
// from the display window's centre. The compiler lowers only the strength the mode reads (the
// other is a constant without an identity). A grade kind whose prepared table gains the input's
// geometry where it is applied (color_grade_image_effect.hpp); docs/architecture/image-filters.md,
// "Vignette", has the formulas.
enum class VignetteShape : std::uint8_t { Elliptical, Rectangular };
enum class VignetteMode : std::uint8_t { Multiply, Exposure };
struct VignetteKernel final {
    VignetteShape shape = VignetteShape::Elliptical;
    VignetteMode mode = VignetteMode::Multiply;
    CompiledScalarParameter amount{{}, 0.5};
    CompiledScalarParameter exposure{{}, -1.0};
    CompiledScalarParameter midpoint{{}, 0.5};
    CompiledScalarParameter roundness{{}, 0.0};
    CompiledScalarParameter feather{{}, 0.5};
    CompiledScalarParameter highlightProtection{{}, 0.0};
    CompiledVec2Parameter center{{}, document::Vec2d{}};
    friend bool operator==(const VignetteKernel&, const VignetteKernel&) = default;
};
// #525 Fields: one instruction of a field program (bloom/core/field_program.hpp). `op` is a
// core::field::FieldOp; `output` and `inputs` are registers (-1 an absent input). `choices` are the
// constant choices the operation's encoder reads, and the operands its animatable values, sampled
// per frame in this order: the scalars, the vectors, the curve, the ramp. `slot` is the image slot
// an Image Field or mask Shape Distance reads (CompiledImageEffect::fieldImages), -1 for none.
// runtime/field_raster.hpp has each operation's operand order.
struct FieldInstruction final {
    std::uint8_t op = 0;
    std::int8_t output = 0;
    std::array<std::int8_t, 4> inputs{-1, -1, -1, -1};
    std::vector<std::int64_t> choices{};
    std::vector<CompiledScalarParameter> scalars{};
    std::vector<CompiledVec2Parameter> vectors{};
    std::optional<CompiledPathParameter> curve{};
    std::optional<CompiledGradientParameter> ramp{};
    std::int8_t slot = -1;
    friend bool operator==(const FieldInstruction&, const FieldInstruction&) = default;
};
// The program, its result register and how its value becomes premultiplied RGBA
// (core::field::FieldResultKind and FieldOutputMode).
struct FieldProgram final {
    std::vector<FieldInstruction> instructions;
    std::uint8_t result = 0;
    std::uint8_t kind = 0;
    std::uint8_t mode = 0;
    // The composition's duration in seconds: a looping Noise's default loop length.
    double compositionSeconds = 0.0;
    // The program reads the clock time itself (a looping Noise, a moving Pattern), so it is time
    // dependent even with constant operands.
    bool usesTime = false;
    friend bool operator==(const FieldProgram&, const FieldProgram&) = default;
};
// #525: a field rasterized over its input's data window at the evaluation resolution, every pixel
// replaced (the input supplies only its windows). A grade kind whose prepared table gains the
// raster's geometry where it is applied (color_grade_image_effect.hpp); its image slots are
// CompiledImageEffect::fieldImages. docs/architecture/fields.md is the specification.
struct FieldRasterKernel final {
    FieldProgram program;
    // 1, 4 or 16 sub-samples a pixel.
    std::uint32_t samples = 1;
    friend bool operator==(const FieldRasterKernel&, const FieldRasterKernel&) = default;
};
// #525 Shape Distance from a Mask: the exact signed Euclidean distance transform of the input's
// coverage (alpha at 1/2), in evaluation pixels in every channel (outside positive), over the
// input's data window grown by `reach` composition pixels, each distance limited to that reach plus
// one pixel. Felzenszwalb and Huttenlocher's separable transform on the CPU; on the GPU the same
// exact minimum by a bounded search (docs/architecture/fields.md, "Mask distance").
struct MaskDistanceKernel final {
    double reach = 0.0;
    friend bool operator==(const MaskDistanceKernel&, const MaskDistanceKernel&) = default;
};

// #483 Depth effects (docs/architecture/image-filters.md, "Depth effects"). A depth map reads one
// channel of its input as depth and gives, per pixel, either Z Defocus's signed circle of confusion
// (in evaluation pixels: negative nearer than the focus, positive farther; the image (c, 0, 0,
// min(|c| / R, 1)) with R the widest blur) or Depth Fog's fog factor (the Mask transport). A grade
// kind whose prepared table gains the input's geometry where it is applied; the Choice values are
// document::depth_parameters' stored values.
enum class DepthMapOutput : std::uint8_t { CircleOfConfusion, Fog };
struct DepthMapKernel final {
    DepthMapOutput output = DepthMapOutput::CircleOfConfusion;
    // 0 R, 1 G, 2 B (each unpremultiplied), 3 A.
    std::uint8_t channel = 0;
    std::int64_t interpretation = 0;
    bool invert = false;
    std::int64_t mistFalloff = 0;
    std::int64_t background = 0;
    // Z Defocus.
    std::int64_t mode = 0;
    std::int64_t focusFrom = 0;
    std::int64_t blurRegion = 0;
    std::int64_t units = 0;
    std::int64_t edgeExpand = 0;
    // Depth Fog.
    std::int64_t falloff = 0;
    CompiledScalarParameter nearDistance{{}, 0.0};
    CompiledScalarParameter farDistance{{}, 1.0};
    CompiledScalarParameter focusDistance{{}, 10.0};
    CompiledVec2Parameter focusPoint{{}, document::Vec2d{}};
    CompiledScalarParameter depthOfField{{}, 0.0};
    CompiledScalarParameter size{{}, 16.0};
    CompiledScalarParameter maxSize{{}, 64.0};
    CompiledScalarParameter focalLength{{}, 50.0};
    CompiledScalarParameter sensorWidth{{}, 36.0};
    CompiledScalarParameter fStop{{}, 2.8};
    CompiledScalarParameter amount{{}, 1.0};
    CompiledScalarParameter start{{}, 0.0};
    CompiledScalarParameter end{{}, 1.0};
    CompiledScalarParameter density{{}, 1.0};
    std::optional<CompiledPathParameter> curve{};
    friend bool operator==(const DepthMapKernel&, const DepthMapKernel&) = default;
};
// #483 Z Defocus's gather: a variable-radius lens blur of the input by a circle of confusion map
// (absent in Uniform mode, where every pixel's radius is `size`). Its slots: the tile maximum (a
// colour-carrying Dilate of the map: the neighbourhood maximum in alpha, each pixel's own signed
// circle of confusion in red), the input box-prefiltered at the widest ring spacing and the bokeh
// image. `slotImages` index CompiledImageEffect::fieldImages (-1 absent) in that order. The output
// is the input's data window grown by the widest reach.
inline constexpr std::size_t kDefocusTileSlot = 0;
inline constexpr std::size_t kDefocusPrefilterSlot = 1;
inline constexpr std::size_t kDefocusBokehSlot = 2;
inline constexpr std::size_t kDefocusSlots = 3;
struct DefocusKernel final {
    std::int64_t mode = 0;
    std::int64_t shape = 0;
    std::int64_t blades = 6;
    std::int64_t quality = 1;
    std::array<std::int8_t, kDefocusSlots> slotImages{-1, -1, -1};
    CompiledScalarParameter size{{}, 16.0};
    CompiledScalarParameter maxSize{{}, 64.0};
    CompiledScalarParameter rotation{{}, 0.0};
    CompiledScalarParameter roundness{{}, 0.0};
    CompiledScalarParameter highlightGain{{}, 0.0};
    CompiledScalarParameter highlightThreshold{{}, 1.0};
    CompiledScalarParameter chromatic{{}, 0.0};
    friend bool operator==(const DefocusKernel&, const DefocusKernel&) = default;
};

// #507 Lens Flare (docs/architecture/image-filters.md, "Lens Flare"): adds an analytic element stack
// (glow, streaks, ghosts, a ring, iris ghosts, a hoop and an anamorphic streak) to the input, over
// the input's data window joined with its display window. `values` holds the document's Scalars in
// table order (document::lensFlareScalarIndex). The input's brightness at the light and the
// occlusion matte (CompiledImageEffect::fieldImages, at most one image) are probed once a frame over
// the light's sample disc. `occlusionChannel` is the matte selector's stored value; a Mask's is
// lowered to its coverage, keeping the selector's invert.
struct LensFlareKernel final {
    std::int64_t lightMode = 0;
    std::int64_t streakCount = 3;
    std::int64_t ghostCount = 6;
    std::int64_t seed = 1;
    std::int64_t irisCount = 4;
    std::int64_t irisBlades = 6;
    std::int64_t occlusionChannel = 0;
    CompiledVec2Parameter position{{}, document::Vec2d{}};
    CompiledColorParameter color{{}, core::Color4d{1.0, 1.0, 1.0, 1.0}};
    CompiledColorParameter anamorphicColor{{}, core::Color4d{1.0, 1.0, 1.0, 1.0}};
    std::array<CompiledScalarParameter, document::kLensFlareScalarCount> values{};
    friend bool operator==(const LensFlareKernel&, const LensFlareKernel&) = default;
};

// #486 Film Grain (docs/architecture/image-filters.md, "Film Grain"): adds seeded grain to the input's
// colour in proportion to its alpha, each channel's amplitude and a response to the pixel's straight
// luma. `values` holds the document's Scalars in table order (document::filmGrainScalarIndex). Every
// Frame draws from the operation clock's frame, so the operation varies with time; Held does not.
// `monochrome` gives every channel the red channel's pattern, each at its own amplitude.
// #565: `mode` Sensor Noise replaces the luma response with a shadow weighting and adds chroma
// blotches and a fixed pattern; the values the mode does not read keep their defaults.
struct FilmGrainKernel final {
    std::int64_t animation = document::kFilmGrainEveryFrame;
    std::int64_t seed = 1;
    bool monochrome = false;
    std::int64_t mode = document::kFilmGrainFilm;
    std::array<CompiledScalarParameter, document::kFilmGrainScalarCount> values{};
    // Whether the picture follows the frame: Every Frame draws anew for each, except a Sensor Noise
    // whose grain and blotches are constant zero, which leaves only the fixed pattern (the same on
    // every frame). An animated or driven amount counts as not zero, so it still varies.
    [[nodiscard]] bool drawsPerFrame() const {
        if (animation == document::kFilmGrainHeld)
            return false;
        if (mode != document::kFilmGrainSensorNoise)
            return true;
        const auto zero = [this](const std::string_view role) {
            const auto* constant =
                std::get_if<double>(&values[document::filmGrainScalarIndex(role)].source);
            return constant != nullptr && *constant == 0.0;
        };
        const bool grainless = zero("intensity") || (zero("redAmount") && zero("greenAmount") &&
                                                     zero("blueAmount"));
        const bool blotchless = zero("intensity") || zero("chromaBlotches");
        return !(grainless && blotchless);
    }
    friend bool operator==(const FilmGrainKernel&, const FilmGrainKernel&) = default;
};

// #484 Motion blur (docs/architecture/motion-blur.md). One temporal-sampling engine shared by every
// motion-blur node: the shutter model below, a time sample (an input evaluated at a sub-frame or
// neighbour-frame time), the accumulator that sums time samples, the path sampler (Directional,
// Spin, Zoom and Transform 2D blur), the motion-vector stages and the optical flow solver. Every
// pixel stage runs the one motion kernel (motion_kernel.glsl) over an atlas on the CPU and the
// GPU alike (motion_blur_image_effect.hpp).
//
// The shutter, constant per node. `shutter` is the open time in frames (180 degrees = 0.5);
// `offset` (document::kShutterOffset*) places it Centered on the frame, from its Start, ending at
// its End, or (Custom) opening at `phase` frames; `curve` (document::kShutterCurve*) weights the
// open time Box, Triangle, Gaussian or by `customCurve` (a Curves point list over [0, 1]).
// `quality` (document::kMotionQuality*) picks the sample count: Draft 4, Standard 16, High 64, or
// Final, adaptive from the widest displacement to at most a pixel per sample within [minSamples,
// maxSamples]. `jitter` offsets each pixel's samples within their strata by a hash of the node,
// the frame and the pixel. `maxBlur` limits the blur length in full-resolution pixels.
// `rate` converts shutter frames to seconds.
struct CompiledShutter final {
    double shutter = 0.5;
    std::int64_t offset = 0;
    double phase = 0.0;
    std::int64_t curve = 0;
    document::PathValue customCurve{};
    std::int64_t quality = 3;
    std::int64_t minSamples = 8;
    std::int64_t maxSamples = 64;
    bool jitter = false;
    double maxBlur = 512.0;
    // The composition's frame rate: a frame of shutter time in seconds.
    document::FrameRate rate = document::FrameRate::framesPerSecond24();
    friend bool operator==(const CompiledShutter&, const CompiledShutter&) = default;
};
// The pixels of `input` evaluated on a time-offset clock: this operation's clock time, posterized
// first to `posterizeNumerator / posterizeDenominator` frames a second when the numerator is
// positive (Echo's Posterize Time), plus `offset`. With `holdFootage` (Motion Blur's Hold), an
// operation whose only time dependence is footage keeps the frame's own time. The effect itself is
// the identity: its pixels are its input's at that time (plan_time_scopes.hpp).
struct TimeSampleKernel final {
    core::RationalTime offset{};
    std::int64_t posterizeNumerator = 0;
    std::int64_t posterizeDenominator = 1;
    bool holdFootage = false;
    // #563 Codec Degrade's look-back: with `gopLength` positive the sample is on the frame
    // `keyframe + max(min(gopStep, position) - gopLag, 0)`, where `keyframe` is the last multiple of
    // `gopLength` at or before this clock's frame and `position` the frames since it. The offset and
    // the posterize fields are then unused.
    std::int64_t gopLength = 0;
    std::int64_t gopStep = 0;
    std::int64_t gopLag = 0;
    // #566 the look-back window (shared by every node that reads the frames before its own): with
    // `lookBack` set the sample is on the frame `max(frame - lagFrames, 0)` of the `rate` grid, this
    // clock's frame counted as a Time node's frame is. A sample therefore never reads before the
    // composition's first frame, and the same source time is shared by every frame whose window
    // reaches it. The offset, the posterize fields and the group of pictures are then unused.
    bool lookBack = false;
    std::int64_t lagFrames = 0;
    // Frame Rate Hold's irregular cadence: with `cadenceChance` positive (in thousandths) a held
    // frame, counted on the posterize grid, is a repeat of the one before it when its seeded draw
    // falls under the chance and the previous frame was not itself a repeat; the source frame that
    // would have followed the repeated one is then dropped. The draw is a pure function of
    // (`cadenceSeed`, held frame), so frames holding the same source time share one result.
    std::int64_t cadenceChance = 0;
    std::int64_t cadenceSeed = 0;
    friend bool operator==(const TimeSampleKernel&, const TimeSampleKernel&) = default;
};
// Two images combined over the union of their windows, premultiplied and HDR-safe: `input` (the
// accumulator) times `inputWeight` and `secondaryInput` (a time sample) times `sampleWeight` by
// the operator (sum, per-channel maximum or minimum, screen, the sample over or under the
// accumulator); alpha is limited to [0, 1]. A chain of them sums a node's time samples.
enum class AccumulateOperator : std::uint8_t { Add, Maximum, Minimum, Screen, Over, Under };
struct TemporalAccumulateKernel final {
    AccumulateOperator op = AccumulateOperator::Add;
    double inputWeight = 1.0;
    double sampleWeight = 1.0;
    friend bool operator==(const TemporalAccumulateKernel&,
                           const TemporalAccumulateKernel&) = default;
};
// The path sampler: each output pixel averages the input along a path sampled across the shutter
// (bilinear, or nearest with Transform 2D's Nearest filter). Directional moves by `length` pixels
// along `angle` (degrees clockwise from screen right); Spin turns by `amount` degrees and Zoom
// scales by 1 + `amount` about `centre` (an offset in full-resolution pixels from the display
// window's centre); Transform resamples through `transform` sampled at each sub-frame time.
enum class MotionPathMode : std::uint8_t { Directional, Spin, Zoom, Transform };
struct MotionPathBlurKernel final {
    MotionPathMode mode = MotionPathMode::Directional;
    CompiledShutter shutter{};
    CompiledScalarParameter angle{{}, 0.0};
    CompiledScalarParameter length{{}, 0.0};
    CompiledVec2Parameter centre{{}, document::Vec2d{}};
    CompiledScalarParameter amount{{}, 0.0};
    std::optional<Transform2DKernel> transform{};
    std::uint32_t seed = 0;
    friend bool operator==(const MotionPathBlurKernel&, const MotionPathBlurKernel&) = default;
};
// How a motion-vector pass reads (the renderer convention): component v of a vector is
// (stored * scale + offset) in `units` (0 full-resolution pixels, 1 normalised device coordinates,
// -1 to 1 across the frame, 2 evaluation pixels: Bloom's own optical flow), y up when `flipY`; `layout` 0 one vector (the path linear), 1 the forward vector and
// a backward one (to the previous frame), 2 the motion from the previous frame and the motion to
// the next (in that order); `alphaValid` reads anything under a full third channel of the forward
// image (the vector image's alpha) as no vector.
struct CompiledVectorDecode final {
    double scale = 1.0;
    double offset = 0.0;
    bool flipY = false;
    std::int64_t layout = 0;
    std::int64_t units = 0;
    bool alphaValid = false;
    friend bool operator==(const CompiledVectorDecode&, const CompiledVectorDecode&) = default;
};
// The images a vector stage reads beside its input, in VectorBlurKernel::slotImages order.
inline constexpr std::size_t kVectorForwardSlot = 0;
inline constexpr std::size_t kVectorBackwardSlot = 1;
inline constexpr std::size_t kVectorTileSlot = 2;
inline constexpr std::size_t kVectorDepthSlot = 3;
inline constexpr std::size_t kVectorMatteSlot = 4;
inline constexpr std::size_t kVectorNextSlot = 5;
inline constexpr std::size_t kVectorNextFlowSlot = 6;
inline constexpr std::size_t kVectorPreviousSlot = 7;
inline constexpr std::size_t kVectorPreviousFlowSlot = 8;
inline constexpr std::size_t kVectorConsistencySlot = 9;
// (Consistency reads a second flow's flow back from kVectorPreviousFlowSlot.)
inline constexpr std::size_t kVectorNextConsistencySlot = 10;
inline constexpr std::size_t kVectorPreviousConsistencySlot = 11;
inline constexpr std::size_t kVectorNextMatteSlot = 12;
inline constexpr std::size_t kVectorPreviousMatteSlot = 13;
inline constexpr std::size_t kVectorSlots = 14;
// The motion-vector stages. Gather is McGuire et al. 2012's reconstruction filter with Guertin et
// al. 2014's tile neighbourhood and jitter (the tiles are its own first pass); Splat the soft z-ordered forward splat of the colour input
// (Niklaus and Liu 2020) for the layered algorithm and optical-flow blur, with the neighbour
// frames blended by visibility (Jiang et al. 2018); Fill Baker et al. 2011's outside-in fill of
// the input where the matte slot's coverage reaches `threshold`; TemporalFill (TemporalFillMask)
// the background behind the foreground from the neighbour frames (the pixels still unfilled);
// Consistency Sundaram et al. 2010's forward-backward check of the input flow against the
// forward slot (the other frame's flow back), the Mask of consistency (`layer` 1: of occlusion), or
// with the backward slot and its flow back in the previous-flow slot the pair as data.
enum class VectorBlurStage : std::uint8_t {
    Gather,
    Splat,
    Fill,
    TemporalFill,
    TemporalFillMask,
    Consistency
};
struct VectorBlurKernel final {
    VectorBlurStage stage = VectorBlurStage::Gather;
    CompiledVectorDecode decode{};
    CompiledShutter shutter{};
    bool curved = false;
    // The depth slot's channel (0 R .. 3 A) and McGuire et al.'s soft depth extent in its units;
    // `zMode` (Splat): 0 no ordering, 1 nearer depth first, 2 brightness constancy.
    std::int64_t depthChannel = 0;
    double softZ = 1.0;
    std::int64_t zMode = 0;
    // Splat: 0 every pixel, 1 the matte's foreground, 2 its background.
    std::int64_t layer = 0;
    // 0 transparent, 1 the edge extended, 2 wrapped horizontally (a 360 frame).
    std::int64_t border = 0;
    double brightness = 20.0;
    double threshold = 0.5;
    std::array<std::int8_t, kVectorSlots> slotImages{-1, -1, -1, -1, -1, -1, -1,
                                                     -1, -1, -1, -1, -1, -1, -1};
    std::uint32_t seed = 0;
    friend bool operator==(const VectorBlurKernel&, const VectorBlurKernel&) = default;
};
// Dense optical flow from the input frame to the frame in slot 0, in evaluation pixels with y down
// as [u, v, 0, 1] over the union of both frames' windows. `method` 0 is DIS (Kroeger et al. 2016),
// 1 TV-L1 (Zach et al. 2007, as Sanchez Perez et al.'s IPOL 2013 algorithm); `quality`
// (document::kMotionQuality*) fixes the pyramid, warps and iterations; `layer` 1 (2) weights the
// data term by the matte in slot 1 (by its complement), Wang and Adelson's per-layer motion.
// CutDetect gives the Mask of 1 everywhere when the mean residual of the flow in slot 2 between the
// input and slot 0 exceeds `cutThreshold` grey levels (a diagnostic only).
enum class OpticalFlowStage : std::uint8_t { Solve, CutDetect };
struct OpticalFlowKernel final {
    OpticalFlowStage stage = OpticalFlowStage::Solve;
    std::int64_t method = 1;
    std::int64_t quality = 1;
    std::int64_t border = 1;
    std::int64_t layer = 0;
    double cutThreshold = 24.0;
    std::array<std::int8_t, 3> slotImages{-1, -1, -1};
    friend bool operator==(const OpticalFlowKernel&, const OpticalFlowKernel&) = default;
};

// #486 Sharpen (docs/architecture/image-filters.md, "Sharpen"): an unsharp mask over the input and its
// blur, CompiledImageEffect::fieldImages' one image. The radius is the blur's, read here only so that
// no radius is an identity; its parameter's identity belongs to the blur.
struct SharpenKernel final {
    CompiledScalarParameter amount;
    CompiledScalarParameter threshold;
    CompiledScalarParameter radius;
    friend bool operator==(const SharpenKernel&, const SharpenKernel&) = default;
};

// #563 Codec Degrade (docs/architecture/image-filters.md, "Codec Degrade"): one pass of the node's
// imitation of a codec. The node lowers to a look-back chain of `gopFrames` steps, each reading the
// source frame `keyframe + min(step, position)` through time samples:
//   Encode  the input's colour as straight, display-referred, clamped values (a pointwise pass);
//   Step    the chain's step `step`: step 0 codes its Encoded input as a keyframe, a later step on or
//           before the current frame codes the Encoded frame in fieldImages[0] against its input (the
//           chain so far) moved by the block motion of the optical flow in fieldImages[1], and a
//           later step after the current frame is the exact identity;
//   Finish  the chain's last step back to premultiplied scene-linear colour.
// `values` are the quality and the persistence, operands of a Step only.
enum class CodecDegradeRole : std::uint8_t { Encode, Step, Finish };
struct CodecDegradeKernel final {
    CodecDegradeRole role = CodecDegradeRole::Encode;
    std::int64_t blockPixels = 8;
    std::int64_t gopFrames = 12;
    std::int64_t step = 0;
    bool chromaBleed = true;
    std::array<CompiledScalarParameter, document::kCodecDegradeScalarCount> values{};
    friend bool operator==(const CodecDegradeKernel&, const CodecDegradeKernel&) = default;
};

// #566 Auto Exposure (docs/architecture/image-filters.md, "Auto Exposure"): one pass of the node's
// measurement of the footage and its lagged gain. The node lowers to the look-back window of time
// samples of its input, each reduced to its mean colour, then
//   Reduce  a mean reduction by tiles: the frame to at most `grid` by `grid` tile sums of the
//           premultiplied colour, each already divided by the frame's pixel count (`grid` 64), then
//           those to one pixel (`grid` 1), the frame's mean colour;
//   Filter  the recursive filter over the window's means (the input is the newest, fieldImages the
//           older ones oldest first): one pixel holding the exposure and white balance gains in
//           stops. `values` are the node's scalars, in autoExposureScalarIndex order;
//   Apply   the input times the gains of the Filter pixel in fieldImages[0].
enum class AutoExposureRole : std::uint8_t { Reduce, Filter, Apply };
struct AutoExposureKernel final {
    AutoExposureRole role = AutoExposureRole::Reduce;
    std::int64_t grid = 1;
    std::array<CompiledScalarParameter, document::kAutoExposureScalarCount> values{};
    friend bool operator==(const AutoExposureKernel&, const AutoExposureKernel&) = default;
};

// #481 Image Math, Image Map Range and Image Clamp (docs/architecture/image-filters.md, "Image Math,
// Map Range and Clamp"): one per-pixel function of the input and its operand. `channels` is the
// node's stored choice of the channels it changes (document::kImagePixelColor ...), `straight` how
// premultiplication is treated, `clamp` a Map Range's limit to its output range and `second` that
// the operand is the pixel of the effect's second image (an Image Math with its `second` input
// connected). `values` are the node's scalars in forEachColorGradeOperand order: an arithmetic
// operation's one operand (none for Absolute or with a second image), a Map Range's input minimum,
// input maximum, output minimum and output maximum, a Clamp's minimum and maximum of red, green,
// blue and alpha.
enum class PixelMathOperation : std::uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide,
    Power,
    Minimum,
    Maximum,
    Absolute,
    MapRange,
    Clamp,
    // #349 Image Compare: A against the second image B, which is required. `compareMode` is the
    // node's stored mode (document::kImageCompareDifference ...) and `values` its gain, split
    // position, split angle in degrees and checkerboard cell size in pixels.
    Compare
};
struct PixelMathKernel final {
    PixelMathOperation operation = PixelMathOperation::Multiply;
    std::int64_t channels = document::kImagePixelColor;
    bool straight = true;
    bool clamp = false;
    bool second = false;
    std::int64_t compareMode = document::kImageCompareDifference;
    std::array<CompiledScalarParameter, 8> values{};
    friend bool operator==(const PixelMathKernel&, const PixelMathKernel&) = default;
};

// #349 Histogram (docs/architecture/image-filters.md, "Histogram"): the node lowers to three passes
// over the frame, the way Auto Exposure's reduction does.
//   Bin   cuts the frame into at most `grid` by `grid` tiles and counts, in each tile, the pixels
//         whose value falls in each of `bins` equal bins of the range `values[0]`..`values[1]`, for
//         up to four channels at once (the `channels` choice, document::kHistogram*): one pixel per
//         bin and tile, `bins` wide and one row per tile;
//   Sum   adds the tiles' counts: one pixel per bin, the counts of the whole frame;
//   Plot  draws the counts as bars on a `bins` by `height` image, `logarithmic` or not.
// `straight` says the colour is divided by alpha before it is counted.
enum class HistogramRole : std::uint8_t { Bin, Sum, Plot };
struct HistogramKernel final {
    HistogramRole role = HistogramRole::Bin;
    std::int64_t channels = document::kHistogramRgb;
    bool straight = true;
    bool logarithmic = false;
    std::int64_t bins = 256;
    std::int64_t height = 128;
    std::int64_t grid = 32;
    std::array<CompiledScalarParameter, 2> values{};
    friend bool operator==(const HistogramKernel&, const HistogramKernel&) = default;
};

// #347 Audio Waveform and Audio Spectrum (docs/architecture/media-io.md, "Audio analysis"): one pass
// over a transparent canvas of `width` by `height` pixels that draws the analysis of a window of
// the audio of `source` at the frame. The analysis (the minimum and maximum of each lane in each
// column, or the level of each frequency column) is prepared on a worker (audio_analysis.hpp) and
// reaches the pass as its table, so the pass is a pure function of that table and runs on the CPU
// reference and the GPU alike. A null `source` is an input with no audio wired: silence.
//   Waveform  values: the window in seconds and the vertical scale; `separate` draws a lane for
//             each channel instead of their mean.
//   Spectrum  values: the lowest and highest frequency in hertz and the floor and ceiling in
//             decibels; `fftSize` is the transform length and `logarithmic` the frequency axis.
// `color` is the line or bars and `background` the fill behind them, both straight colors.
enum class AudioPlotKind : std::uint8_t { Waveform, Spectrum };
struct AudioPlotKernel final {
    AudioPlotKind kind = AudioPlotKind::Waveform;
    std::shared_ptr<const AudioAnalysisSource> source;
    std::int64_t width = 1024;
    std::int64_t height = 256;
    bool separate = false;
    std::int64_t fftSize = 2048;
    bool logarithmic = true;
    std::array<CompiledScalarParameter, 4> values{};
    CompiledColorParameter color;
    CompiledColorParameter background;
    friend bool operator==(const AudioPlotKernel&, const AudioPlotKernel&) noexcept;
};

using ImageEffectKernel =
    std::variant<IdentityImageKernel, CstKernel, FileTransformKernel, PointwiseImageKernel,
                 ImageCombineKernel, CanvasImageKernel, DiscreteImageKernel, ResizeFitKernel,
                 SeparableBlurKernel, LayerMaskKernel, Transform2DKernel, LumaKeyKernel,
                 LevelsKernel, CurvesKernel, HueSaturationKernel, ChannelSelectKernel,
                 ChannelCombineKernel, GradeKernel, DirectionalBlurKernel, HighlightRollOffKernel,
                 AlphaAssociationKernel, ColorizeKernel, VignetteKernel, FieldRasterKernel,
                 MaskDistanceKernel, DepthMapKernel, DefocusKernel, LensFlareKernel,
                 FilmGrainKernel, TimeSampleKernel, TemporalAccumulateKernel,
                 MotionPathBlurKernel, VectorBlurKernel, OpticalFlowKernel, SharpenKernel,
                 CodecDegradeKernel, AutoExposureKernel, PixelMathKernel, HistogramKernel,
                 AudioPlotKernel>;

// #484: the motion kernels that run the motion kernel (a time sample is an identity).
[[nodiscard]] inline bool isMotionKernel(const ImageEffectKernel& kernel) noexcept {
    return std::holds_alternative<TemporalAccumulateKernel>(kernel) ||
           std::holds_alternative<MotionPathBlurKernel>(kernel) ||
           std::holds_alternative<VectorBlurKernel>(kernel) ||
           std::holds_alternative<OpticalFlowKernel>(kernel);
}

// #525: every operand of a field program, in instruction order and, within one, scalars, vectors,
// the curve, then the ramp.
template <typename Visit> void forEachFieldOperand(const FieldProgram& program, Visit&& visit) {
    for (const auto& instruction : program.instructions) {
        for (const auto& scalar : instruction.scalars)
            visit(scalar);
        for (const auto& vector : instruction.vectors)
            visit(vector);
        if (instruction.curve)
            visit(*instruction.curve);
        if (instruction.ramp)
            visit(*instruction.ramp);
    }
}

// #340: visits every animatable operand of a Levels, Curves, Hue/Saturation (#494: or Grade;
// #506: or Highlight Roll-off; #503: or Alpha Association; #504: or Colorize) kernel in the kernel's canonical order
// (channel-major), calling `visit(parameter)` with a CompiledScalarParameter, a
// CompiledVec2Parameter, (#493 Curves) a CompiledPathParameter, (#503 Alpha Association) a
// CompiledColorParameter or (#541 Colorize's gradient) a CompiledGradientParameter. Returns false for every other kernel. The one traversal the evaluator's
// validation, sampling, cache key and time dependence all share.
template <typename Visit>
bool forEachColorGradeOperand(const ImageEffectKernel& kernel, Visit&& visit) {
    if (const auto* levels = std::get_if<LevelsKernel>(&kernel)) {
        for (const auto& channel : levels->channels) {
            visit(channel.inputBlack);
            visit(channel.inputWhite);
            visit(channel.gamma);
            visit(channel.outputBlack);
            visit(channel.outputWhite);
        }
        return true;
    }
    if (const auto* curves = std::get_if<CurvesKernel>(&kernel)) {
        for (const auto& curve : curves->curves)
            visit(curve);
        return true;
    }
    if (const auto* hueSaturation = std::get_if<HueSaturationKernel>(&kernel)) {
        visit(hueSaturation->hue);
        visit(hueSaturation->saturation);
        return true;
    }
    if (const auto* grade = std::get_if<GradeKernel>(&kernel)) {
        for (const auto& value : grade->values)
            visit(value);
        return true;
    }
    if (const auto* rollOff = std::get_if<HighlightRollOffKernel>(&kernel)) {
        visit(rollOff->amount);
        return true;
    }
    // #503: a CompiledColorParameter.
    if (const auto* association = std::get_if<AlphaAssociationKernel>(&kernel)) {
        visit(association->background);
        return true;
    }
    // #504: amount, the four colours, midpoint, repetitions, phase, then the six weights.
    if (const auto* colorize = std::get_if<ColorizeKernel>(&kernel)) {
        visit(colorize->amount);
        visit(colorize->color);
        visit(colorize->shadows);
        visit(colorize->midtones);
        visit(colorize->highlights);
        visit(colorize->midpoint);
        visit(colorize->repetitions);
        visit(colorize->phase);
        for (const auto& weight : colorize->weights)
            visit(weight);
        // #541: then the gradient, when lowered.
        if (colorize->gradient)
            visit(*colorize->gradient);
        return true;
    }
    // #505: amount, exposure, midpoint, roundness, feather, highlight protection, then the centre
    // (a CompiledVec2Parameter).
    if (const auto* vignette = std::get_if<VignetteKernel>(&kernel)) {
        visit(vignette->amount);
        visit(vignette->exposure);
        visit(vignette->midpoint);
        visit(vignette->roundness);
        visit(vignette->feather);
        visit(vignette->highlightProtection);
        visit(vignette->center);
        return true;
    }
    // #525: the field program's operands (forEachFieldOperand).
    if (const auto* field = std::get_if<FieldRasterKernel>(&kernel)) {
        forEachFieldOperand(field->program, visit);
        return true;
    }
    // #483: near and far, then the circle of confusion's focus distance, focus point, in-focus
    // range, size, maximum size, focal length, sensor width and f-stop, or the fog's amount,
    // start, end, density and (when lowered) curve.
    if (const auto* depth = std::get_if<DepthMapKernel>(&kernel)) {
        visit(depth->nearDistance);
        visit(depth->farDistance);
        if (depth->output == DepthMapOutput::CircleOfConfusion) {
            visit(depth->focusDistance);
            visit(depth->focusPoint);
            visit(depth->depthOfField);
            visit(depth->size);
            visit(depth->maxSize);
            visit(depth->focalLength);
            visit(depth->sensorWidth);
            visit(depth->fStop);
        } else {
            visit(depth->amount);
            visit(depth->start);
            visit(depth->end);
            visit(depth->density);
            if (depth->curve)
                visit(*depth->curve);
        }
        return true;
    }
    // #483: size, maximum size, rotation, roundness, highlight gain and threshold, chromatic.
    if (const auto* defocus = std::get_if<DefocusKernel>(&kernel)) {
        visit(defocus->size);
        visit(defocus->maxSize);
        visit(defocus->rotation);
        visit(defocus->roundness);
        visit(defocus->highlightGain);
        visit(defocus->highlightThreshold);
        visit(defocus->chromatic);
        return true;
    }
    // #507: the position, the two colours, then the Scalars in table order.
    if (const auto* flare = std::get_if<LensFlareKernel>(&kernel)) {
        visit(flare->position);
        visit(flare->color);
        visit(flare->anamorphicColor);
        for (const auto& value : flare->values)
            visit(value);
        return true;
    }
    // #486: the Scalars in table order.
    if (const auto* grain = std::get_if<FilmGrainKernel>(&kernel)) {
        for (const auto& value : grain->values)
            visit(value);
        return true;
    }
    // #484: the path sampler's angle, length, centre and amount, then a Transform path's
    // translate, pivot, scale and rotation. The other motion kernels have constant operands.
    if (const auto* path = std::get_if<MotionPathBlurKernel>(&kernel)) {
        visit(path->angle);
        visit(path->length);
        visit(path->centre);
        visit(path->amount);
        if (path->transform) {
            visit(path->transform->translate);
            visit(path->transform->pivot);
            visit(path->transform->scale);
            visit(path->transform->rotation);
        }
        return true;
    }
    if (const auto* sharpen = std::get_if<SharpenKernel>(&kernel)) {
        visit(sharpen->amount);
        visit(sharpen->threshold);
        visit(sharpen->radius);
        return true;
    }
    // #563: the quality and the persistence of a step; an Encode and a Finish have no operands.
    if (const auto* codec = std::get_if<CodecDegradeKernel>(&kernel)) {
        if (codec->role == CodecDegradeRole::Step)
            for (const auto& value : codec->values)
                visit(value);
        return true;
    }
    // #566: the scalars of the filter; a reduction and an application have no operands.
    if (const auto* autoExposure = std::get_if<AutoExposureKernel>(&kernel)) {
        if (autoExposure->role == AutoExposureRole::Filter)
            for (const auto& value : autoExposure->values)
                visit(value);
        return true;
    }
    // #481: the operand an operation reads, or a Map Range's four, or a Clamp's eight.
    if (const auto* pixelMath = std::get_if<PixelMathKernel>(&kernel)) {
        std::size_t count = 0;
        if (pixelMath->operation == PixelMathOperation::Clamp)
            count = 8;
        else if (pixelMath->operation == PixelMathOperation::Compare)
            count = 4;
        else if (pixelMath->operation == PixelMathOperation::MapRange)
            count = 4;
        else if (!pixelMath->second && pixelMath->operation != PixelMathOperation::Absolute)
            count = 1;
        for (std::size_t index = 0; index < count; ++index)
            visit(pixelMath->values[index]);
        return true;
    }
    // #349: a Histogram's Bin reads the range; its Sum and Plot have no operands.
    if (const auto* histogram = std::get_if<HistogramKernel>(&kernel)) {
        if (histogram->role == HistogramRole::Bin)
            for (const auto& value : histogram->values)
                visit(value);
        return true;
    }
    // #347: the window and scale, or the range of the axes, then the two colors.
    if (const auto* plot = std::get_if<AudioPlotKernel>(&kernel)) {
        const std::size_t count = plot->kind == AudioPlotKind::Waveform ? 2 : 4;
        for (std::size_t index = 0; index < count; ++index)
            visit(plot->values[index]);
        visit(plot->color);
        visit(plot->background);
        return true;
    }
    return isMotionKernel(kernel);
}

[[nodiscard]] inline bool isColorGradeKernel(const ImageEffectKernel& kernel) noexcept {
    return std::holds_alternative<LevelsKernel>(kernel) ||
           std::holds_alternative<CurvesKernel>(kernel) ||
           std::holds_alternative<HueSaturationKernel>(kernel) ||
           std::holds_alternative<GradeKernel>(kernel) ||
           std::holds_alternative<HighlightRollOffKernel>(kernel) ||
           std::holds_alternative<AlphaAssociationKernel>(kernel) ||
           std::holds_alternative<ColorizeKernel>(kernel) ||
           std::holds_alternative<VignetteKernel>(kernel) ||
           std::holds_alternative<FieldRasterKernel>(kernel) ||
           std::holds_alternative<DepthMapKernel>(kernel) ||
           std::holds_alternative<DefocusKernel>(kernel) ||
           std::holds_alternative<LensFlareKernel>(kernel) ||
           std::holds_alternative<FilmGrainKernel>(kernel) ||
           std::holds_alternative<SharpenKernel>(kernel) ||
           std::holds_alternative<CodecDegradeKernel>(kernel) ||
           std::holds_alternative<AutoExposureKernel>(kernel) ||
           std::holds_alternative<PixelMathKernel>(kernel) ||
           std::holds_alternative<HistogramKernel>(kernel) ||
           std::holds_alternative<AudioPlotKernel>(kernel) || isMotionKernel(kernel);
}
struct CompiledImageEffect final {
    document::NodeId sourceNodeId;
    OperationIndex input;
    ImageEffectKernel kernel;
    bool bypass = false;
    bool look = false;
    // Second image/matte for flat RGBA combines. The base input owns the output data/display
    // windows; the second input is sampled by global pixel coordinate and clipped to those windows.
    std::optional<OperationIndex> secondaryInput{};
    // ADR 0028: Image Mix's optional Mask (coverage in the alpha channel of the mask transport),
    // sampled by global pixel coordinate; its per-pixel factor is factor * mask.
    std::optional<OperationIndex> maskInput{};
    // ADR 0028: this effect applies a track matte to the Layer Output at `input`; a Merge reads
    // that layer's blend mode and clock through it (see mergeEntryLayerOutput).
    bool trackMatte = false;
    // #525: a FieldRasterKernel's image slots, in slot order (an Image Field's image, a mask Shape
    // Distance's distance image), each sampled by its own data window; #483 a DefocusKernel's
    // (DefocusKernel::slotImages); #507 a LensFlareKernel's occlusion matte; #484 a VectorBlurKernel's
    // and an OpticalFlowKernel's (their slotImages); #486 a SharpenKernel's blur; #563 a Codec Degrade
    // step's Encoded source frame, then its optical flow.
    std::vector<OperationIndex> fieldImages{};
    friend bool operator==(const CompiledImageEffect&, const CompiledImageEffect&) = default;
};

struct CompiledImageSource {
    document::NodeId sourceNodeId;
    std::optional<document::AssetRecord> asset;
    std::int64_t startFrame = 0;
    std::int64_t loopMode = 0;
    std::int64_t colorSpace = 0;
    std::string inputColorSpaceId;
    bool premultiply = true;
    // ADR 0026: the OpenEXR part (empty: first) and colour layer (empty: root) of the image output.
    std::string part{};
    std::string layer{};
    friend bool operator==(const CompiledImageSource&, const CompiledImageSource&) = default;
};

// ADR 0026: a Shuffle's image output. The channel expression is resolved against the probed
// sources; the four slots name resolved channels or the constants "0" and "1". Colour mode converts
// the RGB triple from the red slot's source input colour space; Raw places the values unchanged;
// Auto (#480) is Raw when a mapped RGB channel is a data channel and Colour otherwise, and Colour on
// a data channel is refused. `associated` says the mapped RGB is already premultiplied by the mapped
// alpha. A non-finite mapped sample becomes `nonFinite` (#480). With `pass` (#480 Pass) the slots
// are derived at evaluation from the resolved set (runtime::detail::passMapping) and `mapping` is
// unused. Slots that read Image to Channel Set channels never reach this operation: the compiler
// lowers them to Channel Select and Channel Combine effects.
struct CompiledChannelShuffle {
    document::NodeId sourceNodeId;
    CompiledChannelExpression channels;
    std::array<std::string, 4> mapping{};
    std::int64_t mode = 0;
    bool associated = true;
    float nonFinite = 0.0F;
    std::optional<std::string> pass{};
    friend bool operator==(const CompiledChannelShuffle&, const CompiledChannelShuffle&) = default;
};

// #364: a Cryptomatte matte. The selected IDs' coverage from the rank pairs of `layer` in the
// channel expression, as the premultiplied white Mask (ADR 0028) in the anchor source's frame. The
// node's Image output with an input image is a MultiplyAlpha effect reading this operation.
struct CompiledCryptomatte {
    document::NodeId sourceNodeId;
    CompiledChannelExpression channels;
    std::string layer{};
    std::vector<std::string> mattes{};
    // #364 Viewer-only plans: the ID-colour preview (every object in its own colour, the selected
    // ones brightened) instead of the matte. Never set by an ordinary composition plan.
    bool preview = false;
    friend bool operator==(const CompiledCryptomatte&, const CompiledCryptomatte&) = default;
};

struct CompiledVideoSource {
    document::NodeId sourceNodeId;
    std::optional<document::AssetRecord> asset;
    std::int64_t startFrame = 0;
    std::int64_t loopMode = 0;
    std::int64_t colorSpace = 0;
    std::string inputColorSpaceId;
    friend bool operator==(const CompiledVideoSource&, const CompiledVideoSource&) = default;
};

// ADR 0024: a Layer Output's own clock and its upstream's content clock. Present only for a layer
// whose timing is not identity; the extent is already resolved (a Source extent names no node
// here). The Time Remap operand is sampled on the layer clock.
struct CompiledLayerTime final {
    core::RationalTime startTime{};
    document::LayerSpeed speed{};
    std::optional<CompiledScalarParameter> timeRemap{};
    std::optional<core::RationalTime> extentDuration{};
    document::FrameRate extentFrameRate = document::FrameRate::framesPerSecond24();
    document::LayerTimeMode beforeMode = document::LayerTimeMode::None;
    document::LayerTimeMode afterMode = document::LayerTimeMode::None;
    friend bool operator==(const CompiledLayerTime&, const CompiledLayerTime&) = default;
};

// #381: a video's audio follows its Interpret Footage rate and Loop count. Source time (after the
// node's start) is wrapped to one pass of the footage while within `loops` passes, silent after,
// then scaled by numerator/denominator (the conformed rate over the file's rate).
struct FootageAudioTiming final {
    core::RationalTime pass{};
    std::uint32_t loops = 1;
    std::int64_t numerator = 1;
    std::int64_t denominator = 1;
    friend bool operator==(const FootageAudioTiming&, const FootageAudioTiming&) = default;
};

struct CompiledAudioSource final {
    document::NodeId sourceNodeId;
    document::AssetId assetId;
    std::int64_t startFrame = 0;
    CompiledScalarParameter level;
    // #354: a video source's stored loop mode (0 Hold, 1 Loop, 2 PingPong) and its picture extent,
    // so embedded audio wraps exactly as its picture does. An audio source has neither (Hold): the
    // stream plays once.
    std::int64_t loopMode = 0;
    std::optional<TimeExtent> loopExtent{};
    // Document 1.25: which of the asset's audio streams (0-based among its audio streams) plays.
    std::uint32_t audioStream = 0;
    // #381: absent unless the video is conformed or looped.
    std::optional<FootageAudioTiming> footage{};

    friend bool operator==(const CompiledAudioSource&, const CompiledAudioSource&) = default;
};

// #569: the audio effects a layer's sound runs through, nearest the source first.
enum class AudioEffectKind : std::uint8_t { Equalizer, Compressor, AutoLevel, Mono, Hiss };

struct CompiledAudioEffect final {
    document::NodeId nodeId;
    AudioEffectKind kind = AudioEffectKind::Mono;
    // One per parameter of the node, in the node's definition order (audio_effect_nodes.hpp).
    std::vector<CompiledScalarParameter> parameters;

    friend bool operator==(const CompiledAudioEffect&, const CompiledAudioEffect&) = default;
};

struct CompiledAudioLayer final {
    document::NodeId layerOutputNodeId;
    document::LayerId layerId;
    std::size_t sourceIndex = 0;
    bool enabled = true;
    bool solo = false;
    core::RationalTime inPoint{};
    core::RationalTime outPoint{};
    // ADR 0024: the layer's own time mapping; audio follows exactly the picture mapping.
    std::optional<CompiledLayerTime> time{};
    // #569: the effect chain between the source and this layer, nearest the source first.
    std::vector<CompiledAudioEffect> effects{};

    friend bool operator==(const CompiledAudioLayer&, const CompiledAudioLayer&) = default;
};

// Audio is intentionally a sibling of the image operation chain. It carries no device, decoded
// samples, or filesystem state; the UI/audio boundary resolves the asset identity to a buffer.
struct CompiledCompositionAudioLayer final {
    CompiledCompositionSource source;
    bool enabled = true;
    bool solo = false;
    core::RationalTime inPoint{}, outPoint{};
    std::optional<CompiledLayerTime> time{};
    friend bool operator==(const CompiledCompositionAudioLayer&,
                           const CompiledCompositionAudioLayer&) = default;
};
struct CompositionAudioMix final {
    document::NodeId outputNodeId;
    std::vector<CompiledAudioSource> sources;
    std::vector<CompiledAudioLayer> layers;
    std::vector<CompiledCompositionAudioLayer> nestedLayers{};

    friend bool operator==(const CompositionAudioMix&, const CompositionAudioMix&) = default;
};

struct CompiledShape {
    document::NodeId sourceNodeId;
    document::ShapeKind kind;
    CompiledVec2Parameter size;
    double cornerRadius = 0;
    std::int64_t points = 5;
    double innerRatio = 0.5;
    document::Vec2d lineStart, lineEnd;
    document::PathValue path;
    bool fillEnabled = true;
    CompiledColorParameter fillColor;
    bool strokeEnabled = false;
    CompiledColorParameter strokeColor;
    CompiledScalarParameter strokeWidth;
    document::ShapeStrokeAlign strokeAlign = document::ShapeStrokeAlign::Center;
    document::ShapeStrokeJoin strokeJoin = document::ShapeStrokeJoin::Miter;
    document::ShapeStrokeCap strokeCap = document::ShapeStrokeCap::Butt;
    document::ShapeFillRule fillRule = document::ShapeFillRule::NonZero;
    // The fill and stroke switches may be driven by a Boolean value node: the plan then carries the
    // value-graph output each reads per frame, beside the authored constant it replaces (see
    // CompiledText::drivenContent). Resolve them with detail::resolvedFillEnabled() and
    // resolvedStrokeEnabled(), never by reading fillEnabled/strokeEnabled directly.
    document::ParameterId fillEnabledParameterId{};
    std::optional<ValueOutputIndex> drivenFillEnabled{};
    document::ParameterId strokeEnabledParameterId{};
    std::optional<ValueOutputIndex> drivenStrokeEnabled{};
    // #589: the path a Path output hands this shape per frame, in place of `path`. Resolve it with
    // detail::resolvedShapePath(), never by reading `path` directly.
    std::optional<ValueOutputIndex> drivenPath{};
    friend bool operator==(const CompiledShape&, const CompiledShape&) = default;
};

struct CompiledText {
    CompiledText() = default;
    CompiledText(document::NodeId source, document::ParameterId contentParameter,
                 std::string authoredContent, CompiledScalarParameter authoredSize,
                 CompiledColorParameter authoredColor, CompiledTextLayout authoredLayout,
                 std::optional<ValueOutputIndex> contentDriver = {},
                 render::EmbeddedFace authoredFace = render::EmbeddedFace::DejaVuSans)
        : sourceNodeId(source), contentParameterId(contentParameter),
          content(std::move(authoredContent)), size(authoredSize), color(authoredColor),
          layout(authoredLayout), drivenContent(contentDriver), face(authoredFace) {}

    document::NodeId sourceNodeId;
    document::ParameterId contentParameterId;
    std::string content;
    CompiledScalarParameter size;
    CompiledColorParameter color;
    CompiledTextLayout layout;
    // Task DRIVE-1. Present exactly when the content parameter carries a driver binding; `content`
    // is then the empty authored fallback the parameter store no longer holds a constant for, and
    // the evaluator reads the String this output produces instead.
    //
    // This is an OPTIONAL BESIDE the constant rather than a third alternative of a typed
    // CompiledStringParameter, and deliberately so. A String, an Integer and a Boolean have no
    // curve table -- none of the three interpolates -- so a typed operand for them would carry
    // exactly these two alternatives and nothing more, while changing `content` from a std::string
    // into one WOULD be "a field's meaning changing" in the sense the semantics-version comment at
    // the top of this file describes, and would therefore move
    // kCompiledCompositionPlanSemanticsVersion and with it every process-frame semantic identity
    // digest (src/output/process_frame_semantic_identity.cpp hashes that number). A new optional
    // field appearing is the "new alternative" case that same comment names as NOT a semantics
    // change: an existing plan value still means precisely what it meant, and every pixel and
    // every digest an existing plan produces is unchanged.
    std::optional<ValueOutputIndex> drivenContent{};

    // The built-in face drawn when `font` is a built-in face. It is optional at the document binding
    // boundary for old text-source nodes, which lower to this default and therefore retain the
    // pre-FONT-1 DejaVu Sans pixels exactly.
    render::EmbeddedFace face = render::EmbeddedFace::DejaVuSans;
    render::TextFont font = render::EmbeddedFace::DejaVuSans;
    // The font ID text the node stores (docs/architecture/fonts.md). `font` is the face drawn: the
    // font itself, or the fallback when `fontMissing`.
    std::string fontId;
    bool fontMissing = false;
    // The font is downloading; `font` is the fallback until it arrives.
    bool fontPending = false;

    friend bool operator==(const CompiledText&, const CompiledText&) = default;
};

// A lowered Layer Output boundary. The six parameters appear in the registered authoring order --
// position, anchor, scale, rotation, opacity, blend mode -- and each one carries its own parameter
// identity so a diagnostic can name the exact parameter that failed. The first five are animatable,
// so each is either a resolved constant or an index into the plan's curve tables.
//
// The blend mode is non-animatable -- the schema says so, and there is no meaningful value between
// Multiply and Screen for a curve to interpolate -- so it has no curve alternative. It can still be
// DRIVEN (task DRIVE-1): an Integer node handing a layer its mode per frame is a jump between two
// named modes, which is exactly what a driver expresses and a curve cannot. It lives HERE, on the
// layer boundary that owns it, rather than on the stack entry that consumes it -- the stack entry
// is the ordering of layers, and the mode is a property of the layer.
struct CompiledLayerOutput {
    document::NodeId sourceNodeId;
    document::LayerId layerId;
    OperationIndex input;
    CompiledVec2Parameter position;
    CompiledVec2Parameter anchor;
    CompiledVec2Parameter scale;
    CompiledScalarParameter rotation;
    CompiledScalarParameter opacity;
    document::ParameterId blendModeParameterId;
    core::BlendMode blendMode = core::kDefaultBlendMode;

    core::RationalTime inPoint{};
    // Absent means full composition duration.
    std::optional<core::RationalTime> outPoint{};
    // Position places centre(local bounds) + anchor at the authored parent-space point.
    // Task DRIVE-1. The value-graph output the blend mode is driven by, when it is driven. The
    // Integer it produces is mapped through core::blendModeFromStoredValue(), the same closed
    // enumeration an authored one goes through, so a driven mode and an authored mode are the same
    // set of modes. `blendMode` beside it stays the authored constant, or the registry default when
    // the parameter holds no constant at all because it is driven. See CompiledText::drivenContent
    // for why the driver sits beside the constant rather than inside a typed operand.
    std::optional<ValueOutputIndex> drivenBlendMode{};
    // Plan v4: authored transform dependency, evaluated before this boundary.
    std::optional<OperationIndex> parent{};
    // Plan v9 (ADR 0024): absent for identity timing.
    std::optional<CompiledLayerTime> time{};
    // #381 Interpret Footage: the horizontal pixel aspect the layer's footage is shown with,
    // relative to how it decodes (the override over the file's ratio). It scales the layer's own
    // content in layer space, before its scale and rotation, like a pixel-aspect conversion; a
    // child layer inherits the layer's transform without it. 1 for everything else.
    double sourcePixelAspect = 1.0;
    // #502: scale and rotation pivot about the composition frame's centre (plus the anchor)
    // instead of the content bounds' centre, so a Merge entry's own layer at its defaults is the
    // identity whatever its content's bounds. Appended false: no existing plan changes meaning.
    bool framePivot = false;
    friend bool operator==(const CompiledLayerOutput&, const CompiledLayerOutput&) = default;
};

// Plan v3: an invalid layerId denotes a plain image with Normal blending.
// #339: a plain image entry (no layer) may instead name its blend mode explicitly; the compositing
// recipes add (Glow) or wrap (Light Wrap, Atop) an image this way. A Layer entry never carries
// one, since its mode is the Layer Output's own. Appended with an absent default, so no existing
// plan value changes meaning.
struct CompiledMergeInput {
    document::LayerSlotId slotId;
    document::LayerId layerId;
    OperationIndex input;
    std::optional<core::BlendMode> blendMode{};

    friend bool operator==(const CompiledMergeInput&, const CompiledMergeInput&) = default;
};

struct CompiledMerge {
    document::NodeId sourceNodeId;
    std::vector<CompiledMergeInput> entries;

    friend bool operator==(const CompiledMerge&, const CompiledMerge&) = default;
};

struct CompiledCompositionOutput {
    document::NodeId sourceNodeId;
    OperationIndex input;
    // ADR 0026 §7: the channel set connected to the output's `channels` input, written beside RGBA
    // by layered OpenEXR, and the comma-separated layer names to write (empty: every layer).
    // Neither affects the process image.
    std::optional<CompiledChannelExpression> channelLayers{};
    std::string layers{};

    friend bool operator==(const CompiledCompositionOutput&,
                           const CompiledCompositionOutput&) = default;
};

using CompiledOperation =
    std::variant<CompiledSolid, CompiledChecker, CompiledText, CompiledImageSource,
                 CompiledVideoSource, CompiledLayerOutput, CompiledMerge, CompiledCompositionOutput,
                 CompiledShape, CompiledCompositionSource, CompiledImageEffect,
                 CompiledChannelShuffle, CompiledCryptomatte>;

// ADR 0024 time clocks. Clock 0 is the plan's own clock (the request time). A Layer clock maps its
// parent clock through a layer's start time and speed; a Content clock (whose parent is that Layer
// clock) applies the layer's Time Remap and out-of-extent modes, and may be absent. Every
// operation evaluates its operands on operationClock(i) and is consumed on placementClock(i); the
// two differ only for a retimed Layer Output (layer clock vs. the clock it is placed in). A plan
// with no retimed layer has no clocks and is exactly the pre-v9 operation list.
// #484: an Offset clock is a TimeSampleKernel's (`layer` names the time sample): its parent's time,
// posterized, plus the sample's offset.
enum class CompiledClockKind : std::uint8_t { Plan, Layer, Content, Offset };
struct CompiledTimeClock final {
    CompiledClockKind kind = CompiledClockKind::Plan;
    std::uint32_t parent = 0;
    // The (placement-clock copy of the) Layer Output whose CompiledLayerTime defines this clock.
    OperationIndex layer = OperationIndex::fromRaw(0);
    friend bool operator==(const CompiledTimeClock&, const CompiledTimeClock&) = default;
};

// Mutable construction storage is deliberately a distinct type. Publishing a plan copies or moves
// this complete definition into private storage, so retaining or changing the definition cannot
// change evaluation or identity semantics after publication.
struct CompiledCompositionPlanDefinition final {
    document::Revision sourceRevision;
    document::ProjectId projectId;
    document::CompositionId compositionId;
    document::CompositionFormat format;
    std::vector<CompiledOperation> operations;
    OperationIndex output;
    std::vector<CompiledScalarCurve> scalarCurves{};
    std::vector<CompiledVec2Curve> vec2Curves{};
    std::vector<CompiledVec3Curve> vec3Curves{};
    std::vector<CompiledColor4Curve> color4Curves{};
    // The value graph (task S7), in topological order: every operation's operands name only earlier
    // outputs, so one linear sweep evaluates the whole of it. `valueOutputCount` is the size of the
    // flat output table the operations' runs partition, kept explicitly so the evaluator can
    // bounds- check a ValueOutputIndex without summing the operations first.
    std::vector<CompiledValueOperation> valueOperations{};
    std::size_t valueOutputCount = 0;
    std::uint32_t planSemanticsVersion = kCompiledCompositionPlanSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion = kAnimationSamplingSemanticsVersion;

    bool bypassOperationCache = false;
    CompositionAudioMix audioMix{};
    std::vector<std::shared_ptr<const CompiledCompositionPlan>> nestedPlans{};
    core::RationalTime duration{};
    // ADR 0024. Empty means every operation is on clock 0.
    std::vector<CompiledTimeClock> clocks{};
    std::vector<std::uint32_t> operationClocks{};
    std::vector<std::uint32_t> placementClocks{};

    friend bool operator==(const CompiledCompositionPlanDefinition&,
                           const CompiledCompositionPlanDefinition&) = default;
};

// Immutable after construction. Frames may retain this object through shared ownership without a
// per-frame plan copy because no public API can mutate the published fields or vector storage.
class CompiledCompositionPlan final {
  public:
    explicit CompiledCompositionPlan(CompiledCompositionPlanDefinition definition);

    CompiledCompositionPlan(const CompiledCompositionPlan&) = delete;
    CompiledCompositionPlan& operator=(const CompiledCompositionPlan&) = delete;
    CompiledCompositionPlan(CompiledCompositionPlan&&) = delete;
    CompiledCompositionPlan& operator=(CompiledCompositionPlan&&) = delete;
    ~CompiledCompositionPlan() = default;

    [[nodiscard]] std::span<const std::shared_ptr<const CompiledCompositionPlan>>
    nestedPlans() const noexcept {
        return nestedPlans_;
    }
    [[nodiscard]] core::RationalTime duration() const noexcept { return duration_; }
    // ADR 0024 clocks; clock 0 is implicit and always present.
    [[nodiscard]] std::span<const CompiledTimeClock> clocks() const& noexcept { return clocks_; }
    [[nodiscard]] std::span<const CompiledTimeClock> clocks() const&& = delete;
    [[nodiscard]] std::size_t clockCount() const noexcept {
        return clocks_.empty() ? 1 : clocks_.size();
    }
    [[nodiscard]] std::uint32_t operationClock(const OperationIndex index) const noexcept {
        return index.value() < operationClocks_.size() ? operationClocks_[index.value()] : 0;
    }
    [[nodiscard]] std::uint32_t placementClock(const OperationIndex index) const noexcept {
        return index.value() < placementClocks_.size() ? placementClocks_[index.value()] : 0;
    }
    [[nodiscard]] bool bypassOperationCache() const noexcept { return bypassOperationCache_; }
    [[nodiscard]] bool operationTimeDependent(OperationIndex index) const noexcept {
        return index.value() >= operationTimeDependent_.size() ||
               operationTimeDependent_[index.value()] != 0;
    }
    [[nodiscard]] std::span<const std::uint8_t> valueTimeDependence() const noexcept {
        return valueTimeDependent_;
    }
    // Whether every asset record operation `index` carries (image, Cryptomatte and LUT sources)
    // passes AssetRecord::validate(). Derived once here because the records are immutable and
    // the evaluator's preflight asks for every operation of every frame; an image sequence's
    // record holds every frame's file name, so asking per frame costs layers x sequence length.
    [[nodiscard]] bool operationAssetsValid(const OperationIndex index) const noexcept {
        return index.value() < operationAssetsValid_.size() &&
               operationAssetsValid_[index.value()] != 0;
    }
    [[nodiscard]] document::Revision sourceRevision() const noexcept { return sourceRevision_; }
    [[nodiscard]] document::ProjectId projectId() const noexcept { return projectId_; }
    [[nodiscard]] document::CompositionId compositionId() const noexcept { return compositionId_; }
    [[nodiscard]] const document::CompositionFormat& format() const& noexcept { return format_; }
    [[nodiscard]] const document::CompositionFormat& format() const&& = delete;
    [[nodiscard]] std::span<const CompiledOperation> operations() const& noexcept {
        return operations_;
    }
    [[nodiscard]] std::span<const CompiledOperation> operations() const&& = delete;
    [[nodiscard]] OperationIndex output() const noexcept { return output_; }
    [[nodiscard]] std::span<const CompiledScalarCurve> scalarCurves() const& noexcept {
        return scalarCurves_;
    }
    [[nodiscard]] std::span<const CompiledScalarCurve> scalarCurves() const&& = delete;
    [[nodiscard]] std::span<const CompiledVec2Curve> vec2Curves() const& noexcept {
        return vec2Curves_;
    }
    [[nodiscard]] std::span<const CompiledVec2Curve> vec2Curves() const&& = delete;
    [[nodiscard]] std::span<const CompiledVec3Curve> vec3Curves() const& noexcept {
        return vec3Curves_;
    }
    [[nodiscard]] std::span<const CompiledVec3Curve> vec3Curves() const&& = delete;
    [[nodiscard]] std::span<const CompiledColor4Curve> color4Curves() const& noexcept {
        return color4Curves_;
    }
    [[nodiscard]] std::span<const CompiledColor4Curve> color4Curves() const&& = delete;
    [[nodiscard]] std::span<const CompiledValueOperation> valueOperations() const& noexcept {
        return valueOperations_;
    }
    [[nodiscard]] std::span<const CompiledValueOperation> valueOperations() const&& = delete;
    [[nodiscard]] std::size_t valueOutputCount() const noexcept { return valueOutputCount_; }
    [[nodiscard]] const CompositionAudioMix& audioMix() const& noexcept { return audioMix_; }
    [[nodiscard]] const CompositionAudioMix& audioMix() const&& = delete;
    [[nodiscard]] std::uint32_t planSemanticsVersion() const noexcept {
        return planSemanticsVersion_;
    }
    [[nodiscard]] std::uint32_t animationSamplingSemanticsVersion() const noexcept {
        return animationSamplingSemanticsVersion_;
    }

    // This is intentionally an allocating deep copy for tests and tooling that need a mutable
    // candidate definition. It never exposes aliases into the published plan.
    [[nodiscard]] CompiledCompositionPlanDefinition copyDefinition() const;

    // Memoization controls and derived dependence tables are not semantic identity inputs.
    friend bool operator==(const CompiledCompositionPlan&, const CompiledCompositionPlan&);

  private:
    bool bypassOperationCache_ = false;
    void analyzeTimeDependence();
    void analyzeAssetValidity();
    std::vector<std::uint8_t> operationTimeDependent_;
    std::vector<std::uint8_t> operationAssetsValid_;
    std::vector<std::uint8_t> valueTimeDependent_;
    document::Revision sourceRevision_;
    document::ProjectId projectId_;
    document::CompositionId compositionId_;
    document::CompositionFormat format_;
    std::vector<CompiledOperation> operations_;
    OperationIndex output_;
    std::vector<CompiledScalarCurve> scalarCurves_;
    std::vector<CompiledVec2Curve> vec2Curves_;
    std::vector<CompiledVec3Curve> vec3Curves_;
    std::vector<CompiledColor4Curve> color4Curves_;
    std::vector<CompiledValueOperation> valueOperations_;
    std::size_t valueOutputCount_ = 0;
    std::uint32_t planSemanticsVersion_ = kCompiledCompositionPlanSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion_ = kAnimationSamplingSemanticsVersion;
    CompositionAudioMix audioMix_{};
    std::vector<std::shared_ptr<const CompiledCompositionPlan>> nestedPlans_;
    core::RationalTime duration_{};
    std::vector<CompiledTimeClock> clocks_;
    std::vector<std::uint32_t> operationClocks_;
    std::vector<std::uint32_t> placementClocks_;
};

// ADR 0028: the operation a Merge entry takes its layer blend mode and clock from -- the entry's
// own input, or the Layer Output behind a track-matte effect.
[[nodiscard]] inline OperationIndex
mergeEntryLayerIndex(std::span<const CompiledOperation> operations,
                     const OperationIndex input) noexcept {
    if (input.value() < operations.size())
        if (const auto* effect = std::get_if<CompiledImageEffect>(&operations[input.value()]);
            effect != nullptr && effect->trackMatte)
            return effect->input;
    return input;
}

// #502: an entry naming a layer takes them from that layer's Layer Output, which may sit further up
// the entry's image chain (Layer -> Blur -> Merge): the CompiledLayerOutput with the entry's layer
// id reached from the entry's input back through image effects' primary input, track-matte effects
// included. An entry with no layer, or whose chain reaches no such Layer Output, keeps the rule
// above exactly.
[[nodiscard]] inline OperationIndex
mergeEntryLayerIndex(std::span<const CompiledOperation> operations,
                     const CompiledMergeInput& entry) noexcept {
    if (entry.layerId.isValid()) {
        auto current = entry.input;
        while (current.value() < operations.size()) {
            const auto& operation = operations[current.value()];
            if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
                if (layer->layerId == entry.layerId)
                    return current;
                break;
            }
            const auto* effect = std::get_if<CompiledImageEffect>(&operation);
            // Inputs always precede their consumer, so the walk ends.
            if (effect == nullptr || effect->input.value() >= current.value())
                break;
            current = effect->input;
        }
    }
    return mergeEntryLayerIndex(operations, entry.input);
}

[[nodiscard]] inline OperationIndex mergeEntryLayerIndex(const CompiledCompositionPlan& plan,
                                                         const OperationIndex input) noexcept {
    return mergeEntryLayerIndex(plan.operations(), input);
}

[[nodiscard]] inline OperationIndex mergeEntryLayerIndex(const CompiledCompositionPlan& plan,
                                                         const CompiledMergeInput& entry) noexcept {
    return mergeEntryLayerIndex(plan.operations(), entry);
}

} // namespace bloom::runtime

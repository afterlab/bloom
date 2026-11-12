#pragma once

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/codec_degrade_nodes.hpp>
#include <bloom/document/lens_distortion_nodes.hpp>
#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/test_card_nodes.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/sharpen_nodes.hpp>
#include <bloom/document/field_parameters.hpp>
#include <bloom/document/gradient_interpolation.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>
#include <bloom/document/vignette_nodes.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::document {

inline constexpr std::string_view kSolidColorParameterSchemaKey = "bloom.solid.color";
inline constexpr std::string_view kSolidWidthParameterSchemaKey = "bloom.solid.width";
inline constexpr std::string_view kSolidHeightParameterSchemaKey = "bloom.solid.height";
inline constexpr std::string_view kCheckerCellWidthParameterSchemaKey = "bloom.checker.cell-width";
inline constexpr std::string_view kCheckerCellHeightParameterSchemaKey =
    "bloom.checker.cell-height";
inline constexpr std::string_view kCheckerWidthParameterSchemaKey = "bloom.checker.width";
inline constexpr std::string_view kCheckerHeightParameterSchemaKey = "bloom.checker.height";
// Gradient geometry is expressed in full-resolution source pixel-edge coordinates. A pixel is
// sampled at its centre. Radial distance uses the composition's physical pixel aspect.
inline constexpr std::string_view kGradientStartParameterSchemaKey = "bloom.gradient.start";
inline constexpr std::string_view kGradientEndParameterSchemaKey = "bloom.gradient.end";
// #541: the gradient's colours, a GradientValue of one to 64 stops along start -> end (schema 2).
inline constexpr std::string_view kGradientStopsParameterSchemaKey = "bloom.gradient.stops";
inline constexpr std::string_view kGradientRepeatParameterSchemaKey = "bloom.gradient.repeat";
inline constexpr std::string_view kTextParameterSchemaKey = "bloom.text.content";
inline constexpr std::string_view kTextSizeParameterSchemaKey = "bloom.text.size";
// A text font is a font ID's stored text (font_id.hpp, docs/architecture/fonts.md). The integer
// faces below and the old family|style|digest references are read only while opening an older
// document, which converts them; new documents never persist either.
inline constexpr std::string_view kTextFontParameterSchemaKey = "bloom.text.font";
inline constexpr std::int64_t kTextFontDejaVuSans = 0;
inline constexpr std::int64_t kTextFontInterRegular = 1;
inline constexpr std::int64_t kTextFontInterMedium = 2;
inline constexpr std::int64_t kTextFontInterSemiBold = 3;
inline constexpr std::int64_t kTextFontChoiceCount = 4;
inline constexpr std::string_view kDefaultTextFontReference =
    "DejaVu Sans|Book|7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954";
inline constexpr std::int64_t kDefaultTextFontValue = kTextFontDejaVuSans;
// Alignment: 0 = Left, 1 = Center, 2 = Right. Line height is a positive em multiplier;
// letter spacing is a finite number of full-resolution pixels between adjacent glyphs.
inline constexpr std::string_view kTextAlignmentParameterSchemaKey = "bloom.text.alignment";
inline constexpr std::string_view kTextLineHeightParameterSchemaKey = "bloom.text.line-height";
inline constexpr std::string_view kTextLetterSpacingParameterSchemaKey =
    "bloom.text.letter-spacing";
inline constexpr std::string_view kTextBoxParameterSchemaKey = "bloom.text.box";
inline constexpr std::string_view kTextWrapParameterSchemaKey = "bloom.text.wrap";
inline constexpr std::string_view kTextVerticalAlignmentParameterSchemaKey =
    "bloom.text.vertical-alignment";
inline constexpr std::string_view kTextAnchorModeParameterSchemaKey = "bloom.text.anchor-mode";
inline constexpr std::string_view kTextOverflowParameterSchemaKey = "bloom.text.overflow";
// The text anchor-mode value whose block anchor follows the horizontal alignment. Stored 0..2 keep
// the placement every project saved before this value existed has (position at the block centre).
inline constexpr std::int64_t kTextAnchorModeFollowsAlignment = 3;
inline constexpr std::string_view kTextColorParameterSchemaKey = "bloom.text.color";
inline constexpr std::string_view kPositionParameterSchemaKey = "bloom.transform.position";
// Offset from the centre of the source's local content bounds, in full-resolution pixels.
// Position places that anchor in composition space; scale and rotation act about it.
// New Layer v4 follows this contract. Persisted Layer v3 retains frame-relative placement.
inline constexpr std::string_view kAnchorParameterSchemaKey = "bloom.transform.anchor";
// Unitless per-axis scale factor. 1 is unscaled; a negative factor mirrors the axis; 0 collapses
// the layer to nothing, which evaluation renders as an empty layer rather than an error.
inline constexpr std::string_view kScaleParameterSchemaKey = "bloom.transform.scale";
// Clockwise screen rotation in DEGREES (Bloom's y axis points down, so a positive value turns the
// layer clockwise, matching every timeline-based compositor). Unbounded: 450 and 90 evaluate
// identically but are distinct authored values so a rotation curve can wind past a full turn.
inline constexpr std::string_view kRotationParameterSchemaKey = "bloom.transform.rotation";
inline constexpr std::string_view kOpacityParameterSchemaKey = "bloom.layer.opacity";
// How the layer combines with what is beneath it in the stack. The value kind is a small integer
// carrying core::BlendMode's own durable mapping (Normal = 0; see bloom/core/blend_mode.hpp and
// docs/architecture/color-management.md, "Blend modes"), not a name: a stored name would make every
// saved document depend on a spelling, and the enumeration is closed and owned by Bloom. NOT
// animatable -- a blend mode is a discrete choice with no meaningful value between two modes, so
// neither isScalarAnimatableSchemaKey() nor isVec2AnimatableSchemaKey() accepts it and no curve can
// be created over it.
inline constexpr std::string_view kBlendModeParameterSchemaKey = "bloom.layer.blend-mode";
// ADR 0024: optional Layer Output Time Remap, Float64 seconds of content time sampled at layer
// time. Any finite value is admitted; the out-of-extent modes govern values outside the extent.
inline constexpr std::string_view kTimeRemapParameterSchemaKey = "bloom.layer.time-remap";
inline constexpr std::string_view kAudioLevelParameterSchemaKey = "bloom.audio.level";
inline constexpr std::string_view kImageInputColorSpaceIdParameterRole = "inputColorSpaceId";
inline constexpr std::string_view kImageInputColorSpaceIdParameterSchemaKey =
    "bloom.image.input-color-space-id";
inline constexpr std::string_view kVideoInputColorSpaceIdParameterRole = "inputColorSpaceId";
inline constexpr std::string_view kVideoInputColorSpaceIdParameterSchemaKey =
    "bloom.video.input-color-space-id";
// Which of the asset's audio streams (0-based among its audio streams) the Video
// node plays. Optional and non-animatable; absent means the first audio stream.
inline constexpr std::string_view kVideoAudioStreamParameterRole = "audioStream";
inline constexpr std::string_view kVideoAudioStreamParameterSchemaKey = "bloom.video.audio-stream";

// ---------------------------------------------------------------------------------------------
// Value-graph parameter schemas (task S7).
//
// Every one of these is a value a node AUTHORS inline and can equally receive from a driver, which
// is why each is a parameter schema rather than a port-only concept: the inline widget writes the
// parameter's constant, and linking a value-graph output into the matching socket replaces that
// constant with a driver binding. The two are the same storage, never two parallel ones.
//
// The LITERAL value schemas whose kind already has a curve -- Scalar, Vector 2 and Colour -- are
// animatable (task FIX1, item G): a Scalar node the artist keys 0 to 1 over ten frames, driving a
// layer's opacity, is the whole of what "primitives should be able to be animated too" asks for,
// and it reuses the existing curve kinds, commands and sampling rather than inventing a second
// authoring path. Vector 3, Integer, Boolean and String literals stay constant-or-driven, because
// each needs a curve KIND that does not exist yet.
//
// The generic OPERAND schemas below stay constant-or-driven whatever their kind. An operand is a
// value a node reads, and the artist already shapes it with a curve upstream -- wire an animated
// Scalar node into the socket -- so giving it its own curve as well would be a second authoring
// path to the same picture.
// ---------------------------------------------------------------------------------------------

// The literal Value nodes. One schema per authored kind, because the kind IS the meaning here:
// "this node carries a Scalar" is the whole of what a Scalar node's parameter says.
inline constexpr std::string_view kIntegerValueParameterSchemaKey = "bloom.value.integer";
inline constexpr std::string_view kScalarValueParameterSchemaKey = "bloom.value.scalar";
inline constexpr std::string_view kVector2ValueParameterSchemaKey = "bloom.value.vector2";
inline constexpr std::string_view kVector3ValueParameterSchemaKey = "bloom.value.vector3";
inline constexpr std::string_view kStringValueParameterSchemaKey = "bloom.value.string";
inline constexpr std::string_view kColorValueParameterSchemaKey = "bloom.value.color";
inline constexpr std::string_view kBooleanValueParameterSchemaKey = "bloom.value.boolean";
// #589: the literal Path node's value.
inline constexpr std::string_view kPathValueParameterSchemaKey = "bloom.value.path";

// Generic operands, shared by every node in the library that reads a value of that kind. One schema
// per KIND rather than one per node-and-role: a Math node's "a" and a Mix node's "a" mean exactly
// the same thing -- an unconstrained finite scalar -- and two schema keys saying that would be two
// spellings of one domain. The node's own role names which operand it is; the schema names what a
// value of that kind may be.
inline constexpr std::string_view kScalarOperandParameterSchemaKey = "bloom.operand.scalar";
inline constexpr std::string_view kIntegerOperandParameterSchemaKey = "bloom.operand.integer";
inline constexpr std::string_view kBooleanOperandParameterSchemaKey = "bloom.operand.boolean";
inline constexpr std::string_view kVector2OperandParameterSchemaKey = "bloom.operand.vector2";
inline constexpr std::string_view kVector3OperandParameterSchemaKey = "bloom.operand.vector3";
inline constexpr std::string_view kColorOperandParameterSchemaKey = "bloom.operand.color";
inline constexpr std::string_view kStringOperandParameterSchemaKey = "bloom.operand.string";

// The inline selectors. Each is an Integer under its own closed mapping (value_operations.hpp), for
// the same reason the blend mode is: a stored name would make every saved document depend on a
// spelling. None of them is socket-linkable -- the selector is what decides which kernel the plan
// compiles, so it has to be known at compile time, not delivered per frame.
inline constexpr std::string_view kScalarOperationParameterSchemaKey = "bloom.math.operation";
inline constexpr std::string_view kVectorOperationParameterSchemaKey =
    "bloom.vector-math.operation";
inline constexpr std::string_view kVectorReductionParameterSchemaKey =
    "bloom.vector-reduce.operation";
inline constexpr std::string_view kRangeInterpolationParameterSchemaKey =
    "bloom.map-range.interpolation";
inline constexpr std::string_view kCompareOperationParameterSchemaKey = "bloom.compare.operation";
// A Math or Map Range node's result-clamping toggle. Boolean, and deliberately NOT linkable for the
// same reason as the selectors above: it changes the shape of the compiled operation.
inline constexpr std::string_view kClampResultParameterSchemaKey = "bloom.math.clamp-result";
// Compare's equality tolerance. Its own schema rather than the generic scalar operand because the
// domain differs: a negative tolerance is not a tolerance.
inline constexpr std::string_view kCompareEpsilonParameterSchemaKey = "bloom.compare.epsilon";

// Task UTIL-1's own selectors, each an Integer under its own closed mapping in
// value_utility_nodes.hpp and each inline for the same reason as the five above.
inline constexpr std::string_view kRoundingModeParameterSchemaKey = "bloom.convert.rounding-mode";
// The radix a number is read or written in. The stored value is the RADIX ITSELF rather than an
// index into an offered list, so a document that stores 16 means base sixteen even if the card
// later offers a different set of bases.
inline constexpr std::string_view kNumberRadixParameterSchemaKey = "bloom.convert.radix";
inline constexpr std::string_view kStringCaseParameterSchemaKey = "bloom.string.case";
inline constexpr std::string_view kStringPadSideParameterSchemaKey = "bloom.string.pad-side";
inline constexpr std::string_view kIntegerOperationParameterSchemaKey = "bloom.integer.operation";
inline constexpr std::string_view kBooleanOperationParameterSchemaKey = "bloom.boolean.operation";

// The initial solid schema owns straight/unassociated RGBA authoring values and retains this
// historical encoding label for document compatibility. Numeric values are interpreted in the
// effective project/composition working space; evaluation converts them to the canonical
// premultiplied image representation. The text color schema (kTextColorParameterSchemaKey)
// authors in exactly this same label with exactly the same straight-alpha meaning -- a text color
// is a solid color that glyph coverage then scales -- so it reuses this one constant rather than
// declaring a second, identical encoding name.
inline constexpr std::string_view kSolidColorEncoding = "bloom.reference.linear-srgb";

// Text size bounds, in pixels per em, owned by the text size parameter schema. The lower bound is
// exclusive (a zero or negative em size has no meaning); the upper bound is inclusive and is the
// same value render::kMaximumTextPixelSize imposes on the rasterizer, so a document the schema
// accepts is always a document the reference rasterizer can draw. src/runtime, the one module that
// sees both headers, static_asserts the two equal rather than leaving them to drift -- see
// src/runtime/cpu_composition_evaluator.cpp.
inline constexpr double kMaximumTextSizePixels = 4096.0;
// Neighborhood v1 authors at most 513 coefficients per axis at full resolution. Larger
// unsupported radii are rejected explicitly, never silently clamped by preview or final render.
inline constexpr double kMaximumBlurRadiusPixels = 256.0;
// #339: the recipe radii are blur radii.
static_assert(kMaximumRecipeBlurRadius == kMaximumBlurRadiusPixels);
// #338 Luma Key thresholds are luma values bounded to [-1e6, 1e6], so every value the key prepares
// (the ramp's reciprocal included) stays a normal binary32 number on the CPU and GPU routes.
inline constexpr double kMaximumLumaKeyThreshold = 1.0e6;
// The size a newly authored text layer starts at.
inline constexpr double kDefaultTextSizePixels = 72.0;

struct Vec2d {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Vec2d&, const Vec2d&) = default;
};

// Handles are absolute author-space coordinates. Empty and open paths are valid.
// #589: the cap on one contour. A whole value is bounded by kMaximumPathValueAnchors below, so a
// compound value holding a long equation or a paragraph is not limited to one contour's worth.
inline constexpr std::size_t kMaximumPathAnchors = 4096;
// How an anchor's handles respond to editing. It is authoring intent only:
// the rendered curve is always the stored point and handles, so the mode never changes a frame.
//   Corner    no handles; a sharp point.
//   Mirrored  collinear handles of equal length; dragging one moves the other symmetrically.
//   Aligned   collinear handles of independent length; dragging one rotates the other.
//   Free      fully independent handles.
//   Auto      handles derived from the neighbouring anchors (Catmull-Rom), recomputed by every
//             edit that moves a neighbour; dragging a handle turns the anchor Aligned.
enum class PathAnchorMode : std::uint8_t { Corner, Mirrored, Aligned, Free, Auto };
struct PathAnchor {
    Vec2d point;
    std::optional<Vec2d> inHandle;
    std::optional<Vec2d> outHandle;
    PathAnchorMode mode = PathAnchorMode::Corner;
    friend bool operator==(const PathAnchor&, const PathAnchor&) = default;
};
// The mode a stored anchor's geometry implies, for paths that carry none (documents before the
// mode existed, data blocks and script values that omit it): no handle is Corner; two collinear
// handles of equal length are Mirrored; two collinear handles are Aligned; anything else is Free.
[[nodiscard]] PathAnchorMode inferPathAnchorMode(const PathAnchor& anchor) noexcept;
[[nodiscard]] std::string_view pathAnchorModeName(PathAnchorMode mode) noexcept;
[[nodiscard]] std::optional<PathAnchorMode> pathAnchorModeFromName(std::string_view name) noexcept;
// #589: one run of anchors with its own closed flag (docs/architecture/path-value.md).
struct PathContour {
    std::vector<PathAnchor> anchors;
    bool closed = false;
    friend bool operator==(const PathContour&, const PathContour&) = default;
};
// The value's own fill rule. Inherit leaves the choice to whoever reads the value: Shape Source's
// `fillRule` parameter. An explicit rule wins over that parameter. Every path saved before #589
// loads as Inherit, so its Shape keeps the rule it was saved with.
enum class PathFillRule : std::uint8_t { Inherit, NonZero, EvenOdd };
[[nodiscard]] std::string_view pathFillRuleName(PathFillRule rule) noexcept;
[[nodiscard]] std::optional<PathFillRule> pathFillRuleFromName(std::string_view name) noexcept;
// A whole value's bounds. The anchor total is the raster's own segment bound (one anchor is at
// least one flattened point), so a value that cannot rasterize is refused where it is authored.
inline constexpr std::size_t kMaximumPathValueAnchors = 262144;
inline constexpr std::size_t kMaximumPathContours = 65536;
inline constexpr std::size_t kMaximumPathGroups = 65536;
inline constexpr std::size_t kMaximumPathGroupKeyBytes = 256;
// #589: a keyed run of consecutive contours. `key` names the same part across two different paths:
// it is author-chosen, or derived from a source span with pathGroupKeyFromSpan, and it is compared
// as bytes. Groups do not overlap, ascend by first contour, and need not cover every contour.
// `color` is the first per-group attribute; a group that omits it says nothing about color.
struct PathGroup {
    std::string key;
    std::uint32_t firstContour = 0;
    std::uint32_t contourCount = 0;
    std::optional<core::Color4d> color;
    friend bool operator==(const PathGroup&, const PathGroup&) = default;
};
// #589: a path value is ordered contours, a fill rule and keyed groups. A value with no contours is
// the empty path. Fill and stroke read the contours together, in order.
struct PathValue {
    std::vector<PathContour> contours;
    PathFillRule fillRule = PathFillRule::Inherit;
    std::vector<PathGroup> groups;

    // The value holding one contour. An open contour with no anchors is the empty path, which holds
    // none, so an empty open path has one spelling.
    [[nodiscard]] static PathValue fromContour(PathContour contour);
    [[nodiscard]] static PathValue fromContour(std::vector<PathAnchor> anchors, bool closed);
    // The first contour; an empty open one when the value holds none. For readers that have already
    // refused a compound value (isSimple) or that take only the first contour on purpose.
    [[nodiscard]] const PathContour& firstContour() const noexcept;
    // The first contour to edit in place, added when the value holds none.
    [[nodiscard]] PathContour& editableContour();
    // True when at most one contour is held and neither a fill rule nor a group is: the shape every
    // path had before #589, and the only one a mask or a curve accepts.
    [[nodiscard]] bool isSimple() const noexcept;
    [[nodiscard]] std::size_t anchorCount() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
    friend bool operator==(const PathValue&, const PathValue&) = default;
};
// The key for a group derived from the source span [begin, end) (a run of glyphs, an element's byte
// range): the same span always answers the same key, so two paths built from one source match.
[[nodiscard]] std::string pathGroupKeyFromSpan(std::uint64_t begin, std::uint64_t end);

// #541: a colour gradient, the value of a gradient parameter (Colorize's Gradient Map). Any number
// of stops, each a position in [0, 1] and a straight colour; the stops need not be sorted (a reader
// sorts them by position, keeping the stored order of equal positions, which makes a hard edge).
// Between two stops the colour blends as its modes say (#546, gradient_interpolation.hpp: Linear
// in RGB unless set otherwise), outside the first and last stop it holds (the spline modes extend
// to 0 and 1). A gradient animates as a whole value, like a Path: two keys with the same stop count
// interpolate stop by stop (position and colour); otherwise the earlier key holds. The modes always
// come from the earlier key.
inline constexpr std::size_t kMaximumGradientStops = 64;
struct GradientStop {
    double position = 0.0;
    core::Color4d color;
    friend bool operator==(const GradientStop&, const GradientStop&) = default;
};
struct GradientValue {
    std::vector<GradientStop> stops;
    // #546: how the colour blends between stops.
    GradientModes modes{};
    // One to kMaximumGradientStops stops, each position finite within [0, 1] and each colour
    // finite with its alpha within [0, 1], and modes each within its vocabulary.
    [[nodiscard]] bool isValid() const noexcept;
    friend bool operator==(const GradientValue&, const GradientValue&) = default;
};
// The stops by position, equal positions in stored order.
[[nodiscard]] std::vector<GradientStop> sortedGradientStops(const GradientValue& value);
// The straight colour of `value` at `position` in [0, 1], blended as its modes say (binary64).
[[nodiscard]] core::Color4d gradientColorAt(const GradientValue& value, double position);
// #541: a new Colorize node's gradient: its ramp's default colours, black, the warm mid grey at
// 0.18 and white, so a new Gradient Map looks as it did before the gradient existed.
[[nodiscard]] GradientValue defaultColorizeGradient();
// #541: a new Linear or Radial Gradient's colours, black at the start to white at the end, as its
// schema 1 start and end colours were.
[[nodiscard]] GradientValue defaultGeneratorGradient();
// #541: a new Glow's tint gradient, white in the tightest octave to the warm (1, 0.5, 0.25) in the
// widest, as its schema 1 tint and outer tint were.
[[nodiscard]] GradientValue defaultGlowTintGradient();
// #541: a gradient whose stops are valid and within Colorize's +-1e6 colour bound.
[[nodiscard]] bool isBoundedGradient(const GradientValue& value) noexcept;

// #543: a gradient stop's colour or position driven by a value node. Each driven component is its
// own parameter record, bound on the gradient's node under the stop's socket name
// (`<gradient role>.stops[<index>].color` or `.position`) and holding nothing but the driver
// binding, so every walk over a node's drivers (reachability, ordering, cycles, removal) sees it
// as it sees any other driven parameter. `stop` is the stop's stored index in the gradient (the
// stop of that index in every key of an animated gradient); at a time the gradient has no such
// stop, the link changes nothing. The authored stop keeps its own colour and position under it.
enum class GradientStopComponent : std::uint8_t { Color = 0, Position = 1 };
inline constexpr std::string_view kGradientStopColorSchemaKey = "bloom.gradient.stop-color";
inline constexpr std::string_view kGradientStopPositionSchemaKey = "bloom.gradient.stop-position";
struct GradientStopPort final {
    std::string role;
    std::uint32_t stop = 0;
    GradientStopComponent component = GradientStopComponent::Color;
    friend bool operator==(const GradientStopPort&, const GradientStopPort&) = default;
};
[[nodiscard]] constexpr bool isGradientStopSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kGradientStopColorSchemaKey || schemaKey == kGradientStopPositionSchemaKey;
}
[[nodiscard]] constexpr std::string_view
gradientStopSchemaKey(const GradientStopComponent component) noexcept {
    return component == GradientStopComponent::Color ? kGradientStopColorSchemaKey
                                                     : kGradientStopPositionSchemaKey;
}
// The socket name of one stop component, and its inverse: nullopt for any other port, including
// an index of kMaximumGradientStops or more, a leading zero or a sign.
[[nodiscard]] std::string gradientStopPortName(std::string_view gradientRole, std::uint32_t stop,
                                               GradientStopComponent component);
[[nodiscard]] std::optional<GradientStopPort> parseGradientStopPort(std::string_view port);
// One driven stop component's value this frame.
struct GradientStopValue final {
    std::uint32_t stop = 0;
    GradientStopComponent component = GradientStopComponent::Color;
    std::variant<core::Color4d, double> value;
    friend bool operator==(const GradientStopValue&, const GradientStopValue&) = default;
};
// `value` with each driven value applied: a colour replaces its stop's colour, a position (clamped
// to [0, 1]) its stop's position; one whose stop the gradient does not have changes nothing.
// nullopt when a driven value is not finite or leaves the gradient invalid.
[[nodiscard]] std::optional<GradientValue>
applyGradientStopValues(GradientValue value, std::span<const GradientStopValue> values);

// The third authoring vector width (task S7). A genuinely new authoring type rather than a reuse of
// Vec2d or Color4d: a Color is not a Vec4 under
// docs/architecture/evaluation-primitives.md's Type Binding rule, and by the same reasoning a
// three-component vector is not a two-component one with a spare field. It has no animation curve
// kind yet -- no schema declares a Vec3d parameter animatable -- so it is constant-or-driven only.
struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    friend bool operator==(const Vec3d&, const Vec3d&) = default;
};

// The Layer Output transform defaults, in one place so the node definition registry, the layer
// creation commands, and Project I/O's version-1 node upgrade cannot drift apart. Together they are
// the identity transform: no anchor offset, no scaling, no rotation.
inline constexpr Vec2d kDefaultAnchor{};
inline constexpr Vec2d kDefaultScale{1.0, 1.0};
inline constexpr double kDefaultRotationDegrees = 0.0;
// The blend-mode default, in the same one place and for the same reason: Normal, stored as the
// integer core::BlendMode's mapping gives it, so a newly authored layer and an upgraded version-1
// or version-2 Layer Output agree exactly.
inline constexpr std::int64_t kDefaultBlendModeValue =
    core::blendModeStoredValue(core::kDefaultBlendMode);

// ADR 0028 mask parameter schemas, shared by layer masks and the Mask node.
inline constexpr std::string_view kLayerMaskPathSchemaKey = "bloom.mask.path";
inline constexpr std::string_view kLayerMaskFeatherSchemaKey = "bloom.mask.feather";
inline constexpr std::string_view kLayerMaskExpansionSchemaKey = "bloom.mask.expansion";
inline constexpr std::string_view kLayerMaskOpacitySchemaKey = "bloom.mask.opacity";
inline constexpr std::string_view kLayerMaskInvertedSchemaKey = "bloom.mask.inverted";
inline constexpr double kMaximumLayerMaskFeatherPixels = 1000.0;
inline constexpr double kMaximumLayerMaskExpansionPixels = 1000.0;

// The animatable schema set, split by the curve value kind each member requires. These predicates
// are the single authority every layer asks -- document validation, the animation commands, and the
// snapshot compiler's override gate -- so the set cannot be widened in one place and stay narrow in
// another. A schema key that satisfies none of them is constant-only, which after task S5 is
// exactly the text CONTENT schema (a String has no interpolation) and every unregistered key.
[[nodiscard]] bool isVec2AnimatableSchemaKey(const std::string_view schemaKey) noexcept;

[[nodiscard]] constexpr bool isVec3AnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kVector3ValueParameterSchemaKey;
}

[[nodiscard]] bool isScalarAnimatableSchemaKey(const std::string_view schemaKey) noexcept;

// ADR 0028: Path parameters animate through whole-value Path curves. #493: so does each Curves
// curve, a Path used as a point list (grade_curves.hpp).
[[nodiscard]] constexpr bool isPathAnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kLayerMaskPathSchemaKey || schemaKey == kPathValueParameterSchemaKey ||
           isGradeCurveSchemaKey(schemaKey) || isFieldPathAnimatableSchemaKey(schemaKey) ||
           isDepthPathAnimatableSchemaKey(schemaKey);
}

// #541: a gradient parameter animates through whole-value Gradient curves.
[[nodiscard]] constexpr bool
isGradientAnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return isColorizeGradientSchemaKey(schemaKey) ||
           schemaKey == kGradientStopsParameterSchemaKey ||
           schemaKey == kGlowTintGradientSchemaKey || isFieldGradientAnimatableSchemaKey(schemaKey);
}

[[nodiscard]] constexpr bool isCanvasMarginSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == "bloom.crop.left" || schemaKey == "bloom.crop.top" ||
           schemaKey == "bloom.crop.right" || schemaKey == "bloom.crop.bottom" ||
           schemaKey == "bloom.pad.left" || schemaKey == "bloom.pad.top" ||
           schemaKey == "bloom.pad.right" || schemaKey == "bloom.pad.bottom";
}

// The Color4d-valued animatable schemas (task S5): a solid's colour and a text layer's colour.
// Both author straight RGBA under the historical kSolidColorEncoding label, so one curve kind
// serves both.
//
// Task FIX1, item G: the value LIBRARY's own literals join all four sets. A Scalar node, a Vector
// 2/3 node and a Colour node hold exactly the kinds these curves carry, so animating one is
// the same gesture, the same command, and the same sampling the layer parameters already have --
// and a driven parameter downstream of one reads the sampled value per frame. Integer, Boolean and
// String stay constant-or-driven: each would need a separate discrete interpolation contract.
[[nodiscard]] bool isColor4AnimatableSchemaKey(const std::string_view schemaKey) noexcept;

[[nodiscard]] bool isAnimatableSchemaKey(const std::string_view schemaKey) noexcept;

// Whether a scalar value under this schema is confined to [0, 1]. Opacity is the only one: rotation
// degrees must be free to wind past a full turn in either direction, so the unit domain belongs to
// the schema rather than to "scalar values" as a class.
[[nodiscard]] constexpr bool hasUnitDomainSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kOpacityParameterSchemaKey || schemaKey == kAudioLevelParameterSchemaKey ||
           schemaKey == "bloom.image-mix.factor" || schemaKey == "bloom.multiply-alpha.factor" ||
           schemaKey == kLayerMaskOpacitySchemaKey;
}

// Whether a scalar value under this schema is confined to the text size domain (0, kMaximumText-
// SizePixels]. Same reasoning as the unit domain above: the bound belongs to the schema, not to
// "scalar" as a class, and task S5 made text size animatable so a KEY has to satisfy it too.
[[nodiscard]] constexpr bool hasTextSizeDomainSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kTextSizeParameterSchemaKey;
}

// The ONE scalar-domain gate every layer asks (document validation, the animation commands, the
// evaluator's per-key check, the session's write paths), so a constant and a keyframe under the
// same schema can never be admitted on different terms. `value` must already be finite; finiteness
// is checked separately by each caller because the diagnostic it produces differs.
[[nodiscard]] bool isScalarWithinSchemaDomain(const std::string_view schemaKey,
                                                        const double value) noexcept;

using ParameterValue = std::variant<bool, std::int64_t, double, Vec2d, Vec3d, core::Color4d,
                                    std::string, core::RationalTime, PathValue, GradientValue>;

struct ConstantValueSource {
    ParameterValue value;

    friend bool operator==(const ConstantValueSource&, const ConstantValueSource&) = default;
};

struct AnimationCurveSource {
    AnimationCurveId curveId;
    // Component curves with no keyframes use this parameter value for their constant component.
    // It is optional only for source-compatibility with pre-component in-memory callers; current
    // document records carry it whenever a component-aware curve can need a fallback.
    std::optional<ParameterValue> defaultValue{};

    friend bool operator==(const AnimationCurveSource&, const AnimationCurveSource&) = default;
};

// A parameter driven by a value-graph node's output (task S7). Durable, and structurally identical
// to an ordinary OutputPortRef -- it just lands in parameter-address space instead of on an input
// port, which is the whole of what a driver is.
//
// Deliberately inline rather than an id into a separate driver table: a table would add a document
// collection, an encoding, and an id space whose only content is this pair, and a dangling id would
// be a new failure mode the pair cannot have. DriverBindingId still exists as an allocated id space
// (ids.hpp) because a saved document's high-water marks are durable and this slice is not the place
// to change the allocator's shape; no live record uses it.
//
// Cycles are not a new concept either: CanonicalGraph::validate() feeds these bindings into the
// same adjacency the edge set builds, so a driver that closes a loop is refused by the one existing
// same-time cycle check rather than by a second rule that could disagree with it.
struct DriverBindingSource {
    NodeId sourceNodeId;
    std::string outputPort;

    friend bool operator==(const DriverBindingSource&, const DriverBindingSource&) = default;
};

using ParameterSource =
    std::variant<ConstantValueSource, AnimationCurveSource, DriverBindingSource>;

struct ParameterRecord {
    ParameterId id;
    std::string schemaKey;
    ParameterSource source;

    friend bool operator==(const ParameterRecord&, const ParameterRecord&) = default;
};

struct ParameterBinding {
    std::string role;
    ParameterId parameterId;

    friend bool operator==(const ParameterBinding&, const ParameterBinding&) = default;
};

class ParameterStore final {
  public:
    [[nodiscard]] std::span<const ParameterRecord> records() const noexcept { return records_; }
    [[nodiscard]] const ParameterRecord* find(ParameterId id) const noexcept;

    [[nodiscard]] bool insert(ParameterRecord record);
    [[nodiscard]] bool erase(ParameterId id);
    [[nodiscard]] bool setSource(ParameterId id, ParameterSource source);

    [[nodiscard]] ValidationResult validate() const;

  private:
    std::vector<ParameterRecord> records_;
    // True while the records are in ascending id order, which is how ids are allocated and how a
    // document is read, so a lookup is a binary search. An out-of-order insert clears it and
    // lookups scan, as they always did.
    bool sorted_ = true;
};

} // namespace bloom::document

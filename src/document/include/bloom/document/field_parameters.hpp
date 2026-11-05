#pragma once

// #525 Fields: the parameter vocabulary of the Fields node family (field_nodes.hpp has the nodes
// and their sockets). Every table here is constexpr so the document's animatable-schema and domain
// predicates (parameter.hpp) answer for the Fields keys without a second list.
//
// Schema keys are "<type id>.<role>" (bloom.field-noise.size). Every Scalar and Vector 2 parameter
// marked `animated` is animatable and has a socket of its own (drivable); every Choice, Integer,
// Seed and Boolean is a constant that chooses the arithmetic, with no socket. A Curve is a Curves
// master point list (grade_curves.hpp, at most kMaximumFieldCurvePoints points), animatable as a
// whole value; a Gradient is a Gradient value, animatable as a whole value.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kFieldNoiseNodeType = "bloom.field-noise";
inline constexpr std::string_view kFieldVoronoiNodeType = "bloom.field-voronoi";
inline constexpr std::string_view kFieldRandomNodeType = "bloom.field-random";
inline constexpr std::string_view kFieldPatternNodeType = "bloom.field-pattern";
inline constexpr std::string_view kFieldGradientNodeType = "bloom.field-gradient";
inline constexpr std::string_view kFieldShapeDistanceNodeType = "bloom.field-shape-distance";
inline constexpr std::string_view kFieldFromImageNodeType = "bloom.field-from-image";
inline constexpr std::string_view kFieldForceNodeType = "bloom.field-force";
inline constexpr std::string_view kFieldMathNodeType = "bloom.field-math";
inline constexpr std::string_view kFieldRemapNodeType = "bloom.field-remap";
inline constexpr std::string_view kFieldTransformNodeType = "bloom.field-transform";
inline constexpr std::string_view kFieldDerivativeNodeType = "bloom.field-derivative";
inline constexpr std::string_view kFieldCombineNodeType = "bloom.field-combine";
inline constexpr std::string_view kFieldSeparateNodeType = "bloom.field-separate";
inline constexpr std::string_view kFieldSampleNodeType = "bloom.field-sample";
inline constexpr std::string_view kFieldRasterizeNodeType = "bloom.field-rasterize";

inline constexpr std::size_t kMaximumFieldCurvePoints = 16;
// Every authored length, offset and position stays within +-1e6 composition pixels.
inline constexpr double kMaximumFieldExtent = 1.0e6;

enum class FieldParameterKind : std::uint8_t {
    Choice,  // a closed enumeration, stored as Integer
    Integer, // a bounded whole number
    Seed,    // any Integer, hashed (never added to a coordinate)
    Boolean,
    Scalar,
    Vector2,
    Curve,
    Gradient,
};

struct FieldParameterSpec final {
    std::string_view role;
    FieldParameterKind kind = FieldParameterKind::Scalar;
    // The default: a Scalar's, Integer's, Choice's or Boolean's value in x, a Vector 2's (x, y).
    double defaultX = 0.0;
    double defaultY = 0.0;
    // The document domain of a Scalar, Vector 2 component or Integer.
    double minimum = -kMaximumFieldExtent;
    double maximum = kMaximumFieldExtent;
    // A Choice's labels, in stored-value order.
    std::span<const std::string_view> choices{};
    // A Scalar or Vector 2 that animates and has its own socket.
    bool animated = true;
};

// --- Stored values of the choices (the runtime maps them onto bloom::core::field) ---------------

// Noise
inline constexpr std::int64_t kFieldNoisePerlin = 0;
inline constexpr std::int64_t kFieldNoiseSimplex = 1;
inline constexpr std::int64_t kFieldNoiseLinear = 2;
inline constexpr std::int64_t kFieldNoiseSoftLinear = 3;
inline constexpr std::int64_t kFieldNoiseBlock = 4;
inline constexpr std::int64_t kFieldNoiseSpline = 5;
inline constexpr std::int64_t kFieldFractalNone = 0;
inline constexpr std::int64_t kFieldFractalFbm = 1;
inline constexpr std::int64_t kFieldFractalTurbulence = 2;
inline constexpr std::int64_t kFieldFractalRidged = 3;
inline constexpr std::int64_t kFieldFractalBillow = 4;
inline constexpr std::int64_t kFieldFractalDynamic = 5;
inline constexpr std::int64_t kFieldFractalDynamicProgressive = 6;
inline constexpr std::int64_t kFieldFractalDynamicTwist = 7;
inline constexpr std::int64_t kFieldEvolutionSlice = 0;
inline constexpr std::int64_t kFieldEvolutionFlow = 1;
inline constexpr std::int64_t kFieldOverflowClip = 0;
inline constexpr std::int64_t kFieldOverflowSoftClamp = 1;
inline constexpr std::int64_t kFieldOverflowWrapBack = 2;
inline constexpr std::int64_t kFieldOverflowAllowHdr = 3;
inline constexpr std::int64_t kFieldRangeUnipolar = 0;
inline constexpr std::int64_t kFieldRangeBipolar = 1;

// Shape Distance
inline constexpr std::int64_t kFieldShapeSourcePrimitive = 0;
inline constexpr std::int64_t kFieldShapeSourceMask = 1;

// Rasterize
inline constexpr std::int64_t kFieldRasterRaw = 0;
inline constexpr std::int64_t kFieldRasterMask = 1;
inline constexpr std::int64_t kFieldRasterColor = 2;
inline constexpr std::int64_t kFieldExtentComposition = 0;
inline constexpr std::int64_t kFieldExtentInput = 1;
inline constexpr std::int64_t kFieldExtentExplicit = 2;
inline constexpr std::int64_t kFieldInspectNone = 0;
inline constexpr std::int64_t kFieldInspectHeatmap = 1;
inline constexpr std::int64_t kFieldInspectArrows = 2;
inline constexpr std::int64_t kFieldInspectStreamlines = 3;

// Derivative
inline constexpr std::int64_t kFieldDerivativeGradient = 0;
inline constexpr std::int64_t kFieldDerivativeCurl = 1;
inline constexpr std::int64_t kFieldDerivativeDivergence = 2;


namespace field_parameters {

using Kind = FieldParameterKind;

inline constexpr double kMax = kMaximumFieldExtent;

inline constexpr FieldParameterSpec choice(const std::string_view role,
                                    const std::span<const std::string_view> labels,
                                    const std::int64_t value = 0) {
    return {role, Kind::Choice, static_cast<double>(value), 0.0, 0.0,
            static_cast<double>(labels.size()) - 1.0, labels, false};
}
inline constexpr FieldParameterSpec integer(const std::string_view role, const std::int64_t value,
                                     const std::int64_t minimum, const std::int64_t maximum) {
    return {role,
            Kind::Integer,
            static_cast<double>(value),
            0.0,
            static_cast<double>(minimum),
            static_cast<double>(maximum),
            {},
            false};
}
inline constexpr FieldParameterSpec seed(const std::string_view role = "seed") {
    return {role, Kind::Seed, 0.0, 0.0, -9.0e18, 9.0e18, {}, false};
}
inline constexpr FieldParameterSpec boolean(const std::string_view role, const bool value = false) {
    return {role, Kind::Boolean, value ? 1.0 : 0.0, 0.0, 0.0, 1.0, {}, false};
}
inline constexpr FieldParameterSpec scalar(const std::string_view role, const double value,
                                    const double minimum = -kMax, const double maximum = kMax) {
    return {role, Kind::Scalar, value, 0.0, minimum, maximum, {}, true};
}
inline constexpr FieldParameterSpec vector2(const std::string_view role, const double x, const double y,
                                     const double minimum = -kMax, const double maximum = kMax,
                                     const bool animated = true) {
    return {role, Kind::Vector2, x, y, minimum, maximum, {}, animated};
}
// A curve whose default is the identity, or (falling) the line from (0, 1) down to (1, 0).
inline constexpr FieldParameterSpec curve(const std::string_view role = "curve",
                                          const bool falling = false) {
    return {role, Kind::Curve, falling ? 1.0 : 0.0, 0.0, 0.0, 0.0, {}, true};
}
inline constexpr FieldParameterSpec gradient(const std::string_view role = "ramp") {
    return {role, Kind::Gradient, 0.0, 0.0, 0.0, 0.0, {}, true};
}

// --- Choice labels (stored-value order) ------------------------------------------------------

inline constexpr std::array<std::string_view, 6> kNoiseTypes{"Perlin", "Simplex",  "Linear",
                                                      "Soft Linear", "Block", "Spline"};
// A layer-based compositor's fractal-noise names in brackets where the variant is its equivalent.
inline constexpr std::array<std::string_view, 8> kFractals{"None",
                                                    "fBm (Basic)",
                                                    "Turbulence (Turbulent Basic)",
                                                    "Ridged (Turbulent Sharp)",
                                                    "Billow (Turbulent Smooth)",
                                                    "Dynamic",
                                                    "Dynamic Progressive",
                                                    "Dynamic Twist"};
inline constexpr std::array<std::string_view, 2> kEvolutionModes{"Slice", "Flow"};
inline constexpr std::array<std::string_view, 4> kOverflows{"Clip", "Soft Clamp", "Wrap Back",
                                                     "Allow HDR Results"};
inline constexpr std::array<std::string_view, 2> kRanges{"Unipolar", "Bipolar"};
inline constexpr std::array<std::string_view, 5> kVoronoiModes{"F1", "F2", "F2 - F1", "Edge",
                                                        "Smooth F1"};
inline constexpr std::array<std::string_view, 3> kMetrics{"Euclidean", "Manhattan", "Chebyshev"};
inline constexpr std::array<std::string_view, 2> kDistributions{"Uniform", "Gaussian"};
inline constexpr std::array<std::string_view, 7> kPatternShapes{"Stripes", "Checker",    "Rings", "Dots",
                                                         "Grid Lines", "Hex", "Wave"};
inline constexpr std::array<std::string_view, 4> kWaveforms{"Sine", "Triangle", "Square", "Saw"};
inline constexpr std::array<std::string_view, 4> kGradientModes{"Linear", "Radial", "Angular",
                                                         "Diamond"};
inline constexpr std::array<std::string_view, 3> kRepeats{"Clamp", "Repeat", "Mirror"};
inline constexpr std::array<std::string_view, 2> kShapeSources{"Primitive", "Mask"};
inline constexpr std::array<std::string_view, 7> kPrimitives{"Circle",  "Box",  "Rounded Box", "Segment",
                                                      "Polygon", "Star", "Ring"};
inline constexpr std::array<std::string_view, 4> kFalloffCurves{"Linear", "Smooth", "Exponential",
                                                         "Step"};
inline constexpr std::array<std::string_view, 5> kChannels{"Red", "Green", "Blue", "Alpha", "Luma"};
inline constexpr std::array<std::string_view, 4> kPairChannels{"Red", "Green", "Blue", "Alpha"};
inline constexpr std::array<std::string_view, 3> kFilters{"Bilinear", "Nearest", "Bicubic"};
inline constexpr std::array<std::string_view, 4> kEdges{"Clamp", "Transparent", "Repeat", "Mirror"};
inline constexpr std::array<std::string_view, 3> kForceModes{"Vortex", "Radial", "Uniform"};
inline constexpr std::array<std::string_view, 2> kSpins{"Counter-clockwise", "Clockwise"};
inline constexpr std::array<std::string_view, 4> kForceFalloffs{"Linear", "Smooth", "Inverse Square",
                                                         "Curve"};
inline constexpr std::array<std::string_view, 18> kMathOperations{
    "Add",        "Subtract",   "Multiply",   "Divide", "Min",    "Max",
    "Power",      "Abs",        "Difference", "Lerp",   "Screen", "Smooth Min",
    "Smooth Max", "Dot",        "Length",     "Normalize", "Scale", "Rotate"};
inline constexpr std::array<std::string_view, 5> kCoordinateModes{"None", "Polar", "Tile", "Mirror",
                                                           "Kaleidoscope"};
inline constexpr std::array<std::string_view, 3> kDerivativeModes{"Gradient", "Curl", "Divergence"};
inline constexpr std::array<std::string_view, 3> kRasterModes{"Raw Data", "Mask", "Colour (Ramp)"};
inline constexpr std::array<std::string_view, 3> kExtents{"Composition", "Input Image", "Explicit"};
inline constexpr std::array<std::string_view, 3> kSupersamples{"1 Sample", "4 Samples", "16 Samples"};
inline constexpr std::array<std::string_view, 4> kInspections{"None", "Heatmap", "Arrows",
                                                       "Streamlines"};


inline constexpr std::array kNoiseParameters{
    choice("type", kNoiseTypes, kFieldNoisePerlin),
    choice("fractal", kFractals, kFieldFractalFbm),
    seed(),
    vector2("size", 200.0, 200.0, 0.01, kMax),
    vector2("offset", 0.0, 0.0),
    scalar("rotation", 0.0),
    scalar("octaves", 6.0, 1.0, 10.0),
    scalar("lacunarity", 2.0, 1.0, 10.0),
    scalar("gain", 0.5, 0.0, 1.0),
    scalar("octaveRotation", 0.0),
    vector2("octaveOffset", 0.0, 0.0),
    scalar("ridgeOffset", 1.0, 0.0, 10.0),
    scalar("ridgeGain", 2.0, 0.0, 10.0),
    scalar("evolution", 0.0),
    choice("evolutionMode", kEvolutionModes, kFieldEvolutionSlice),
    boolean("loop"),
    scalar("loopLength", 0.0, 0.0, kMax),
    scalar("contrast", 1.0, 0.0, 100.0),
    scalar("brightness", 0.0, -10.0, 10.0),
    boolean("invert"),
    choice("overflow", kOverflows, kFieldOverflowAllowHdr),
    choice("range", kRanges, kFieldRangeUnipolar)};
inline constexpr std::array kVoronoiParameters{choice("mode", kVoronoiModes),
                                        choice("metric", kMetrics),
                                        seed(),
                                        scalar("size", 100.0, 0.01, kMax),
                                        scalar("jitter", 1.0, 0.0, 1.0),
                                        scalar("smoothness", 0.0, 0.0, kMax),
                                        scalar("evolution", 0.0),
                                        integer("octaves", 1, 1, 4)};
inline constexpr std::array kRandomParameters{seed(), vector2("cellSize", 1.0, 1.0, 0.01, kMax),
                                       scalar("evolution", 0.0), boolean("smooth"),
                                       choice("distribution", kDistributions)};
inline constexpr std::array kPatternParameters{choice("shape", kPatternShapes),
                                        scalar("size", 50.0, 0.01, kMax),
                                        scalar("width", 0.5, 0.0, 1.0),
                                        scalar("rotation", 0.0),
                                        scalar("phase", 0.0),
                                        vector2("centre", 0.0, 0.0),
                                        choice("waveform", kWaveforms),
                                        scalar("speed", 0.0),
                                        scalar("softness", 1.0, 0.0, kMax)};
inline constexpr std::array kGradientParameters{choice("mode", kGradientModes),
                                         vector2("start", -480.0, 0.0),
                                         vector2("end", 480.0, 0.0), choice("repeat", kRepeats),
                                         scalar("cycle", 0.0)};
inline constexpr std::array kShapeParameters{choice("source", kShapeSources),
                                      choice("primitive", kPrimitives),
                                      vector2("centre", 0.0, 0.0),
                                      vector2("size", 200.0, 200.0, 0.0, kMax),
                                      scalar("rotation", 0.0),
                                      scalar("cornerRadius", 0.0, 0.0, kMax),
                                      integer("sides", 5, 3, 64),
                                      scalar("innerRatio", 0.5, 0.0, 1.0),
                                      scalar("falloff", 100.0, 0.0, kMax),
                                      choice("falloffCurve", kFalloffCurves),
                                      boolean("invert")};
inline constexpr std::array kImageParameters{choice("channel", kChannels, 4),
                                      choice("pairFirst", kPairChannels, 0),
                                      choice("pairSecond", kPairChannels, 1),
                                      choice("filter", kFilters), choice("edge", kEdges)};
inline constexpr std::array kForceParameters{choice("mode", kForceModes),
                                      vector2("centre", 0.0, 0.0),
                                      scalar("strength", 1.0),
                                      scalar("radius", 200.0, 0.01, kMax),
                                      choice("direction", kSpins),
                                      choice("falloff", kForceFalloffs),
                                      curve("curve", true),
                                      scalar("angle", 0.0)};
inline constexpr std::array kMathParameters{choice("operation", kMathOperations),
                                     scalar("valueA", 0.0),
                                     scalar("valueB", 1.0),
                                     scalar("valueT", 0.5),
                                     scalar("smoothness", 0.1, 0.0, kMax),
                                     scalar("angle", 0.0)};
inline constexpr std::array kRemapParameters{scalar("inLow", 0.0),   scalar("inHigh", 1.0),
                                      scalar("outLow", 0.0),  scalar("outHigh", 1.0),
                                      boolean("clamp"),       boolean("invert"),
                                      integer("steps", 0, 0, 1024), scalar("bias", 0.5, 0.0, 1.0),
                                      scalar("gain", 0.5, 0.0, 1.0), curve()};
inline constexpr std::array kTransformParameters{vector2("translate", 0.0, 0.0),
                                          scalar("rotation", 0.0),
                                          vector2("scale", 1.0, 1.0),
                                          vector2("pivot", 0.0, 0.0),
                                          choice("mode", kCoordinateModes),
                                          vector2("tile", 200.0, 200.0, 0.01, kMax),
                                          integer("segments", 6, 1, 64),
                                          scalar("warpAmount", 0.0)};
inline constexpr std::array kDerivativeParameters{choice("mode", kDerivativeModes),
                                           scalar("step", 1.0, 0.001, kMax),
                                           scalar("strength", 1.0)};
inline constexpr std::array kCombineParameters{scalar("valueX", 0.0), scalar("valueY", 0.0),
                                        scalar("valueZ", 0.0), scalar("valueW", 1.0)};
inline constexpr std::array<FieldParameterSpec, 0> kNoParameters{};
inline constexpr std::array kSampleParameters{vector2("position", 0.0, 0.0)};
inline constexpr std::array kRasterParameters{
    choice("output", kRasterModes, kFieldRasterRaw),
    choice("extent", kExtents, kFieldExtentComposition),
    vector2("explicitCentre", 0.0, 0.0, -kMax, kMax, false),
    vector2("explicitSize", 1920.0, 1080.0, 1.0, 16384.0, false),
    choice("supersample", kSupersamples),
    choice("inspect", kInspections, kFieldInspectNone),
    gradient(),
    scalar("rampLow", 0.0),
    scalar("rampHigh", 1.0),
    scalar("arrowSpacing", 32.0, 4.0, kMax),
    scalar("arrowScale", 20.0, 0.0, kMax)};

} // namespace field_parameters

// The parameters of the Fields node `typeId` (empty for any other type).
[[nodiscard]] constexpr std::span<const FieldParameterSpec>
fieldParameters(const std::string_view typeId) noexcept {
    using namespace field_parameters;
    if (typeId == kFieldNoiseNodeType)
        return kNoiseParameters;
    if (typeId == kFieldVoronoiNodeType)
        return kVoronoiParameters;
    if (typeId == kFieldRandomNodeType)
        return kRandomParameters;
    if (typeId == kFieldPatternNodeType)
        return kPatternParameters;
    if (typeId == kFieldGradientNodeType)
        return kGradientParameters;
    if (typeId == kFieldShapeDistanceNodeType)
        return kShapeParameters;
    if (typeId == kFieldFromImageNodeType)
        return kImageParameters;
    if (typeId == kFieldForceNodeType)
        return kForceParameters;
    if (typeId == kFieldMathNodeType)
        return kMathParameters;
    if (typeId == kFieldRemapNodeType)
        return kRemapParameters;
    if (typeId == kFieldTransformNodeType)
        return kTransformParameters;
    if (typeId == kFieldDerivativeNodeType)
        return kDerivativeParameters;
    if (typeId == kFieldCombineNodeType)
        return kCombineParameters;
    if (typeId == kFieldSampleNodeType)
        return kSampleParameters;
    if (typeId == kFieldRasterizeNodeType)
        return kRasterParameters;
    return {};
}

// The parameter `role` of the Fields node `typeId`, or null.
[[nodiscard]] constexpr const FieldParameterSpec* fieldParameterSpec(const std::string_view typeId,
                                                                     const std::string_view role) noexcept {
    for (const auto& parameter : fieldParameters(typeId))
        if (parameter.role == role)
            return &parameter;
    return nullptr;
}

// The parameter `schemaKey` names, or null for a key that is not a Fields parameter.
[[nodiscard]] constexpr const FieldParameterSpec*
fieldParameterSpec(const std::string_view schemaKey) noexcept {
    if (!schemaKey.starts_with("bloom.field-"))
        return nullptr;
    const auto dot = schemaKey.rfind('.');
    if (dot == std::string_view::npos || dot < 12)
        return nullptr;
    return fieldParameterSpec(schemaKey.substr(0, dot), schemaKey.substr(dot + 1));
}

[[nodiscard]] constexpr bool isFieldScalarAnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = fieldParameterSpec(key);
    return spec != nullptr && spec->kind == FieldParameterKind::Scalar && spec->animated;
}
[[nodiscard]] constexpr bool isFieldVec2AnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = fieldParameterSpec(key);
    return spec != nullptr && spec->kind == FieldParameterKind::Vector2 && spec->animated;
}
[[nodiscard]] constexpr bool isFieldPathAnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = fieldParameterSpec(key);
    return spec != nullptr && spec->kind == FieldParameterKind::Curve;
}
[[nodiscard]] constexpr bool
isFieldGradientAnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = fieldParameterSpec(key);
    return spec != nullptr && spec->kind == FieldParameterKind::Gradient;
}
// A constant choice (Choice, Integer, Seed, Boolean or a constant Vector 2): no socket and no
// animation.
[[nodiscard]] constexpr bool isFieldConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = fieldParameterSpec(key);
    if (spec == nullptr)
        return false;
    switch (spec->kind) {
    case FieldParameterKind::Choice:
    case FieldParameterKind::Integer:
    case FieldParameterKind::Seed:
    case FieldParameterKind::Boolean:
        return true;
    case FieldParameterKind::Vector2:
        return !spec->animated;
    case FieldParameterKind::Scalar:
    case FieldParameterKind::Curve:
    case FieldParameterKind::Gradient:
        return false;
    }
    return false;
}
// Whether the Scalar `value` lies in the Fields parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isFieldScalarWithinDomain(const std::string_view key,
                                                       const double value) noexcept {
    const auto* spec = fieldParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document

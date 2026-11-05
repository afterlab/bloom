#pragma once

// #483 Depth effects: the durable parameter vocabulary of Z Defocus and Depth Fog, the two Filters nodes that
// read a depth pass. Both take an Image on `input`, a depth source on `depth` and give an Image on
// `image`; both take an effect mask (matte_nodes.hpp).
//
// The depth source is any pixel image: an OpenEXR pass through Pass or Shuffle, a PNG, TIFF or JPEG
// depth or mist map (8 or 16-bit integer, or float), any node's image output, or a channel set
// directly (its channel picked by name). Depth is one value per pixel, read from the channel the
// `depthChannel` picker names: R, G, B or A of an image (empty is the first channel, R), or a
// channel's name in a channel set (Z, depth.V; empty is the set's first data channel). It is never
// luminance-weighted. Integer files arrive as their normalised 0-1 values; the interpretation says
// what the value is:
//   Z (scene units)        the distance itself;
//   Inverse depth (1/Z)    the reciprocal of the distance (0 is infinitely far);
//   Normalised (0-1)       0 is `near`, 1 is `far`, linear in distance;
//   Mist (0-1)             a mist pass: 0 at `near`, 1 at `far`, after the renderer's falloff
//                          (`mistFalloff`: Linear, Quadratic or Inverse quadratic, the common mist
//                          encodings), which is undone first.
// `invert` reads a normalised or mist map with white near. A pixel is empty -- no surface -- when its
// alpha is 0, its value is not finite, or (Z) it is not above 0 or is 1e9 or more, or (1/Z) it is
// not above 0; `background` says what an empty pixel is: infinitely far, at the far distance, or
// unaffected (no blur, no fog). docs/architecture/image-filters.md, "Depth effects", is the
// specification.
//
// Schema keys are "<type id>.<role>" (bloom.z-defocus.focusDistance), like the Fields keys. Every
// Scalar, Vector 2 and Colour marked `animated` is animatable, drivable and has a socket of its own;
// every Choice, Integer, Boolean and Text is a constant that chooses the arithmetic, with no socket.
// The fog falloff Curve is a Curves master point list, animatable as a whole value.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kZDefocusNodeType = "bloom.z-defocus";
inline constexpr std::string_view kDepthFogNodeType = "bloom.depth-fog";
inline constexpr std::uint32_t kDepthEffectNodeSchemaVersion = 1;

// The ports. `depth` takes an Image, a Mask or a channel set; `bokeh` (Z Defocus) and `fog` (Depth
// Fog) an optional Image.
inline constexpr std::string_view kDepthEffectInputPort = "input";
inline constexpr std::string_view kDepthEffectDepthPort = "depth";
inline constexpr std::string_view kZDefocusBokehPort = "bokeh";
inline constexpr std::string_view kDepthFogImagePort = "fog";
inline constexpr std::string_view kDepthEffectOutputPort = "image";

// Distances are scene units within [0, 1e9]; blur sizes full-resolution pixels within [0, 256]
// (the blur radius bound of every neighbourhood filter).
inline constexpr double kMaximumDepthDistance = 1.0e9;
inline constexpr double kMaximumDefocusSize = 256.0;
// A Z value at or above this is empty (a renderer's "nothing hit" depth).
inline constexpr double kEmptyDepthThreshold = 1.0e9;
inline constexpr std::size_t kMaximumDepthChannelBytes = 256;
inline constexpr std::size_t kMaximumDepthCurvePoints = 16;
// The focus point reads the depth over this many pixels on each side of it (a 5 x 5 area).
inline constexpr std::int64_t kFocusSampleRadius = 2;

enum class DepthParameterKind : std::uint8_t {
    Choice,  // a closed enumeration, stored as Integer
    Integer, // a bounded whole number
    Boolean,
    Scalar,
    Vector2,
    Color,
    Curve,
    Text,
};

struct DepthParameterSpec final {
    std::string_view role;
    DepthParameterKind kind = DepthParameterKind::Scalar;
    // The default: a Scalar's, Integer's, Choice's or Boolean's value in [0], a Vector 2's in [0]
    // and [1], a Colour's straight RGBA.
    std::array<double, 4> value{};
    // The document domain of a Scalar, a Vector 2 component or an Integer.
    double minimum = 0.0;
    double maximum = 0.0;
    // A Choice's labels, in stored-value order.
    std::span<const std::string_view> choices{};
    bool animated = false;
    // The control's label and its presentation: the span a slider covers, one step, the decimals
    // shown and the unit.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 0.01;
    int decimals = 2;
    std::string_view unit{};
};

// --- Stored values of the choices ---------------------------------------------------------------

inline constexpr std::int64_t kDepthInterpretationZ = 0;
inline constexpr std::int64_t kDepthInterpretationInverse = 1;
inline constexpr std::int64_t kDepthInterpretationNormalised = 2;
inline constexpr std::int64_t kDepthInterpretationMist = 3;

inline constexpr std::int64_t kMistFalloffLinear = 0;
inline constexpr std::int64_t kMistFalloffQuadratic = 1;
inline constexpr std::int64_t kMistFalloffInverseQuadratic = 2;

inline constexpr std::int64_t kDepthBackgroundInfinite = 0;
inline constexpr std::int64_t kDepthBackgroundFar = 1;
inline constexpr std::int64_t kDepthBackgroundUnaffected = 2;

inline constexpr std::int64_t kDefocusModeDepth = 0;
inline constexpr std::int64_t kDefocusModePhysical = 1;
inline constexpr std::int64_t kDefocusModeSizeMap = 2;
inline constexpr std::int64_t kDefocusModeUniform = 3;

inline constexpr std::int64_t kFocusFromDistance = 0;
inline constexpr std::int64_t kFocusFromPoint = 1;

inline constexpr std::int64_t kDefocusBlurNearAndFar = 0;
inline constexpr std::int64_t kDefocusBlurFarOnly = 1;
inline constexpr std::int64_t kDefocusBlurNearOnly = 2;

inline constexpr std::int64_t kSceneUnitMetres = 0;
inline constexpr std::int64_t kSceneUnitCentimetres = 1;
inline constexpr std::int64_t kSceneUnitMillimetres = 2;
inline constexpr std::int64_t kSceneUnitFeet = 3;
inline constexpr std::int64_t kSceneUnitInches = 4;

inline constexpr std::int64_t kBokehDisk = 0;
inline constexpr std::int64_t kBokehPolygon = 1;
inline constexpr std::int64_t kBokehImage = 2;

inline constexpr std::int64_t kDefocusQualityDraft = 0;
inline constexpr std::int64_t kDefocusQualityNormal = 1;
inline constexpr std::int64_t kDefocusQualityHigh = 2;
inline constexpr std::int64_t kDefocusQualityMaximum = 3;

inline constexpr std::int64_t kFogFalloffLinear = 0;
inline constexpr std::int64_t kFogFalloffQuadratic = 1;
inline constexpr std::int64_t kFogFalloffInverseQuadratic = 2;
inline constexpr std::int64_t kFogFalloffExponential = 3;
inline constexpr std::int64_t kFogFalloffExponentialSquared = 4;
inline constexpr std::int64_t kFogFalloffCurve = 5;

// The rings of the defocus gather at each quality: ring k of K holds 8k samples, so a frame's blur
// reads at most 1 + 4K(K + 1) samples a pixel (121, 289, 625, 1089).
inline constexpr std::array<std::int64_t, 4> kDefocusQualityRings{5, 8, 12, 16};

namespace depth_parameters {

using Kind = DepthParameterKind;

inline constexpr std::array<std::string_view, 4> kInterpretations{
    "Z (scene units)", "Inverse depth (1/Z)", "Normalised (0\xe2\x80\x93" "1)",
    "Mist (0\xe2\x80\x93" "1)"};
inline constexpr std::array<std::string_view, 3> kMistFalloffs{"Linear", "Quadratic",
                                                               "Inverse quadratic"};
inline constexpr std::array<std::string_view, 3> kBackgrounds{
    "Infinitely far", "At the far distance", "Unaffected"};
inline constexpr std::array<std::string_view, 4> kModes{"Depth", "Physical lens", "Size map",
                                                        "Uniform"};
inline constexpr std::array<std::string_view, 2> kFocusFrom{"Focus distance",
                                                            "Depth at the focus point"};
inline constexpr std::array<std::string_view, 3> kBlurRegions{"Near and far", "Far only",
                                                              "Near only"};
inline constexpr std::array<std::string_view, 5> kUnits{"Metres", "Centimetres", "Millimetres",
                                                        "Feet", "Inches"};
inline constexpr std::array<std::string_view, 3> kShapes{"Disk", "Polygon (blades)", "Image"};
inline constexpr std::array<std::string_view, 4> kQualities{"Draft", "Normal", "High", "Maximum"};
inline constexpr std::array<std::string_view, 6> kFalloffs{
    "Linear", "Quadratic", "Inverse quadratic", "Exponential", "Exponential squared", "Curve"};

inline constexpr DepthParameterSpec choice(const std::string_view role,
                                           const std::span<const std::string_view> labels,
                                           const std::int64_t value, const std::string_view label) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Choice;
    spec.value = {static_cast<double>(value), 0.0, 0.0, 0.0};
    spec.minimum = 0.0;
    spec.maximum = static_cast<double>(labels.size()) - 1.0;
    spec.choices = labels;
    spec.label = label;
    return spec;
}
inline constexpr DepthParameterSpec integer(const std::string_view role, const std::int64_t value,
                                            const std::int64_t minimum, const std::int64_t maximum,
                                            const std::string_view label,
                                            const std::string_view unit = {}) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Integer;
    spec.value = {static_cast<double>(value), 0.0, 0.0, 0.0};
    spec.minimum = static_cast<double>(minimum);
    spec.maximum = static_cast<double>(maximum);
    spec.label = label;
    spec.softMinimum = spec.minimum;
    spec.softMaximum = spec.maximum;
    spec.step = 1.0;
    spec.decimals = 0;
    spec.unit = unit;
    return spec;
}
inline constexpr DepthParameterSpec boolean(const std::string_view role,
                                            const std::string_view label) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Boolean;
    spec.maximum = 1.0;
    spec.label = label;
    return spec;
}
inline constexpr DepthParameterSpec text(const std::string_view role,
                                         const std::string_view label) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Text;
    spec.label = label;
    return spec;
}
inline constexpr DepthParameterSpec scalar(const std::string_view role, const double value,
                                           const double minimum, const double maximum,
                                           const std::string_view label, const double softMinimum,
                                           const double softMaximum, const double step,
                                           const int decimals, const std::string_view unit = {}) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Scalar;
    spec.value = {value, 0.0, 0.0, 0.0};
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.animated = true;
    spec.label = label;
    spec.softMinimum = softMinimum;
    spec.softMaximum = softMaximum;
    spec.step = step;
    spec.decimals = decimals;
    spec.unit = unit;
    return spec;
}
inline constexpr DepthParameterSpec vector2(const std::string_view role, const double maximum,
                                            const std::string_view label) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Vector2;
    spec.minimum = -maximum;
    spec.maximum = maximum;
    spec.animated = true;
    spec.label = label;
    spec.softMinimum = -2048.0;
    spec.softMaximum = 2048.0;
    spec.step = 1.0;
    spec.decimals = 1;
    spec.unit = "px";
    return spec;
}
inline constexpr DepthParameterSpec color(const std::string_view role,
                                          const std::array<double, 4> value,
                                          const std::string_view label) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Color;
    spec.value = value;
    spec.minimum = -1.0e6;
    spec.maximum = 1.0e6;
    spec.animated = true;
    spec.label = label;
    return spec;
}
inline constexpr DepthParameterSpec curve(const std::string_view role,
                                          const std::string_view label) {
    DepthParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Curve;
    spec.animated = true;
    spec.label = label;
    return spec;
}

inline constexpr double kD = kMaximumDepthDistance;
inline constexpr double kS = kMaximumDefocusSize;

// The depth source's reading, shared by both nodes (`near`, `far`: the defaults differ).
#define BLOOM_DEPTH_SOURCE_PARAMETERS(interpretationDefault, nearDefault, farDefault)             \
    text("depthChannel", "Depth channel"),                                                         \
        choice("interpretation", kInterpretations, interpretationDefault, "Depth is"),             \
        scalar("near", nearDefault, 0.0, kD, "Near distance (0)", 0.0, 100.0, 0.1, 3),             \
        scalar("far", farDefault, 0.0, kD, "Far distance (1)", 0.0, 1000.0, 0.1, 3),               \
        boolean("invert", "Near is white"),                                                        \
        choice("mistFalloff", kMistFalloffs, kMistFalloffLinear, "Mist falloff"),                  \
        choice("background", kBackgrounds, kDepthBackgroundInfinite, "Empty depth")

inline constexpr std::array kZDefocusParameters{
    choice("mode", kModes, kDefocusModeDepth, "Mode"),
    BLOOM_DEPTH_SOURCE_PARAMETERS(kDepthInterpretationZ, 0.1, 100.0),
    choice("units", kUnits, kSceneUnitMetres, "Scene units"),
    choice("focusFrom", kFocusFrom, kFocusFromDistance, "Focus from"),
    scalar("focusDistance", 10.0, 0.0, kD, "Focus distance", 0.0, 100.0, 0.05, 3),
    vector2("focusPoint", 1.0e6, "Focus point"),
    scalar("depthOfField", 0.0, 0.0, kD, "In-focus range", 0.0, 10.0, 0.01, 3),
    scalar("size", 16.0, 0.0, kS, "Blur size", 0.0, 64.0, 0.5, 2, "px"),
    scalar("maxSize", 64.0, 0.0, kS, "Maximum blur", 0.0, 128.0, 0.5, 2, "px"),
    scalar("focalLength", 50.0, 1.0, 5000.0, "Focal length", 8.0, 300.0, 1.0, 1, "mm"),
    scalar("sensorWidth", 36.0, 1.0, 100.0, "Sensor width", 4.0, 70.0, 0.1, 2, "mm"),
    scalar("fStop", 2.8, 0.5, 128.0, "F-stop", 0.7, 32.0, 0.1, 1),
    choice("blurRegion", kBlurRegions, kDefocusBlurNearAndFar, "Blur"),
    integer("edgeExpand", 1, 0, 4, "Edge expand", "px"),
    choice("shape", kShapes, kBokehDisk, "Bokeh shape"),
    integer("blades", 6, 3, 16, "Blades"),
    scalar("rotation", 0.0, -1.0e6, 1.0e6, "Blade rotation", -360.0, 360.0, 1.0, 1, "\xc2\xb0"),
    scalar("roundness", 0.0, 0.0, 1.0, "Blade curvature", 0.0, 1.0, 0.01, 2),
    scalar("highlightGain", 0.0, 0.0, 100.0, "Highlight boost", 0.0, 10.0, 0.05, 2),
    scalar("highlightThreshold", 1.0, 0.0, 1.0e5, "Highlight threshold", 0.0, 4.0, 0.01, 3),
    scalar("chromatic", 0.0, 0.0, 1.0, "Chromatic bokeh", 0.0, 1.0, 0.01, 2),
    choice("quality", kQualities, kDefocusQualityNormal, "Quality"),
};

inline constexpr std::array kDepthFogParameters{
    BLOOM_DEPTH_SOURCE_PARAMETERS(kDepthInterpretationMist, 0.0, 1.0),
    color("color", {0.75, 0.8, 0.85, 1.0}, "Fog color"),
    scalar("amount", 1.0, 0.0, 1.0, "Fog amount", 0.0, 1.0, 0.01, 2),
    choice("falloff", kFalloffs, kFogFalloffLinear, "Falloff"),
    scalar("start", 0.0, 0.0, kD, "Fog start", 0.0, 100.0, 0.01, 3),
    scalar("end", 1.0, 0.0, kD, "Fog end", 0.0, 100.0, 0.01, 3),
    scalar("density", 1.0, 0.0, 1.0e6, "Density", 0.0, 4.0, 0.01, 3),
    curve("curve", "Falloff curve"),
    boolean("fillEmpty", "Fog transparent areas"),
};

#undef BLOOM_DEPTH_SOURCE_PARAMETERS

} // namespace depth_parameters

[[nodiscard]] constexpr bool isDepthEffectNodeType(const std::string_view typeId) noexcept {
    return typeId == kZDefocusNodeType || typeId == kDepthFogNodeType;
}

// The parameters of a depth effect node in definition order, or an empty span.
[[nodiscard]] constexpr std::span<const DepthParameterSpec>
depthEffectParameters(const std::string_view typeId) noexcept {
    if (typeId == kZDefocusNodeType)
        return depth_parameters::kZDefocusParameters;
    if (typeId == kDepthFogNodeType)
        return depth_parameters::kDepthFogParameters;
    return {};
}

// The spec of a "<type id>.<role>" schema key, or null.
[[nodiscard]] constexpr const DepthParameterSpec*
depthParameterSpec(const std::string_view schemaKey) noexcept {
    for (const auto type : {kZDefocusNodeType, kDepthFogNodeType}) {
        if (schemaKey.size() <= type.size() + 1 || !schemaKey.starts_with(type) ||
            schemaKey[type.size()] != '.')
            continue;
        const auto role = schemaKey.substr(type.size() + 1);
        for (const auto& spec : depthEffectParameters(type))
            if (spec.role == role)
                return &spec;
    }
    return nullptr;
}

[[nodiscard]] constexpr bool isDepthScalarAnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = depthParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Scalar;
}
[[nodiscard]] constexpr bool isDepthVec2AnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = depthParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Vector2;
}
[[nodiscard]] constexpr bool isDepthColorAnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = depthParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Color;
}
[[nodiscard]] constexpr bool isDepthPathAnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = depthParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Curve;
}
// A constant choice (Choice, Integer, Boolean, Text): no socket and no animation.
[[nodiscard]] constexpr bool isDepthConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = depthParameterSpec(key);
    return spec != nullptr &&
           (spec->kind == DepthParameterKind::Choice || spec->kind == DepthParameterKind::Integer ||
            spec->kind == DepthParameterKind::Boolean || spec->kind == DepthParameterKind::Text);
}
// Whether the Scalar `value` lies in the depth parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isDepthScalarWithinDomain(const std::string_view key,
                                                       const double value) noexcept {
    const auto* spec = depthParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document

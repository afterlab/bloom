#pragma once

// #484 Motion blur: the durable parameter vocabulary of the motion-blur nodes. One shutter model is
// shared by every node: a shutter angle (180 degrees is half a frame), where the shutter opens
// (centred on the frame, at its start, ending at it, or at a custom phase), the shutter curve that
// weights the open time (box, triangle, Gaussian or a custom curve) and the sample quality (Draft
// 4, Standard 16, High 64, or Final: a power of two adapted to the widest blur in pixels within a
// minimum and maximum). docs/architecture/motion-blur.md is the specification.
//
// The nodes (all Filters):
//   Motion Blur                 sub-frame accumulation of everything upstream; footage between
//                               frames is held, blurred by its optical flow, or by its vectors.
//   Vector Motion Blur          blur by a motion-vector pass, in any renderer's convention.
//   Optical Flow                dense flow between neighbour frames, with confidence, occlusion
//                               and a cut detection diagnostic.
//   Motion Blur (Optical Flow)  blur by the footage's own optical flow.
//   Echo                        frame blend, echo and trails with Posterize Time.
//   Frame Rate Hold             holds frames to a target rate, with an optional seeded irregular
//                               cadence of doubled and dropped frames (Time group).
//   Directional, Spin, Zoom     the path sampler along a line, an arc or a ray.
// Transform 2D takes the same shutter as its own motion blur.
//
// Schema keys are "<type id>.<role>", as the depth effects' (depth_effect_parameters.hpp, whose
// spec type and builders these tables reuse). Every Scalar and Vector 2 marked animated is
// animatable, drivable and has a socket of its own; every Choice, Integer, Boolean, Text and the
// shutter Curve is a constant.

#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/image_math_parameters.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kMotionBlurNodeType = "bloom.motion-blur";
inline constexpr std::string_view kVectorMotionBlurNodeType = "bloom.vector-motion-blur";
inline constexpr std::string_view kOpticalFlowNodeType = "bloom.optical-flow";
inline constexpr std::string_view kOpticalFlowMotionBlurNodeType = "bloom.optical-flow-motion-blur";
inline constexpr std::string_view kEchoNodeType = "bloom.echo";
// Frame Rate Hold's type id; the Time Values group already owns "bloom.frame-rate" (the composition's
// rate as a number).
inline constexpr std::string_view kFrameRateHoldNodeType = "bloom.frame-rate-hold";
inline constexpr std::string_view kDirectionalBlurNodeType = "bloom.directional-blur";
inline constexpr std::string_view kSpinBlurNodeType = "bloom.spin-blur";
inline constexpr std::string_view kZoomBlurNodeType = "bloom.zoom-blur";
// #512 Camera Shake (Transform): a seeded handheld, impact or gate-weave shake applied as a
// Transform 2D, with the same shutter for its motion blur. Its parameters live in this vocabulary
// (camera_shake_parameters below) but it is not a motion-blur node: isMotionBlurNodeType is false.
inline constexpr std::string_view kCameraShakeNodeType = "bloom.camera-shake";
// Besides its Image, the shake publishes what it applied: `offset` (Vector 2, pixels) and `roll`
// (Scalar, degrees clockwise).
inline constexpr std::string_view kCameraShakeOffsetPort = "offset";
inline constexpr std::string_view kCameraShakeRollPort = "roll";
inline constexpr std::uint32_t kMotionBlurNodeSchemaVersion = 1;

// The ports. Vector Motion Blur reads `vectors` (an Image or a channel set), an optional `depth`
// and an optional `matte`; Motion Blur (Optical Flow) an optional `matte` and `guide`; Optical
// Flow gives `forward`, `backward`, `confidence`, `occlusion` and `cut`.
inline constexpr std::string_view kMotionInputPort = "input";
inline constexpr std::string_view kMotionOutputPort = "image";
inline constexpr std::string_view kMotionVectorsPort = "vectors";
inline constexpr std::string_view kMotionDepthPort = "depth";
inline constexpr std::string_view kMotionMattePort = "matte";
inline constexpr std::string_view kMotionGuidePort = "guide";
inline constexpr std::string_view kFlowForwardPort = "forward";
inline constexpr std::string_view kFlowBackwardPort = "backward";
inline constexpr std::string_view kFlowConfidencePort = "confidence";
inline constexpr std::string_view kFlowOcclusionPort = "occlusion";
inline constexpr std::string_view kFlowCutPort = "cut";

// --- Stored values of the choices ---------------------------------------------------------------

inline constexpr std::int64_t kShutterOffsetCentred = 0;
inline constexpr std::int64_t kShutterOffsetStart = 1;
inline constexpr std::int64_t kShutterOffsetEnd = 2;
inline constexpr std::int64_t kShutterOffsetCustom = 3;

inline constexpr std::int64_t kShutterCurveBox = 0;
inline constexpr std::int64_t kShutterCurveTriangle = 1;
inline constexpr std::int64_t kShutterCurveGaussian = 2;
inline constexpr std::int64_t kShutterCurveCustom = 3;

inline constexpr std::int64_t kMotionQualityDraft = 0;
inline constexpr std::int64_t kMotionQualityStandard = 1;
inline constexpr std::int64_t kMotionQualityHigh = 2;
inline constexpr std::int64_t kMotionQualityFinal = 3;

// Motion Blur: what footage (an Image sequence or Video) does between frames.
inline constexpr std::int64_t kFootageAuto = 0;
inline constexpr std::int64_t kFootageHold = 1;
inline constexpr std::int64_t kFootageFlow = 2;
inline constexpr std::int64_t kFootageVectors = 3;

// Vector Motion Blur: the renderer convention a vector pass is written in.
inline constexpr std::int64_t kVectorConventionBloom = 0;
inline constexpr std::int64_t kVectorConventionNuke = 1;
inline constexpr std::int64_t kVectorConventionRsmb = 2;
inline constexpr std::int64_t kVectorConventionBlender = 3;
inline constexpr std::int64_t kVectorConventionArnold = 4;
inline constexpr std::int64_t kVectorConventionVray = 5;
inline constexpr std::int64_t kVectorConventionRedshift = 6;
inline constexpr std::int64_t kVectorConventionKarma = 7;
inline constexpr std::int64_t kVectorConventionCustom = 8;

inline constexpr std::int64_t kVectorLayoutSingle = 0;
inline constexpr std::int64_t kVectorLayoutForwardBackward = 1;
inline constexpr std::int64_t kVectorLayoutBlender = 2;

inline constexpr std::int64_t kVectorUnitsPixels = 0;
inline constexpr std::int64_t kVectorUnitsNdc = 1;

// Vector Motion Blur's reconstruction: Automatic is Gather at Draft and Standard, Layered at High
// and Final.
inline constexpr std::int64_t kVectorAlgorithmAuto = 0;
inline constexpr std::int64_t kVectorAlgorithmGather = 1;
inline constexpr std::int64_t kVectorAlgorithmLayered = 2;

// Optical flow: Automatic is DIS at Draft and TV-L1 otherwise.
inline constexpr std::int64_t kFlowMethodAuto = 0;
inline constexpr std::int64_t kFlowMethodDis = 1;
inline constexpr std::int64_t kFlowMethodTvl1 = 2;

inline constexpr std::int64_t kFlowBorderBlack = 0;
inline constexpr std::int64_t kFlowBorderExtend = 1;
inline constexpr std::int64_t kFlowBorderWrap = 2;

// Motion Blur (Optical Flow): how the background behind a separated foreground is filled.
inline constexpr std::int64_t kHoleFillOutsideIn = 0;
inline constexpr std::int64_t kHoleFillTemporal = 1;

inline constexpr std::int64_t kEchoModeEcho = 0;
inline constexpr std::int64_t kEchoModeFrameBlend = 1;

inline constexpr std::int64_t kEchoOperatorAdd = 0;
inline constexpr std::int64_t kEchoOperatorMaximum = 1;
inline constexpr std::int64_t kEchoOperatorMinimum = 2;
inline constexpr std::int64_t kEchoOperatorScreen = 3;
inline constexpr std::int64_t kEchoOperatorOver = 4;
inline constexpr std::int64_t kEchoOperatorUnder = 5;

// Camera Shake's style and edges.
inline constexpr std::int64_t kShakeModeHandheld = 0;
inline constexpr std::int64_t kShakeModeImpact = 1;
inline constexpr std::int64_t kShakeModeGateWeave = 2;

inline constexpr std::int64_t kMaximumMotionSamples = 64;
inline constexpr std::int64_t kMaximumEchoes = 64;
inline constexpr double kMaximumMotionBlurPixels = 4096.0;

namespace motion_parameters {

using depth_parameters::boolean;
using depth_parameters::choice;
using depth_parameters::curve;
using depth_parameters::integer;
using depth_parameters::scalar;
using depth_parameters::text;
using depth_parameters::vector2;

inline constexpr std::array<std::string_view, 4> kOffsets{"Centred", "Start on frame",
                                                          "End on frame", "Custom"};
inline constexpr std::array<std::string_view, 4> kCurves{"Box", "Triangle", "Gaussian", "Custom"};
inline constexpr std::array<std::string_view, 4> kQualities{"Draft", "Standard", "High", "Final"};
inline constexpr std::array<std::string_view, 4> kFootage{"Automatic", "Hold", "Optical flow",
                                                          "Motion vectors"};
inline constexpr std::array<std::string_view, 9> kConventions{
    "Bloom", "Forward and backward", "RSMB", "Previous and next", "Arnold", "V-Ray", "Redshift", "Karma",
    "Custom"};
inline constexpr std::array<std::string_view, 3> kLayouts{
    "One vector", "Forward and backward", "Previous and next"};
inline constexpr std::array<std::string_view, 2> kUnits{"Pixels", "Normalised (-1 to 1)"};
inline constexpr std::array<std::string_view, 3> kAlgorithms{"Automatic", "Gather",
                                                             "Layered splat"};
inline constexpr std::array<std::string_view, 3> kMethods{"Automatic", "DIS", "TV-L1"};
inline constexpr std::array<std::string_view, 3> kBorders{"Black", "Extend", "Wrap (360)"};
inline constexpr std::array<std::string_view, 2> kHoleFills{"Outside-in", "Temporal"};
inline constexpr std::array<std::string_view, 2> kEchoModes{"Echo", "Frame blend"};
inline constexpr std::array<std::string_view, 6> kOperators{"Add",    "Maximum", "Minimum",
                                                            "Screen", "Over",    "Under"};

inline constexpr DepthParameterSpec constantScalar(const std::string_view role, const double value,
                                                   const double minimum, const double maximum,
                                                   const std::string_view label,
                                                   const double softMinimum,
                                                   const double softMaximum, const double step,
                                                   const int decimals,
                                                   const std::string_view unit = {}) {
    auto spec = scalar(role, value, minimum, maximum, label, softMinimum, softMaximum, step,
                       decimals, unit);
    spec.animated = false;
    return spec;
}

inline constexpr DepthParameterSpec constantCurve(const std::string_view role,
                                                  const std::string_view label) {
    auto spec = curve(role, label);
    spec.animated = false;
    return spec;
}

// The shutter, every node's (`quality` default differs).
#define BLOOM_SHUTTER_PARAMETERS(qualityDefault)                                                   \
    constantScalar("shutterAngle", 180.0, 0.0, 1440.0, "Shutter angle", 0.0, 360.0, 1.0, 1,       \
                   "\xc2\xb0"),                                                                    \
        choice("shutterOffset", kOffsets, kShutterOffsetCentred, "Shutter opens"),                 \
        constantScalar("shutterPhase", 0.0, -1440.0, 1440.0, "Shutter phase", -360.0, 360.0, 1.0, \
                       1, "\xc2\xb0"),                                                             \
        choice("shutterCurve", kCurves, kShutterCurveBox, "Shutter curve"),                        \
        constantCurve("shutterShape", "Custom shutter curve"),                                             \
        choice("quality", kQualities, qualityDefault, "Quality"),                                  \
        integer("minSamples", 8, 1, kMaximumMotionSamples, "Minimum samples"),                     \
        integer("maxSamples", 64, 1, kMaximumMotionSamples, "Maximum samples")

inline constexpr std::array kMotionBlurParameters{
    choice("footage", kFootage, kFootageAuto, "Footage between frames"),
    BLOOM_SHUTTER_PARAMETERS(kMotionQualityStandard),
    text("vectorLayer", "Motion vector layer"),
};

inline constexpr std::array kVectorMotionBlurParameters{
    choice("convention", kConventions, kVectorConventionBloom, "Vectors from"),
    text("vectorChannels", "Vector channels"),
    constantScalar("vectorScale", 1.0, -1.0e6, 1.0e6, "Vector scale", -4.0, 4.0, 0.01, 3),
    constantScalar("vectorOffset", 0.0, -1.0e6, 1.0e6, "Vector offset", -1.0, 1.0, 0.01, 3),
    constantScalar("maxDisplace", 2048.0, 1.0, 1.0e6, "MaxDisplace", 1.0, 4096.0, 1.0, 0, "px"),
    boolean("flipY", "Y points up"),
    choice("layout", kLayouts, kVectorLayoutSingle, "Vector layout"),
    choice("units", kUnits, kVectorUnitsPixels, "Vector units"),
    boolean("alphaValid", "Alpha marks valid vectors"),
    BLOOM_SHUTTER_PARAMETERS(kMotionQualityStandard),
    boolean("jitter", "Jitter samples"),
    constantScalar("maxBlur", 512.0, 0.0, kMaximumMotionBlurPixels, "Maximum blur", 0.0, 1024.0,
                   1.0, 0, "px"),
    choice("algorithm", kAlgorithms, kVectorAlgorithmAuto, "Reconstruction"),
    boolean("curved", "Curved motion"),
    text("depthChannel", "Depth channel"),
    constantScalar("softDepth", 1.0, 1.0e-6, 1.0e9, "Depth softness", 0.01, 10.0, 0.01, 3),
};

inline constexpr std::array kOpticalFlowParameters{
    choice("method", kMethods, kFlowMethodAuto, "Method"),
    choice("quality", kQualities, kMotionQualityStandard, "Quality"),
    choice("border", kBorders, kFlowBorderExtend, "Frame edges"),
    boolean("cutDetection", "Detect cuts"),
    constantScalar("cutThreshold", 24.0, 0.0, 255.0, "Cut threshold", 0.0, 64.0, 0.5, 1),
};

inline constexpr std::array kOpticalFlowMotionBlurParameters{
    constantScalar("amount", 0.5, 0.0, 4.0, "Amount", 0.0, 2.0, 0.01, 2),
    choice("method", kMethods, kFlowMethodAuto, "Method"),
    choice("quality", kQualities, kMotionQualityStandard, "Quality"),
    choice("border", kBorders, kFlowBorderExtend, "Frame edges"),
    boolean("jitter", "Jitter samples"),
    constantScalar("maxBlur", 512.0, 0.0, kMaximumMotionBlurPixels, "Maximum blur", 0.0, 1024.0,
                   1.0, 0, "px"),
    choice("holeFill", kHoleFills, kHoleFillOutsideIn, "Background fill"),
    constantScalar("matteShrink", 0.0, 0.0, 256.0, "Matte shrink", 0.0, 16.0, 0.5, 1, "px"),
    constantScalar("alphaBoost", 1.0, 0.0, 16.0, "Alpha boost", 0.0, 4.0, 0.01, 2),
};

inline constexpr std::array kEchoParameters{
    choice("mode", kEchoModes, kEchoModeEcho, "Mode"),
    integer("echoes", 4, 1, kMaximumEchoes, "Echoes"),
    constantScalar("interval", -1.0, -240.0, 240.0, "Time between echoes", -24.0, 24.0, 1.0, 2,
                   "frames"),
    constantScalar("intensity", 1.0, 0.0, 16.0, "Starting intensity", 0.0, 1.0, 0.01, 2),
    constantScalar("decay", 0.5, 0.0, 1.0, "Decay", 0.0, 1.0, 0.01, 2),
    choice("operator", kOperators, kEchoOperatorAdd, "Echo operator"),
    boolean("posterize", "Posterize time"),
    constantScalar("posterizeRate", 12.0, 0.1, 240.0, "Posterize rate", 1.0, 60.0, 1.0, 2, "fps"),
};

// Frame Rate Hold: the rate its Posterize Time-style time mapping holds frames to and, when the
// cadence is irregular, how often a held frame is a repeat of the one before it (so the source frame
// that would have followed is dropped). The seed is per node, as Camera Shake's.
inline constexpr std::array kFrameRateHoldParameters{
    constantScalar("rate", 12.0, 0.1, 240.0, "Frame rate", 1.0, 60.0, 1.0, 2, "fps"),
    boolean("irregular", "Irregular cadence"),
    constantScalar("amount", 0.2, 0.0, 1.0, "Drop and double", 0.0, 1.0, 0.01, 2),
    integer("seed", 1, 0, 999999, "Seed"),
};

inline constexpr std::array kDirectionalBlurParameters{
    scalar("angle", 0.0, -1.0e6, 1.0e6, "Direction", -180.0, 180.0, 1.0, 1, "\xc2\xb0"),
    scalar("length", 32.0, 0.0, kMaximumMotionBlurPixels, "Length", 0.0, 256.0, 0.5, 1, "px"),
    choice("shutterOffset", kOffsets, kShutterOffsetCentred, "Blur from"),
    choice("shutterCurve", kCurves, kShutterCurveBox, "Falloff"),
    constantCurve("shutterShape", "Custom falloff"),
    choice("quality", kQualities, kMotionQualityFinal, "Quality"),
    integer("minSamples", 8, 1, kMaximumMotionSamples, "Minimum samples"),
    integer("maxSamples", 64, 1, kMaximumMotionSamples, "Maximum samples"),
    boolean("jitter", "Jitter samples"),
};

#define BLOOM_RADIAL_PARAMETERS(amountRole, amountDefault, amountMin, amountMax, amountLabel,     \
                                softMin, softMax, unit)                                            \
    vector2("centre", 1.0e6, "Centre"),                                                            \
        scalar(amountRole, amountDefault, amountMin, amountMax, amountLabel, softMin, softMax,     \
               0.01, 2, unit),                                                                     \
        choice("shutterOffset", kOffsets, kShutterOffsetCentred, "Blur from"),                     \
        choice("shutterCurve", kCurves, kShutterCurveBox, "Falloff"),                              \
        constantCurve("shutterShape", "Custom falloff"),                                                   \
        choice("quality", kQualities, kMotionQualityFinal, "Quality"),                             \
        integer("minSamples", 8, 1, kMaximumMotionSamples, "Minimum samples"),                     \
        integer("maxSamples", 64, 1, kMaximumMotionSamples, "Maximum samples"),                    \
        boolean("jitter", "Jitter samples"),                                                       \
        constantScalar("maxBlur", 512.0, 0.0, kMaximumMotionBlurPixels, "Maximum blur", 0.0,      \
                       1024.0, 1.0, 0, "px")

inline constexpr std::array kSpinBlurParameters{
    BLOOM_RADIAL_PARAMETERS("amount", 10.0, -3600.0, 3600.0, "Spin", -90.0, 90.0, "\xc2\xb0")};
inline constexpr std::array kZoomBlurParameters{
    BLOOM_RADIAL_PARAMETERS("amount", 0.1, -0.95, 16.0, "Zoom", -0.95, 1.0, "")};

inline constexpr std::array<std::string_view, 3> kShakeModes{"Handheld", "Impact", "Gate weave"};

// Auto overscan is on by default.
inline constexpr DepthParameterSpec shakeOverscan() {
    auto spec = boolean("overscan", "Auto overscan");
    spec.value = {1.0, 0.0, 0.0, 0.0};
    return spec;
}

// Transform 2D's motion blur: off by default; its shutter as the nodes'.
inline constexpr std::array kTransformMotionBlurParameters{
    boolean("motionBlur", "Motion blur"),
    BLOOM_SHUTTER_PARAMETERS(kMotionQualityFinal),
    boolean("jitter", "Jitter samples"),
};

// #512 Camera Shake. The amplitudes, the roll, the frequency, the roughness and the smoothing are
// animatable and have sockets; the style, octaves, seed, loop and overscan choose the arithmetic
// and are constants. The motion blur follows Transform 2D's: off by default, the same shutter.
inline constexpr std::array kCameraShakeParameters{
    choice("mode", kShakeModes, kShakeModeHandheld, "Style"),
    scalar("amplitudeX", 10.0, 0.0, 4096.0, "Amplitude X", 0.0, 100.0, 0.5, 1, "px"),
    scalar("amplitudeY", 10.0, 0.0, 4096.0, "Amplitude Y", 0.0, 100.0, 0.5, 1, "px"),
    scalar("rollAmount", 0.5, 0.0, 90.0, "Roll amount", 0.0, 5.0, 0.05, 2, "\xc2\xb0"),
    scalar("frequency", 2.0, 0.05, 30.0, "Frequency", 0.1, 10.0, 0.05, 2, "Hz"),
    integer("octaves", 2, 1, 4, "Octaves"),
    scalar("roughness", 0.35, 0.0, 1.0, "Roughness", 0.0, 1.0, 0.01, 2),
    scalar("smoothing", 0.0, 0.0, 1.0, "Smoothing", 0.0, 1.0, 0.01, 2),
    integer("seed", 1, 0, 999999, "Seed"),
    boolean("loop", "Loop"),
    constantScalar("loopLength", 4.0, 0.1, 600.0, "Loop length", 0.5, 30.0, 0.1, 1, "s"),
    shakeOverscan(),
    boolean("motionBlur", "Motion blur"),
    BLOOM_SHUTTER_PARAMETERS(kMotionQualityFinal),
    boolean("jitter", "Jitter samples"),
};

#undef BLOOM_SHUTTER_PARAMETERS
#undef BLOOM_RADIAL_PARAMETERS

} // namespace motion_parameters

inline constexpr std::string_view kTransform2DNodeType = "bloom.transform-2d";

[[nodiscard]] constexpr bool isMotionBlurNodeType(const std::string_view typeId) noexcept {
    return typeId == kMotionBlurNodeType || typeId == kVectorMotionBlurNodeType ||
           typeId == kOpticalFlowNodeType || typeId == kOpticalFlowMotionBlurNodeType ||
           typeId == kEchoNodeType || typeId == kFrameRateHoldNodeType ||
           typeId == kDirectionalBlurNodeType || typeId == kSpinBlurNodeType || typeId == kZoomBlurNodeType;
}

// The parameters of a motion-blur node in definition order (Transform 2D: its motion blur's),
// or an empty span.
[[nodiscard]] constexpr std::span<const DepthParameterSpec>
motionBlurParameters(const std::string_view typeId) noexcept {
    if (typeId == kMotionBlurNodeType)
        return motion_parameters::kMotionBlurParameters;
    if (typeId == kVectorMotionBlurNodeType)
        return motion_parameters::kVectorMotionBlurParameters;
    if (typeId == kOpticalFlowNodeType)
        return motion_parameters::kOpticalFlowParameters;
    if (typeId == kOpticalFlowMotionBlurNodeType)
        return motion_parameters::kOpticalFlowMotionBlurParameters;
    if (typeId == kEchoNodeType)
        return motion_parameters::kEchoParameters;
    if (typeId == kFrameRateHoldNodeType)
        return motion_parameters::kFrameRateHoldParameters;
    if (typeId == kDirectionalBlurNodeType)
        return motion_parameters::kDirectionalBlurParameters;
    if (typeId == kSpinBlurNodeType)
        return motion_parameters::kSpinBlurParameters;
    if (typeId == kZoomBlurNodeType)
        return motion_parameters::kZoomBlurParameters;
    if (typeId == kTransform2DNodeType)
        return motion_parameters::kTransformMotionBlurParameters;
    if (typeId == kCameraShakeNodeType)
        return motion_parameters::kCameraShakeParameters;
    return {};
}

// The spec of a "<type id>.<role>" schema key, or null.
[[nodiscard]] constexpr const DepthParameterSpec*
motionParameterSpec(const std::string_view schemaKey) noexcept {
    for (const auto type :
         {kMotionBlurNodeType, kVectorMotionBlurNodeType, kOpticalFlowNodeType,
          kOpticalFlowMotionBlurNodeType, kEchoNodeType, kFrameRateHoldNodeType,
          kDirectionalBlurNodeType, kSpinBlurNodeType, kZoomBlurNodeType, kTransform2DNodeType, kCameraShakeNodeType}) {
        if (schemaKey.size() <= type.size() + 1 || !schemaKey.starts_with(type) ||
            schemaKey[type.size()] != '.')
            continue;
        const auto role = schemaKey.substr(type.size() + 1);
        for (const auto& spec : motionBlurParameters(type))
            if (spec.role == role)
                return &spec;
    }
    return nullptr;
}

// The depth, motion and image pixel tables together: the generic hooks every
// parameter-vocabulary site reads.
[[nodiscard]] constexpr const DepthParameterSpec*
effectParameterSpec(const std::string_view schemaKey) noexcept {
    const auto* depth = depthParameterSpec(schemaKey);
    if (depth != nullptr)
        return depth;
    const auto* motion = motionParameterSpec(schemaKey);
    return motion != nullptr ? motion : imagePixelParameterSpec(schemaKey);
}

[[nodiscard]] constexpr bool isMotionScalarAnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = motionParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Scalar && spec->animated;
}
[[nodiscard]] constexpr bool isMotionVec2AnimatableSchemaKey(const std::string_view key) noexcept {
    const auto* spec = motionParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Vector2;
}
// A constant (Choice, Integer, Boolean, Text, a constant Scalar and the shutter Curve): no socket
// and no animation.
[[nodiscard]] constexpr bool isMotionConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = motionParameterSpec(key);
    return spec != nullptr && !(spec->animated && (spec->kind == DepthParameterKind::Scalar ||
                                                   spec->kind == DepthParameterKind::Vector2));
}
[[nodiscard]] constexpr bool isMotionScalarWithinDomain(const std::string_view key,
                                                        const double value) noexcept {
    const auto* spec = motionParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document

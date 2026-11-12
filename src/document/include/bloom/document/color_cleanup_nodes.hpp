#pragma once

// #518 Tone Map, #517 Color Matrix, #523 HSL Qualifier, #519 Quantize and #527 Dither: the durable
// vocabulary of the
// per-pixel color nodes. Every node takes an Image on `input`; Tone Map and Color Matrix give an
// Image on `image` (and take the optional effect mask every grade takes), the HSL Qualifier gives a
// Mask on `mask`. Schema keys are "<type id>.<role>", like the depth effects'. Every Scalar is
// animatable, drivable and has a socket of its own; every Choice and Boolean is a constant with no
// socket. docs/architecture/image-filters.md, "Tone Map", "Color Matrix", "HSL Qualifier",
// "Quantize" and "Dither", are the specification.
//
//   Dither (`bloom.dither`) adds a tiled threshold pattern to the color and quantizes the sum to
//     `bitDepth` (#676), so a gradient breaks into fine noise instead of bands. `pattern` Ordered (a
//     Bayer matrix of `matrixSize` 2, 4, 8 or 16) or Blue noise (a generated 64 x 64
//     void-and-cluster table). `amplitude` is the pattern's peak-to-peak size in steps (LSBs) of
//     `bitDepth` (1 to 6, 8, 10, 12 or 16 bit; 1 LSB is 1 / (2^bits - 1)), measured and quantized in
//     `domain` Display (the straight value's sRGB encoding, where a delivery's code values are even)
//     or Stored (the working-space value itself). `animation` Every Frame draws a new placement for
//     each frame; Held keeps one. `seed` picks the placement; `monochrome` gives every channel red's.
//     Alpha never changes; a pixel with alpha 0 is left as it is; amplitude 0 quantizes without
//     dither.
//
//   Quantize (`bloom.quantize`) works on the straight working-space color and, in Threshold mode,
//     leaves alpha alone. `mode` Threshold or Posterize. Threshold: `basis` Luma (Rec. 709 luma, one
//     result for all three channels) or Per channel; a value at or above `level` is 1 and below it 0,
//     or with `softness` above 0 a smooth ramp of that width centred on the level (smoothstep); `invert`
//     swaps white and black. Posterize: each straight channel, clamped to [0, 1], snaps to the nearest
//     of `levels` equally spaced values (exact halves round up); `linkLevels` gives red, green and blue
//     the master `levels`, off they read `redLevels`, `greenLevels` and `blueLevels`; `posterizeAlpha`
//     also snaps alpha to `alphaLevels`.
//
//   Color Matrix (`bloom.color-matrix`) mixes the straight channels with a 3 x 3 or 4 x 4 matrix
//     (`rr` is the red result's share of the red input, `rg` of green, and so on; the alpha row
//     and column count only at 4 x 4), after reading the input as a color model and before writing
//     the result as one (RGB, HSL, HSV, YCbCr or Lab). A `preset` other than Custom replaces the
//     matrix' elements with its own.
//
//   HSL Qualifier (`bloom.hsl-qualifier`) gives a Mask on `mask`: where the straight color's hue
//     (degrees: the `hueCenter` and the full `hueWidth` around it, wrapping the turn), HSV saturation
//     and Rec. 709 luma each lie in their range. Every range has a softness: the selection ramps
//     linearly from 0 to 1 over that distance outside the range. The three selections multiply, are
//     inverted by `invert` and weighted by the pixel's alpha. A pixel with no saturation has no hue and
//     is selected by the hue range only if hue 0 is.
//
//   Tone Map (`bloom.tone-map`) works on the straight scene-linear color and leaves alpha alone.
//     In order: gamut compression (off by default), shadow recovery, highlight roll-off.
//     method           Each channel (every channel rolls off on its own) or Brightest channel (the
//                      brightest channel rolls off and the others follow by the same factor, so hue
//                      holds).
//     knee, ceiling    a value up to `knee` is untouched; above it the value bends smoothly toward
//                      `ceiling` and never reaches it. A ceiling at or below the knee is a hard clip
//                      at the ceiling.
//     shadows          a gain on dark values: 1 doubles them near black, -1 takes them to black; it
//                      fades out toward `shadowRange`, the scene-linear value at which the gain has
//                      halved.
//     gamutCompress    pulls colors with a negative channel (outside the gamut) back inside: the
//                      distance from the brightest channel, as a fraction of it, is left alone up to
//                      `gamutThreshold` and compressed above it, so a distance of `gamutLimit`
//                      reaches the edge.

#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kToneMapNodeType = "bloom.tone-map";
inline constexpr std::string_view kColorMatrixNodeType = "bloom.color-matrix";
inline constexpr std::string_view kHslQualifierNodeType = "bloom.hsl-qualifier";
inline constexpr std::string_view kQuantizeNodeType = "bloom.quantize";
inline constexpr std::string_view kDitherNodeType = "bloom.dither";
inline constexpr std::string_view kColorCleanupMaskOutputPort = "mask";
inline constexpr std::uint32_t kColorCleanupNodeSchemaVersion = 1;

inline constexpr std::string_view kColorCleanupInputPort = "input";
inline constexpr std::string_view kColorCleanupImageOutputPort = "image";

// Stored values of Tone Map's method.
inline constexpr std::int64_t kToneMapEachChannel = 0;
inline constexpr std::int64_t kToneMapBrightestChannel = 1;

// Stored values of Color Matrix's preset, size and color models.
inline constexpr std::int64_t kColorMatrixCustom = 0;
inline constexpr std::int64_t kColorMatrixPresetCount = 7;
inline constexpr std::int64_t kColorMatrixSize3 = 0;
inline constexpr std::int64_t kColorMatrixSize4 = 1;
inline constexpr std::int64_t kColorModelRgb = 0;
inline constexpr std::int64_t kColorModelHsl = 1;
inline constexpr std::int64_t kColorModelHsv = 2;
inline constexpr std::int64_t kColorModelYCbCr = 3;
inline constexpr std::int64_t kColorModelLab = 4;
inline constexpr std::int64_t kColorModelCount = 5;
// Stored values of Quantize's mode and threshold basis.
inline constexpr std::int64_t kQuantizeThreshold = 0;
inline constexpr std::int64_t kQuantizePosterize = 1;
inline constexpr std::int64_t kQuantizeBasisLuma = 0;
inline constexpr std::int64_t kQuantizeBasisPerChannel = 1;
// The levels of a posterize: at least two, at most 256 (an 8-bit step).
inline constexpr double kMinimumQuantizeLevels = 2.0;
inline constexpr double kMaximumQuantizeLevels = 256.0;
// A threshold level or softness is finite and within this.
inline constexpr double kMaximumQuantizeThreshold = 16.0;
// Stored values of Dither's pattern, domain and animation; its matrix sizes 2 << value and bit depths.
inline constexpr std::int64_t kDitherOrdered = 0;
inline constexpr std::int64_t kDitherBlueNoise = 1;
inline constexpr std::int64_t kDitherDisplay = 0;
inline constexpr std::int64_t kDitherStored = 1;
inline constexpr std::int64_t kDitherEveryFrame = 0;
inline constexpr std::int64_t kDitherHeld = 1;
// Stored values 0 to 3 are the depths of the first version; the coarse depths that make the
// quantization visible were appended after them (#676), so saved projects keep their depth.
inline constexpr std::array<int, 10> kDitherBitDepths{8, 10, 12, 16, 1, 2, 3, 4, 5, 6};
// The default target depth: 4 bits, sixteen levels, where the dither is plainly visible.
inline constexpr std::int64_t kDitherDefaultBitDepth = 7;
// The Bayer side of a stored matrix size, or 0.
[[nodiscard]] constexpr int ditherBayerSide(const std::int64_t value) noexcept {
    return value >= 0 && value <= 3 ? 2 << value : 0;
}
// The bits of a stored bit depth, or 0.
[[nodiscard]] constexpr int ditherBits(const std::int64_t value) noexcept {
    return value >= 0 && value < static_cast<std::int64_t>(kDitherBitDepths.size())
               ? kDitherBitDepths[static_cast<std::size_t>(value)]
               : 0;
}
// An amplitude is finite and within this many LSBs.
inline constexpr double kMaximumDitherAmplitude = 16.0;
// A matrix element is finite and within this.
inline constexpr double kMaximumColorMatrixElement = 100.0;

// A value, knee, ceiling or range is finite and within this.
inline constexpr double kMaximumToneMapValue = 1.0e6;

namespace color_cleanup_parameters {

using namespace depth_parameters;

inline constexpr std::array<std::string_view, 2> kToneMapMethods{"Each channel",
                                                                  "Brightest channel"};

inline constexpr std::array<std::string_view, 7> kMatrixPresets{
    "Custom matrix",       "Identity",           "Gray (luminance)", "Swap red and blue",
    "Swap red and green",  "Swap green and blue", "Sepia"};
inline constexpr std::array<std::string_view, 2> kMatrixSizes{"3 x 3 (color)",
                                                              "4 x 4 (color and alpha)"};
inline constexpr std::array<std::string_view, 5> kColorModels{"RGB", "HSL", "HSV", "YCbCr", "Lab"};

inline constexpr std::array<std::string_view, 2> kQuantizeModes{"Threshold", "Posterize"};
inline constexpr std::array<std::string_view, 2> kQuantizeBases{"Luma", "Per channel"};

inline constexpr std::array<std::string_view, 2> kDitherPatterns{"Ordered (Bayer)", "Blue noise"};
inline constexpr std::array<std::string_view, 4> kDitherMatrixSizes{"2 x 2", "4 x 4", "8 x 8",
                                                                    "16 x 16"};
inline constexpr std::array<std::string_view, 10> kDitherBitDepthLabels{
    "8-bit", "10-bit", "12-bit", "16-bit", "1-bit", "2-bit", "3-bit", "4-bit", "5-bit", "6-bit"};
inline constexpr std::array<std::string_view, 2> kDitherDomains{"Display (sRGB)", "Stored value"};
inline constexpr std::array<std::string_view, 2> kDitherAnimations{"Every Frame", "Held"};

// A Boolean that is on by default.
inline constexpr DepthParameterSpec booleanOn(const std::string_view role,
                                              const std::string_view label) {
    auto spec = boolean(role, label);
    spec.value[0] = 1.0;
    return spec;
}

inline constexpr double kV = kMaximumToneMapValue;
inline constexpr double kL = 1.0e4;
inline constexpr double kM = kMaximumColorMatrixElement;

inline constexpr std::array kToneMapParameters{
    choice("method", kToneMapMethods, kToneMapEachChannel, "Roll off by"),
    scalar("knee", 0.8, 0.0, kV, "Knee", 0.0, 4.0, 0.01, 3),
    scalar("ceiling", 1.0, 0.0, kV, "Ceiling", 0.0, 4.0, 0.01, 3),
    scalar("shadows", 0.0, -1.0, 16.0, "Shadow recovery", -1.0, 4.0, 0.01, 2),
    scalar("shadowRange", 0.18, 1.0e-4, kV, "Shadow range", 0.001, 1.0, 0.001, 3),
    boolean("gamutCompress", "Compress gamut"),
    scalar("gamutThreshold", 0.8, 0.0, 0.999, "Gamut threshold", 0.0, 0.999, 0.01, 3),
    scalar("gamutLimit", 1.2, 1.001, 100.0, "Gamut limit", 1.001, 2.0, 0.01, 3),
};

inline constexpr std::array kColorMatrixParameters{
    choice("preset", kMatrixPresets, kColorMatrixCustom, "Preset"),
    choice("matrixSize", kMatrixSizes, kColorMatrixSize3, "Matrix size"),
    choice("inputModel", kColorModels, kColorModelRgb, "Read input as"),
    choice("outputModel", kColorModels, kColorModelRgb, "Write result as"),
    scalar("rr", 1.0, -kM, kM, "Red from red", -2.0, 2.0, 0.01, 3),
    scalar("rg", 0.0, -kM, kM, "Red from green", -2.0, 2.0, 0.01, 3),
    scalar("rb", 0.0, -kM, kM, "Red from blue", -2.0, 2.0, 0.01, 3),
    scalar("ra", 0.0, -kM, kM, "Red from alpha", -2.0, 2.0, 0.01, 3),
    scalar("gr", 0.0, -kM, kM, "Green from red", -2.0, 2.0, 0.01, 3),
    scalar("gg", 1.0, -kM, kM, "Green from green", -2.0, 2.0, 0.01, 3),
    scalar("gb", 0.0, -kM, kM, "Green from blue", -2.0, 2.0, 0.01, 3),
    scalar("ga", 0.0, -kM, kM, "Green from alpha", -2.0, 2.0, 0.01, 3),
    scalar("br", 0.0, -kM, kM, "Blue from red", -2.0, 2.0, 0.01, 3),
    scalar("bg", 0.0, -kM, kM, "Blue from green", -2.0, 2.0, 0.01, 3),
    scalar("bb", 1.0, -kM, kM, "Blue from blue", -2.0, 2.0, 0.01, 3),
    scalar("ba", 0.0, -kM, kM, "Blue from alpha", -2.0, 2.0, 0.01, 3),
    scalar("ar", 0.0, -kM, kM, "Alpha from red", -2.0, 2.0, 0.01, 3),
    scalar("ag", 0.0, -kM, kM, "Alpha from green", -2.0, 2.0, 0.01, 3),
    scalar("ab", 0.0, -kM, kM, "Alpha from blue", -2.0, 2.0, 0.01, 3),
    scalar("aa", 1.0, -kM, kM, "Alpha from alpha", -2.0, 2.0, 0.01, 3),
};

inline constexpr double kQ = kMaximumQuantizeThreshold;
inline constexpr double kLevelsMin = kMinimumQuantizeLevels;
inline constexpr double kLevelsMax = kMaximumQuantizeLevels;

inline constexpr std::array kQuantizeParameters{
    choice("mode", kQuantizeModes, kQuantizeThreshold, "Mode"),
    choice("basis", kQuantizeBases, kQuantizeBasisLuma, "Threshold on"),
    scalar("level", 0.5, 0.0, kQ, "Level", 0.0, 1.0, 0.01, 3),
    scalar("softness", 0.0, 0.0, kQ, "Softness", 0.0, 1.0, 0.01, 3),
    boolean("invert", "Invert"),
    scalar("levels", 8.0, kLevelsMin, kLevelsMax, "Levels", 2.0, 64.0, 1.0, 0),
    booleanOn("linkLevels", "Link levels"),
    scalar("redLevels", 8.0, kLevelsMin, kLevelsMax, "Red levels", 2.0, 64.0, 1.0, 0),
    scalar("greenLevels", 8.0, kLevelsMin, kLevelsMax, "Green levels", 2.0, 64.0, 1.0, 0),
    scalar("blueLevels", 8.0, kLevelsMin, kLevelsMax, "Blue levels", 2.0, 64.0, 1.0, 0),
    boolean("posterizeAlpha", "Posterize alpha"),
    scalar("alphaLevels", 8.0, kLevelsMin, kLevelsMax, "Alpha levels", 2.0, 64.0, 1.0, 0),
};

inline constexpr std::array kDitherParameters{
    choice("pattern", kDitherPatterns, kDitherBlueNoise, "Pattern"),
    choice("matrixSize", kDitherMatrixSizes, 2, "Matrix size"),
    choice("bitDepth", kDitherBitDepthLabels, kDitherDefaultBitDepth, "Target bit depth"),
    scalar("amplitude", 1.0, 0.0, kMaximumDitherAmplitude, "Amplitude", 0.0, 4.0, 0.05, 2, "LSB"),
    choice("domain", kDitherDomains, kDitherDisplay, "Measured in"),
    choice("animation", kDitherAnimations, kDitherEveryFrame, "Pattern changes"),
    integer("seed", 1, 0, 2147483647, "Seed"),
    boolean("monochrome", "Monochrome"),
};

inline constexpr std::array kHslQualifierParameters{
    scalar("hueCenter", 0.0, 0.0, 360.0, "Hue", 0.0, 360.0, 1.0, 1, "\xC2\xB0"),
    scalar("hueWidth", 360.0, 0.0, 360.0, "Hue width", 0.0, 360.0, 1.0, 1, "\xC2\xB0"),
    scalar("hueSoftness", 20.0, 0.0, 180.0, "Hue softness", 0.0, 180.0, 1.0, 1, "\xC2\xB0"),
    scalar("satLow", 0.0, 0.0, 1.0, "Saturation from", 0.0, 1.0, 0.01, 3),
    scalar("satHigh", 1.0, 0.0, 1.0, "Saturation to", 0.0, 1.0, 0.01, 3),
    scalar("satSoftness", 0.1, 0.0, 1.0, "Saturation softness", 0.0, 1.0, 0.01, 3),
    scalar("lumaLow", 0.0, 0.0, kL, "Luma from", 0.0, 4.0, 0.01, 3),
    scalar("lumaHigh", 1.0, 0.0, kL, "Luma to", 0.0, 4.0, 0.01, 3),
    scalar("lumaSoftness", 0.1, 0.0, kL, "Luma softness", 0.0, 1.0, 0.01, 3),
    boolean("invert", "Invert selection"),
};

} // namespace color_cleanup_parameters

[[nodiscard]] constexpr bool isColorCleanupNodeType(const std::string_view typeId) noexcept {
    return typeId == kToneMapNodeType || typeId == kColorMatrixNodeType ||
           typeId == kHslQualifierNodeType || typeId == kQuantizeNodeType ||
           typeId == kDitherNodeType;
}

// The parameters of a color cleanup node in definition order, or an empty span.
[[nodiscard]] constexpr std::span<const DepthParameterSpec>
colorCleanupParameters(const std::string_view typeId) noexcept {
    if (typeId == kToneMapNodeType)
        return color_cleanup_parameters::kToneMapParameters;
    if (typeId == kColorMatrixNodeType)
        return color_cleanup_parameters::kColorMatrixParameters;
    if (typeId == kHslQualifierNodeType)
        return color_cleanup_parameters::kHslQualifierParameters;
    if (typeId == kQuantizeNodeType)
        return color_cleanup_parameters::kQuantizeParameters;
    if (typeId == kDitherNodeType)
        return color_cleanup_parameters::kDitherParameters;
    return {};
}

// The spec of a "<type id>.<role>" schema key, or null.
[[nodiscard]] constexpr const DepthParameterSpec*
colorCleanupParameterSpec(const std::string_view schemaKey) noexcept {
    for (const auto type : {kToneMapNodeType, kColorMatrixNodeType, kHslQualifierNodeType,
                            kQuantizeNodeType, kDitherNodeType}) {
        if (schemaKey.size() <= type.size() + 1 || !schemaKey.starts_with(type) ||
            schemaKey[type.size()] != '.')
            continue;
        const auto role = schemaKey.substr(type.size() + 1);
        for (const auto& spec : colorCleanupParameters(type))
            if (spec.role == role)
                return &spec;
    }
    return nullptr;
}

[[nodiscard]] std::string colorCleanupSchemaKey(std::string_view typeId, std::string_view role);
// Whether `value` lies in the document domain of the color cleanup parameter `schemaKey`.
[[nodiscard]] bool isColorCleanupParameterValueValid(std::string_view schemaKey,
                                                     const ParameterValue& value) noexcept;
// The registered definition of a color cleanup node (before the effect mask is appended).
[[nodiscard]] NodeDefinition colorCleanupDefinition(std::string_view typeId);

[[nodiscard]] constexpr bool isColorCleanupScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = colorCleanupParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Scalar;
}
// A constant (Choice, Integer, Boolean): no socket and no animation.
[[nodiscard]] constexpr bool isColorCleanupConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = colorCleanupParameterSpec(key);
    return spec != nullptr && spec->kind != DepthParameterKind::Scalar &&
           spec->kind != DepthParameterKind::Color;
}
[[nodiscard]] constexpr bool
isColorCleanupScalarWithinDomain(const std::string_view key, const double value) noexcept {
    const auto* spec = colorCleanupParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document

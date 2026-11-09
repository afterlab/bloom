#pragma once

// #518 Tone Map, #517 Color Matrix and #523 HSL Qualifier: the durable vocabulary of the
// per-pixel color nodes. Every node takes an Image on `input`; Tone Map and Color Matrix give an
// Image on `image` (and take the optional effect mask every grade takes), the HSL Qualifier gives a
// Mask on `mask`. Schema keys are "<type id>.<role>", like the depth effects'. Every Scalar is
// animatable, drivable and has a socket of its own; every Choice and Boolean is a constant with no
// socket. docs/architecture/image-filters.md, "Tone Map", "Color Matrix" and "HSL Qualifier", are
// the specification.
//
//   Color Matrix (`bloom.color-matrix`) mixes the straight channels with a 3 x 3 or 4 x 4 matrix
//     (`rr` is the red result's share of the red input, `rg` of green, and so on; the alpha row
//     and column count only at 4 x 4), after reading the input as a color model and before writing
//     the result as one (RGB, HSL, HSV, YCbCr or Lab). A `preset` other than Custom replaces the
//     matrix' elements with its own.
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

inline constexpr double kV = kMaximumToneMapValue;
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

} // namespace color_cleanup_parameters

[[nodiscard]] constexpr bool isColorCleanupNodeType(const std::string_view typeId) noexcept {
    return typeId == kToneMapNodeType || typeId == kColorMatrixNodeType;
}

// The parameters of a color cleanup node in definition order, or an empty span.
[[nodiscard]] constexpr std::span<const DepthParameterSpec>
colorCleanupParameters(const std::string_view typeId) noexcept {
    if (typeId == kToneMapNodeType)
        return color_cleanup_parameters::kToneMapParameters;
    if (typeId == kColorMatrixNodeType)
        return color_cleanup_parameters::kColorMatrixParameters;
    return {};
}

// The spec of a "<type id>.<role>" schema key, or null.
[[nodiscard]] constexpr const DepthParameterSpec*
colorCleanupParameterSpec(const std::string_view schemaKey) noexcept {
    for (const auto type : {kToneMapNodeType, kColorMatrixNodeType}) {
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

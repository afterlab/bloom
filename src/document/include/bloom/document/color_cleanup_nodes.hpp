#pragma once

// #518 Tone Map, #517 Color Matrix and #523 HSL Qualifier: the durable vocabulary of the
// per-pixel color nodes. Every node takes an Image on `input`; Tone Map and Color Matrix give an
// Image on `image` (and take the optional effect mask every grade takes), the HSL Qualifier gives a
// Mask on `mask`. Schema keys are "<type id>.<role>", like the depth effects'. Every Scalar is
// animatable, drivable and has a socket of its own; every Choice and Boolean is a constant with no
// socket. docs/architecture/image-filters.md, "Tone Map", "Color Matrix" and "HSL Qualifier", are
// the specification.
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
inline constexpr std::uint32_t kColorCleanupNodeSchemaVersion = 1;

inline constexpr std::string_view kColorCleanupInputPort = "input";
inline constexpr std::string_view kColorCleanupImageOutputPort = "image";

// Stored values of Tone Map's method.
inline constexpr std::int64_t kToneMapEachChannel = 0;
inline constexpr std::int64_t kToneMapBrightestChannel = 1;

// A value, knee, ceiling or range is finite and within this.
inline constexpr double kMaximumToneMapValue = 1.0e6;

namespace color_cleanup_parameters {

using namespace depth_parameters;

inline constexpr std::array<std::string_view, 2> kToneMapMethods{"Each channel",
                                                                  "Brightest channel"};

inline constexpr double kV = kMaximumToneMapValue;

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

} // namespace color_cleanup_parameters

[[nodiscard]] constexpr bool isColorCleanupNodeType(const std::string_view typeId) noexcept {
    return typeId == kToneMapNodeType;
}

// The parameters of a color cleanup node in definition order, or an empty span.
[[nodiscard]] constexpr std::span<const DepthParameterSpec>
colorCleanupParameters(const std::string_view typeId) noexcept {
    if (typeId == kToneMapNodeType)
        return color_cleanup_parameters::kToneMapParameters;
    return {};
}

// The spec of a "<type id>.<role>" schema key, or null.
[[nodiscard]] constexpr const DepthParameterSpec*
colorCleanupParameterSpec(const std::string_view schemaKey) noexcept {
    for (const auto type : {kToneMapNodeType}) {
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

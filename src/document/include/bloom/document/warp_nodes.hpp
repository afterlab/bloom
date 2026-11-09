#pragma once

// #348 Corner Pin, ST Map and Displace: the durable vocabulary of the three nodes that move a picture
// by reading it somewhere else. Each output pixel is an inverse mapping: it picks the position of the
// input to read, and reads it. The three differ only in where that position comes from.
//
//   Corner Pin  four corners, each an offset in pixels from where the input's own corner is
//               (y down), pin the input's data window to a quad with perspective.
//   ST Map      the position is given as data: a map image holds, in two of its channels, the
//               position to read as a fraction of the composition frame (0, 0 the bottom left, 1, 1
//               the top right). Absolute coordinates.
//   Displace    the position is the pixel's own, moved by a map image: (value - midpoint) times an
//               amount, in pixels, in each direction. Relative displacement.
//
// A map is read as the data it is: the values the image holds, unclamped and without a colour
// conversion, and it never takes the transform of an input colour space. The filter and the edge
// rule are the field raster's (docs/architecture/fields.md): Nearest takes one texel exactly, which
// an ID or data image needs.
//
// docs/architecture/image-filters.md, "Corner Pin, ST Map and Displace", is the specification.
//
// Schema keys are "bloom.corner-pin.<role>", "bloom.st-map.<role>" and "bloom.displace.<role>". The
// corners, the amounts and the midpoint are animatable, drivable and have a socket of their own;
// every choice is a constant with no socket.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kCornerPinNodeType = "bloom.corner-pin";
inline constexpr std::string_view kStMapNodeType = "bloom.st-map";
inline constexpr std::string_view kDisplaceNodeType = "bloom.displace";
inline constexpr std::uint32_t kWarpNodeSchemaVersion = 1;

inline constexpr std::string_view kWarpImagePort = "image";
inline constexpr std::string_view kWarpMapPort = "map";

// The filter, in the field raster's order, and the edge rule in the order the node offers (the
// lowering maps it to the raster's).
inline constexpr std::int64_t kWarpFilterBilinear = 0;
inline constexpr std::int64_t kWarpFilterNearest = 1;
inline constexpr std::int64_t kWarpFilterBicubic = 2;
inline constexpr std::int64_t kWarpEdgeTransparent = 0;
inline constexpr std::int64_t kWarpEdgeExtend = 1;
inline constexpr std::int64_t kWarpEdgeRepeat = 2;
inline constexpr std::int64_t kWarpEdgeMirror = 3;
// A channel of a map: red, green, blue, alpha (ST Map); Displace's choice has None in front.
inline constexpr std::int64_t kWarpChannelRed = 0;
inline constexpr std::int64_t kWarpChannelAlpha = 3;
inline constexpr std::int64_t kDisplaceChannelNone = 0;

// How far a corner can be moved, and how far an amount can reach, in pixels.
inline constexpr double kMaximumCornerPinOffset = 1.0e6;
inline constexpr double kMaximumDisplaceAmount = 1.0e5;
inline constexpr double kMaximumDisplaceMidpoint = 1.0e3;

enum class WarpParameterKind : std::uint8_t { Choice, Scalar };

struct WarpParameterSpec final {
    std::string_view role;
    WarpParameterKind kind = WarpParameterKind::Scalar;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    // The control's label and presentation: the span its slider covers, one displayed step, the
    // decimals shown and a unit.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 0.01;
    int decimals = 2;
    std::string_view unit{};
};

namespace warp_parameters {

using Kind = WarpParameterKind;

inline constexpr std::array<std::string_view, 3> kFilters{"Bilinear", "Nearest", "Bicubic"};
inline constexpr std::array<std::string_view, 4> kEdges{"Transparent", "Extend", "Repeat", "Mirror"};
inline constexpr std::array<std::string_view, 4> kChannels{"Red", "Green", "Blue", "Alpha"};
inline constexpr std::array<std::string_view, 5> kOptionalChannels{"None", "Red", "Green", "Blue",
                                                                   "Alpha"};

inline constexpr std::array kCornerPin{
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
};
inline constexpr std::array kStMap{
    WarpParameterSpec{"uChannel", Kind::Choice, 0.0, 0.0, 3.0, kChannels, "U Channel"},
    WarpParameterSpec{"vChannel", Kind::Choice, 1.0, 0.0, 3.0, kChannels, "V Channel"},
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
    WarpParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 3.0, kEdges, "Edges"},
};
inline constexpr std::array kDisplace{
    WarpParameterSpec{"xChannel", Kind::Choice, 1.0, 0.0, 4.0, kOptionalChannels, "X Channel"},
    WarpParameterSpec{"yChannel", Kind::Choice, 2.0, 0.0, 4.0, kOptionalChannels, "Y Channel"},
    WarpParameterSpec{"xAmount", Kind::Scalar, 20.0, -kMaximumDisplaceAmount,
                      kMaximumDisplaceAmount, {}, "X Amount", -200.0, 200.0, 1.0, 1, "px"},
    WarpParameterSpec{"yAmount", Kind::Scalar, 20.0, -kMaximumDisplaceAmount,
                      kMaximumDisplaceAmount, {}, "Y Amount", -200.0, 200.0, 1.0, 1, "px"},
    WarpParameterSpec{"midpoint", Kind::Scalar, 0.5, -kMaximumDisplaceMidpoint,
                      kMaximumDisplaceMidpoint, {}, "Midpoint", -1.0, 1.0, 0.01, 3},
    WarpParameterSpec{"filter", Kind::Choice, 0.0, 0.0, 2.0, kFilters, "Filter"},
    WarpParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 3.0, kEdges, "Edges"},
};

// The four corners, top left first, as the roles of Corner Pin's offsets.
inline constexpr std::array<std::string_view, 4> kCornerRoles{"topLeft", "topRight", "bottomRight",
                                                              "bottomLeft"};
inline constexpr std::array<std::string_view, 4> kCornerLabels{"Top Left", "Top Right",
                                                               "Bottom Right", "Bottom Left"};

} // namespace warp_parameters

[[nodiscard]] constexpr std::span<const WarpParameterSpec> cornerPinParameters() noexcept {
    return warp_parameters::kCornerPin;
}
[[nodiscard]] constexpr std::span<const WarpParameterSpec> stMapParameters() noexcept {
    return warp_parameters::kStMap;
}
[[nodiscard]] constexpr std::span<const WarpParameterSpec> displaceParameters() noexcept {
    return warp_parameters::kDisplace;
}
[[nodiscard]] constexpr std::span<const std::string_view> cornerPinCornerRoles() noexcept {
    return warp_parameters::kCornerRoles;
}
[[nodiscard]] constexpr std::span<const std::string_view> cornerPinCornerLabels() noexcept {
    return warp_parameters::kCornerLabels;
}

// The spec of a "bloom.corner-pin.<role>", "bloom.st-map.<role>" or "bloom.displace.<role>" schema
// key, or null (a corner has none: it is a position).
[[nodiscard]] constexpr const WarpParameterSpec* warpParameterSpec(const std::string_view key) noexcept {
    const auto find = [key](const std::string_view prefix, const auto& table)
        -> const WarpParameterSpec* {
        if (!key.starts_with(prefix))
            return nullptr;
        const auto role = key.substr(prefix.size());
        for (const auto& spec : table)
            if (spec.role == role)
                return &spec;
        return nullptr;
    };
    if (const auto* spec = find("bloom.corner-pin.", warp_parameters::kCornerPin))
        return spec;
    if (const auto* spec = find("bloom.st-map.", warp_parameters::kStMap))
        return spec;
    return find("bloom.displace.", warp_parameters::kDisplace);
}

// Whether `key` is one of Corner Pin's four corner offsets.
[[nodiscard]] constexpr bool isCornerPinCornerSchemaKey(const std::string_view key) noexcept {
    constexpr std::string_view prefix = "bloom.corner-pin.";
    if (!key.starts_with(prefix))
        return false;
    const auto role = key.substr(prefix.size());
    for (const auto corner : warp_parameters::kCornerRoles)
        if (corner == role)
            return true;
    return false;
}

[[nodiscard]] constexpr bool isWarpScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = warpParameterSpec(key);
    return spec != nullptr && spec->kind == WarpParameterKind::Scalar;
}
// A constant (every choice): no socket and no animation.
[[nodiscard]] constexpr bool isWarpConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = warpParameterSpec(key);
    return spec != nullptr && spec->kind == WarpParameterKind::Choice;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isWarpScalarWithinDomain(const std::string_view key,
                                                      const double value) noexcept {
    const auto* spec = warpParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document

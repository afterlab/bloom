#pragma once

// #598 Warp Path (docs/architecture/path-processing.md). The node's type, the vocabulary of its
// warp-function selector and the schema keys of the operands that carry a unit or a range of their
// own. The stored integer of the selector is durable. The skew operands are shared with Transform
// Path, which gained them in the same change.

#include <bloom/document/value_utility_nodes.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kWarpPathNodeType = "bloom.warp-path";

inline constexpr std::string_view kWarpFunctionParameterSchemaKey = "bloom.path.warp-function";
inline constexpr std::string_view kWarpAmountSchemaKey = "bloom.warp.amount";
inline constexpr std::string_view kWarpSizeSchemaKey = "bloom.warp.size";
inline constexpr std::string_view kWarpPhaseSchemaKey = "bloom.warp.phase";
inline constexpr std::string_view kWarpToleranceSchemaKey = "bloom.warp.tolerance";
// Skew of the path matrix (Transform Path and Warp Path): the slant in degrees, and the direction
// the slant runs along in degrees, clockwise from screen right.
inline constexpr std::string_view kPathSkewSchemaKey = "bloom.path.skew";
inline constexpr std::string_view kPathSkewAxisSchemaKey = "bloom.path.skew-axis";

// The skew is a slant angle: a quarter turn is no slant at all, so the stored value stays inside.
inline constexpr double kMaximumPathSkewDegrees = 89.9;

// The built-in warps, each a function of one point in the warp's own space (the space the matrix
// operands describe): Wave moves x by a sine of y; Twist turns the plane about the origin by an
// angle that fades with distance; Bulge pushes points away from the origin (a negative amount pulls
// them in, a pinch); Bend curves the x axis into an arc; Ripple moves points along the line from
// the origin by a sine of their distance.
enum class WarpFunction : std::uint8_t { Wave = 0, Twist = 1, Bulge = 2, Bend = 3, Ripple = 4 };
inline constexpr std::array<WarpFunction, 5> kWarpFunctions{WarpFunction::Wave, WarpFunction::Twist,
                                                            WarpFunction::Bulge, WarpFunction::Bend,
                                                            WarpFunction::Ripple};

// The deepest an adaptive subdivision goes on one segment: a segment splits at most this many
// times along any one chain.
inline constexpr int kMaximumWarpDepth = 14;

} // namespace bloom::document

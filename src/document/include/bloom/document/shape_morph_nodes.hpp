#pragma once

// #597 Shape Morph (docs/architecture/path-processing.md, "Shape Morph"). The node's type, the
// vocabulary of its pairing selector and the schema key of its amount. The stored integer of the
// selector is durable.

#include <bloom/document/value_utility_nodes.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kShapeMorphNodeType = "bloom.shape-morph";

inline constexpr std::string_view kMorphPairingParameterSchemaKey = "bloom.path.morph-pairing";
// How far from the first shape to the second, in percent: 0 is exactly the first, 100 exactly the
// second. A value outside 0 to 100 overshoots, as an eased key can.
inline constexpr std::string_view kMorphAmountSchemaKey = "bloom.morph.amount";

// How the contours of the two shapes correspond: contours of groups with the same key first and the
// rest by how near they lie and how big they are, or the first with the first, the second with the
// second.
enum class MorphPairing : std::uint8_t { ByGroupThenGeometry = 0, ByOrder = 1 };
inline constexpr std::array<MorphPairing, 2> kMorphPairings{MorphPairing::ByGroupThenGeometry,
                                                            MorphPairing::ByOrder};

} // namespace bloom::document

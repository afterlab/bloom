#pragma once

// #503 Alpha Association: the durable vocabulary of the one node that converts between the ways an
// image's colour can relate to its alpha. An Image in and out, one constant mode:
//   Unmult (0)               alpha from the brightest channel of the image over black (fire, smoke
//                            and flares shot on black); with `white` on, from the image over white.
//   Premultiply (1)          RGB times alpha: the colour was straight.
//   Unpremultiply (2)        RGB divided by alpha (above kAlphaAssociationEpsilon): straight out.
//   Remove Color Matting (3) the colour was premultiplied against `background`, not black.
// Every mode works on the scene-linear values as stored (HDR, signed), never clamps colour and
// keeps alpha in [0, 1]. docs/architecture/image-filters.md, "Alpha Association", is the
// specification.

#include <cstdint>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kAlphaAssociationNodeType = "bloom.alpha-association";

inline constexpr std::string_view kAlphaAssociationModeSchemaKey = "bloom.alpha-association.mode";
inline constexpr std::string_view kAlphaAssociationWhiteSchemaKey = "bloom.alpha-association.white";
inline constexpr std::string_view kAlphaAssociationBackgroundSchemaKey =
    "bloom.alpha-association.background";

inline constexpr std::int64_t kAlphaAssociationUnmult = 0;
inline constexpr std::int64_t kAlphaAssociationPremultiply = 1;
inline constexpr std::int64_t kAlphaAssociationUnpremultiply = 2;
inline constexpr std::int64_t kAlphaAssociationRemoveMatting = 3;

// Unpremultiply divides only where alpha exceeds 2^-20 (about 9.5e-7, exact in binary32); at or
// below it RGB passes unchanged, so a near-zero alpha never amplifies noise towards infinity.
inline constexpr float kAlphaAssociationEpsilon = 0x1p-20F;

// The background colour's RGB is finite and within +-1e6, like the grade values.
inline constexpr double kMaximumAlphaAssociationBackground = 1.0e6;

} // namespace bloom::document

#pragma once

// #597 Shape Morph's computation (docs/architecture/path-value.md, "Morphing between different
// shapes"): the pure step that gives two path values the same structure, so that any two values
// morph, whatever their contour counts, anchor counts, closed flags and groups.
//
// `alignPaths(from, to)` answers two values with equal structure: the same contour count and,
// contour by contour, the same anchor count and closed flag, each value drawing exactly its own
// shape. Contour `i` of `from` stays contour `i` (so `from`'s groups still name the same contours);
// contours of `to` that pair with none are appended after them. A morph then moves each anchor and
// handle in a straight line from one to the other. Nothing is stored: the answer is a deterministic
// function of the two values, recomputed wherever it is needed (a path key's sample, the Shape
// Morph node).
//
// How parts correspond:
// - Contours pair by group first: contours of two groups with the same key (#589) pair among
//   themselves. The other contours, those of groups the other value does not hold and those in no
//   group, pair among themselves. Within each set, pairs are taken greedily, cheapest first, by the
//   distance between centroids and between sizes (`ByGroupThenGeometry`), or by order (`ByOrder`).
// - A contour left without a partner shrinks to, or grows from, a point at its own centroid.
// - Two paired contours get the same anchor count by subdividing the one with fewer at its longest
//   segment, again and again (de Casteljau at the middle: the drawn shape is unchanged).
// - Two closed contours: the cyclic start and the direction of the second are the ones that make
// the
//   summed squared distance between corresponding anchors least. Two open contours keep their ends
//   at the ends, the second reversed when its ends lie nearer the first's opposite ends. A closed
//   contour paired with an open one is cut open at the anchor (and in the direction) that makes
//   that sum least, its start repeated at its end, so both are open and the open one's ends travel
//   together.
//
// Pure functions: no clock, no state, no Qt, no cancellation (the work is bounded by the caps
// below).

#include <bloom/document/parameter.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace bloom::runtime::pathmorph {

enum class Pairing : std::uint8_t {
    // Groups with the same key first, the rest by geometry.
    ByGroupThenGeometry,
    // Contour i with contour i; the rest grow or shrink.
    ByOrder,
};

struct AlignOptions final {
    Pairing pairing = Pairing::ByGroupThenGeometry;
};

enum class AlignStatus : std::uint8_t {
    Ok,
    // The aligned form would hold more anchors than a contour or a value may, or more contours than
    // a value may: `requested` says how many, `contour` whether the bound is one contour's.
    TooMany,
};

struct Alignment final {
    AlignStatus status = AlignStatus::Ok;
    document::PathValue from;
    document::PathValue to;
    std::size_t requested = 0;
    bool contour = false;
};

// Past this many candidate pairs in one set the contours of that set pair by order instead of by
// geometry, so the pairing never weighs more than about a million candidates.
inline constexpr std::size_t kMaximumGeometricPairs = std::size_t{1} << 20U;

[[nodiscard]] Alignment alignPaths(const document::PathValue& from, const document::PathValue& to,
                                   const AlignOptions& options = {});

// The value `amount` of the way from one aligned value to the other: every anchor point and handle
// in a straight line (an anchor with no handle where the other has one moves from its point); the
// closed flags, anchor modes, fill rule and groups are `from`'s. `amount` may lie outside 0..1 (an
// eased key that overshoots).
[[nodiscard]] document::PathValue mixAligned(const document::PathValue& from,
                                             const document::PathValue& to, double amount);

// The morph itself: `from` exactly at amount 0, `to` exactly at amount 1 (not the aligned form),
// and otherwise the mix of the alignment, carrying `from`'s fill rule, groups and group attributes.
// Nothing when the alignment is TooMany.
[[nodiscard]] std::optional<document::PathValue> morphPaths(const document::PathValue& from,
                                                            const document::PathValue& to,
                                                            double amount,
                                                            const AlignOptions& options = {});

} // namespace bloom::runtime::pathmorph

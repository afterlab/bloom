#ifndef BLOOM_RUNTIME_TEXT_ANIMATOR_HPP
#define BLOOM_RUNTIME_TEXT_ANIMATOR_HPP

// #758 Text Animator: the per-cluster arithmetic (docs/architecture/evaluation-primitives.md, "Text
// Animator"). Everything here is host parameter resolution: pure, deterministic and the same on
// every route, so the CPU reference and the GPU route draw from one set of per-cluster values.
//
// A cluster is one Unicode scalar of the laid-out text (carriage returns removed) until shaping
// (#345) makes it a shaped cluster. Units count clusters four ways: characters (every cluster but
// a line break), characters excluding spaces, words (runs of non-space clusters) and lines (the
// layout's lines). A cluster a unit does not count (a space, for the middle two) takes the unit of
// the cluster before it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::runtime {

inline constexpr std::size_t kTextUnitKinds = 4;

// The unit each cluster belongs to, per unit kind (document::kTextUnit*), and how many units of
// each kind the text holds. `lineOf` is each cluster's layout line.
struct TextClusterUnits final {
    std::array<std::vector<std::size_t>, kTextUnitKinds> unitOf{};
    std::array<std::size_t, kTextUnitKinds> count{};
};
[[nodiscard]] TextClusterUnits textClusterUnits(std::u32string_view clusters,
                                                std::span<const std::size_t> lineOf,
                                                std::size_t lineCount);
// Whether the cluster is a space (it draws no ink and ends a word).
[[nodiscard]] bool isTextSpace(char32_t scalar) noexcept;

// One range selector's values, resolved at the frame (document::text_animator_parameters).
struct TextSelectorValues final {
    std::int64_t mode = 0;
    std::int64_t units = 0;
    std::int64_t rangeBy = 0;
    std::int64_t shape = 0;
    bool randomize = false;
    double start = 0.0;
    double end = 100.0;
    double offset = 0.0;
    double amount = 100.0;
    double easeHigh = 0.0;
    double easeLow = 0.0;
    double seed = 0.0;
};

// The selector's shape at unit `position` (the unit's place in the selection order) of `count`,
// before easing. The range [s, e) runs from start + offset to end + offset, in units (a percent
// of `count`, or indices), ordered. Square is the part of [position, position + 1) inside the
// range; the other shapes are evaluated at the unit's center, t = (position + 0.5 - s) / (e - s):
// Ramp up is 0 before the range, t inside and 1 after; Ramp down its mirror; Triangle 1 - |2t - 1|,
// Round sqrt(1 - (2t - 1)^2) and Smooth (1 - cos 2 pi t) / 2 inside the range and 0 outside.
[[nodiscard]] double textSelectorShape(const TextSelectorValues& selector, double position,
                                       std::size_t count) noexcept;
// The ease: a cubic Hermite through (0, 0) and (1, 1) whose slope is 1 - easeLow / 100 at 0 and
// 1 - easeHigh / 100 at 1 (0 % is linear, 100 % flattens that end, -100 % steepens it).
[[nodiscard]] double textSelectorEase(double value, double easeHigh, double easeLow) noexcept;
// The selection order of `count` units: the identity, or a permutation from `seed` (a Fisher-Yates
// shuffle driven by SplitMix64, the same on every platform). order[unit] is the unit's position.
[[nodiscard]] std::vector<std::size_t> textSelectorOrder(std::size_t count, bool randomize,
                                                         double seed);
// Every cluster's amount from this selector: the eased shape times Amount / 100.
[[nodiscard]] std::vector<double> textSelectorAmounts(const TextSelectorValues& selector,
                                                      const TextClusterUnits& units);
// Folds one selector into the running amounts: Add adds, Subtract subtracts, Intersect multiplies;
// the result is clamped to [-1, 1]. The first selector folds into 0.
void combineTextSelector(std::span<double> total, std::span<const double> selector,
                         std::int64_t mode) noexcept;

// The reveal: whether each cluster is shown when `reveal` units are (a percent of the unit count,
// or a count, by `revealBy`): unit k shows when k + 1 <= revealed.
[[nodiscard]] std::vector<bool> textRevealVisible(const TextClusterUnits& units,
                                                  std::int64_t revealUnits,
                                                  std::int64_t revealBy, double reveal);

// Whether a cursor blinking `rate` times a second shows at `seconds`: on for the first half of each
// blink; a rate of 0 is always on.
[[nodiscard]] bool textCursorShown(double rate, double seconds) noexcept;

} // namespace bloom::runtime

#endif // BLOOM_RUNTIME_TEXT_ANIMATOR_HPP

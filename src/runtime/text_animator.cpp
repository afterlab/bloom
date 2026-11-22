// #758: the Text Animator's per-cluster arithmetic (text_animator.hpp).
#include <bloom/runtime/text_animator.hpp>

#include <bloom/document/text_animator_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] bool isLineBreak(const char32_t scalar) noexcept {
    return scalar == U'\n' || scalar == U'\u2028' || scalar == U'\u2029' || scalar == U'\u000B' ||
           scalar == U'\u000C' || scalar == U'\u0085';
}

[[nodiscard]] std::uint64_t splitMix(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    auto z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

} // namespace

bool isTextSpace(const char32_t scalar) noexcept {
    return scalar == U' ' || scalar == U'\t' || scalar == U'\u00A0' || scalar == U'\u3000' ||
           scalar == U'\u2002' || scalar == U'\u2003' || scalar == U'\u2009' || isLineBreak(scalar);
}

TextClusterUnits textClusterUnits(const std::u32string_view clusters,
                                  const std::span<const std::size_t> lineOf,
                                  const std::size_t lineCount) {
    TextClusterUnits units;
    for (auto& unitOf : units.unitOf)
        unitOf.resize(clusters.size(), 0);
    std::size_t characters = 0;
    std::size_t visible = 0;
    std::size_t words = 0;
    bool inWord = false;
    for (std::size_t index = 0; index < clusters.size(); ++index) {
        const auto scalar = clusters[index];
        const bool space = isTextSpace(scalar);
        if (isLineBreak(scalar))
            units.unitOf[document::kTextUnitCharacters][index] = characters == 0 ? 0 : characters - 1;
        else
            units.unitOf[document::kTextUnitCharacters][index] = characters++;
        if (space) {
            units.unitOf[document::kTextUnitCharactersExcludingSpaces][index] =
                visible == 0 ? 0 : visible - 1;
            units.unitOf[document::kTextUnitWords][index] = words == 0 ? 0 : words - 1;
            inWord = false;
        } else {
            units.unitOf[document::kTextUnitCharactersExcludingSpaces][index] = visible++;
            if (!inWord)
                ++words;
            inWord = true;
            units.unitOf[document::kTextUnitWords][index] = words - 1;
        }
        units.unitOf[document::kTextUnitLines][index] =
            index < lineOf.size() ? lineOf[index] : (lineCount == 0 ? 0 : lineCount - 1);
    }
    units.count[document::kTextUnitCharacters] = characters;
    units.count[document::kTextUnitCharactersExcludingSpaces] = visible;
    units.count[document::kTextUnitWords] = words;
    units.count[document::kTextUnitLines] = lineCount;
    return units;
}

double textSelectorShape(const TextSelectorValues& selector, const double position,
                         const std::size_t count) noexcept {
    const auto scale =
        selector.rangeBy == document::kTextRangeIndex ? 1.0 : static_cast<double>(count) / 100.0;
    auto s = (selector.start + selector.offset) * scale;
    auto e = (selector.end + selector.offset) * scale;
    if (s > e)
        std::swap(s, e);
    if (selector.shape == document::kTextShapeSquare)
        return std::clamp(std::min(position + 1.0, e) - std::max(position, s), 0.0, 1.0);
    const auto center = position + 0.5;
    if (!(e > s)) {
        if (selector.shape == document::kTextShapeRampUp)
            return center >= s ? 1.0 : 0.0;
        if (selector.shape == document::kTextShapeRampDown)
            return center < s ? 1.0 : 0.0;
        return 0.0;
    }
    const auto t = (center - s) / (e - s);
    if (selector.shape == document::kTextShapeRampUp)
        return std::clamp(t, 0.0, 1.0);
    if (selector.shape == document::kTextShapeRampDown)
        return 1.0 - std::clamp(t, 0.0, 1.0);
    if (t < 0.0 || t > 1.0)
        return 0.0;
    const auto u = 2.0 * t - 1.0;
    if (selector.shape == document::kTextShapeTriangle)
        return 1.0 - std::abs(u);
    if (selector.shape == document::kTextShapeRound)
        return std::sqrt(std::max(0.0, 1.0 - u * u));
    if (selector.shape == document::kTextShapeSmooth)
        return (1.0 - std::cos(2.0 * std::numbers::pi * t)) / 2.0;
    return 0.0;
}

double textSelectorEase(const double value, const double easeHigh, const double easeLow) noexcept {
    const auto v = std::clamp(value, 0.0, 1.0);
    if (easeHigh == 0.0 && easeLow == 0.0)
        return v;
    const auto m0 = 1.0 - easeLow / 100.0;
    const auto m1 = 1.0 - easeHigh / 100.0;
    const auto v2 = v * v;
    const auto v3 = v2 * v;
    const auto eased = (-2.0 * v3 + 3.0 * v2) + (v3 - 2.0 * v2 + v) * m0 + (v3 - v2) * m1;
    return std::clamp(eased, 0.0, 1.0);
}

std::vector<std::size_t> textSelectorOrder(const std::size_t count, const bool randomize,
                                           const double seed) {
    std::vector<std::size_t> order(count);
    for (std::size_t index = 0; index < count; ++index)
        order[index] = index;
    if (!randomize || count < 2)
        return order;
    std::uint64_t state =
        static_cast<std::uint64_t>(std::clamp(std::floor(seed), 0.0, 1.0e15)) ^ 0x7E57A41A7E5ULL;
    for (std::size_t index = count - 1; index > 0; --index) {
        const auto pick = static_cast<std::size_t>(splitMix(state) % (index + 1));
        std::swap(order[index], order[pick]);
    }
    return order;
}

std::vector<double> textSelectorAmounts(const TextSelectorValues& selector,
                                        const TextClusterUnits& units) {
    const auto kind = static_cast<std::size_t>(std::clamp<std::int64_t>(selector.units, 0, 3));
    const auto& unitOf = units.unitOf[kind];
    const auto count = units.count[kind];
    const auto order = textSelectorOrder(count, selector.randomize, selector.seed);
    std::vector<double> perUnit(count, 0.0);
    for (std::size_t unit = 0; unit < count; ++unit)
        perUnit[unit] =
            textSelectorEase(textSelectorShape(selector, static_cast<double>(order[unit]), count),
                             selector.easeHigh, selector.easeLow) *
            (selector.amount / 100.0);
    std::vector<double> amounts(unitOf.size(), 0.0);
    for (std::size_t cluster = 0; cluster < unitOf.size(); ++cluster)
        amounts[cluster] = unitOf[cluster] < count ? perUnit[unitOf[cluster]] : 0.0;
    return amounts;
}

void combineTextSelector(const std::span<double> total, const std::span<const double> selector,
                         const std::int64_t mode) noexcept {
    for (std::size_t index = 0; index < total.size() && index < selector.size(); ++index) {
        const auto value = mode == document::kTextSelectorSubtract ? total[index] - selector[index]
                           : mode == document::kTextSelectorIntersect
                               ? total[index] * selector[index]
                               : total[index] + selector[index];
        total[index] = std::clamp(value, -1.0, 1.0);
    }
}

std::vector<bool> textRevealVisible(const TextClusterUnits& units, const std::int64_t revealUnits,
                                    const std::int64_t revealBy, const double reveal) {
    const auto kind = static_cast<std::size_t>(std::clamp<std::int64_t>(revealUnits, 0, 3));
    const auto& unitOf = units.unitOf[kind];
    const auto count = static_cast<double>(units.count[kind]);
    const auto revealed = revealBy == document::kTextRevealCount ? reveal : reveal / 100.0 * count;
    std::vector<bool> visible(unitOf.size(), false);
    for (std::size_t cluster = 0; cluster < unitOf.size(); ++cluster)
        visible[cluster] = static_cast<double>(unitOf[cluster]) + 1.0 <= revealed + 1e-9;
    return visible;
}

bool textCursorShown(const double rate, const double seconds) noexcept {
    if (!(rate > 0.0) || !std::isfinite(seconds))
        return true;
    const auto phase = seconds * rate - std::floor(seconds * rate);
    return phase < 0.5;
}

} // namespace bloom::runtime

#pragma once

// The layout of the scope count tables (#724, docs/architecture/scopes.md), shared by the CPU
// reference (src/runtime) and the GPU kernel (ScopeTablesV1) so both fill the same words.
//
// A measure is one array of unsigned 32-bit counts:
//   words[0] pixels counted, [1] not finite, [2] no chromaticity, [3] chromaticity outside,
//   [4] vectorscope outside, [5..7] zero;
//   Histogram:    4 channels (red, green, blue, luma) x 1026: bins 0..1023, under, over;
//   Waveform:     4 channels x `columns` x 514: levels 0..511, under, over;
//   Vectorscope:  512 rows (Cr) x 512 (Cb);
//   Chromaticity: 512 rows (y) x 512 (x).
// Only the tables asked for are present, in that order.

#include <cstdint>

namespace bloom::render {

inline constexpr std::uint32_t kScopeTableHistogram = 1U;
inline constexpr std::uint32_t kScopeTableWaveform = 2U;
inline constexpr std::uint32_t kScopeTableVectorscope = 4U;
inline constexpr std::uint32_t kScopeTableChromaticity = 8U;
inline constexpr std::uint32_t kScopeTableAll = 15U;

inline constexpr std::uint32_t kScopeCounterWords = 8U;
inline constexpr std::uint32_t kScopeCounterCounted = 0U;
inline constexpr std::uint32_t kScopeCounterNotFinite = 1U;
inline constexpr std::uint32_t kScopeCounterNoChromaticity = 2U;
inline constexpr std::uint32_t kScopeCounterChromaticityOutside = 3U;
inline constexpr std::uint32_t kScopeCounterVectorscopeOutside = 4U;

inline constexpr std::uint32_t kScopeChannels = 4U; // red, green, blue, luma
inline constexpr std::uint32_t kScopeCodes = 1024U;
inline constexpr std::uint32_t kScopeCodeUnder = 1024U;
inline constexpr std::uint32_t kScopeCodeOver = 1025U;
inline constexpr std::uint32_t kScopeHistogramChannelWords = 1026U;
inline constexpr std::uint32_t kScopeWaveformMaxColumns = 512U;
inline constexpr std::uint32_t kScopeWaveformLevels = 512U;
inline constexpr std::uint32_t kScopeWaveformLevelUnder = 512U;
inline constexpr std::uint32_t kScopeWaveformLevelOver = 513U;
inline constexpr std::uint32_t kScopeWaveformColumnWords = 514U;
inline constexpr std::uint32_t kScopeCells = 512U; // a side of the Vectorscope and CIE tables

struct ScopeTableLayout final {
    std::uint32_t scopes = 0;
    std::uint32_t columns = 0; // Waveform columns; 0 without the Waveform
    std::uint32_t histogramOffset = 0;
    std::uint32_t waveformOffset = 0;
    std::uint32_t vectorscopeOffset = 0;
    std::uint32_t chromaticityOffset = 0;
    std::uint32_t wordCount = kScopeCounterWords;

    [[nodiscard]] constexpr bool has(const std::uint32_t scope) const noexcept {
        return (scopes & scope) != 0U;
    }
    [[nodiscard]] constexpr std::uint32_t waveformChannelWords() const noexcept {
        return columns * kScopeWaveformColumnWords;
    }

    friend constexpr bool operator==(const ScopeTableLayout&, const ScopeTableLayout&) = default;
};

// The layout of a measure of `scopes` over an area `measuredWidth` pixels wide. An absent table's
// offset is 0.
[[nodiscard]] constexpr ScopeTableLayout
scopeTableLayout(const std::uint32_t scopes, const std::uint32_t measuredWidth) noexcept {
    ScopeTableLayout layout;
    layout.scopes = scopes & kScopeTableAll;
    std::uint32_t next = kScopeCounterWords;
    if (layout.has(kScopeTableHistogram)) {
        layout.histogramOffset = next;
        next += kScopeChannels * kScopeHistogramChannelWords;
    }
    if (layout.has(kScopeTableWaveform)) {
        layout.columns =
            measuredWidth < kScopeWaveformMaxColumns ? measuredWidth : kScopeWaveformMaxColumns;
        layout.waveformOffset = next;
        next += kScopeChannels * layout.columns * kScopeWaveformColumnWords;
    }
    if (layout.has(kScopeTableVectorscope)) {
        layout.vectorscopeOffset = next;
        next += kScopeCells * kScopeCells;
    }
    if (layout.has(kScopeTableChromaticity)) {
        layout.chromaticityOffset = next;
        next += kScopeCells * kScopeCells;
    }
    layout.wordCount = next;
    return layout;
}

} // namespace bloom::render

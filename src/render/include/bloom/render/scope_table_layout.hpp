#pragma once

// The layout of the scope count tables (#724, docs/architecture/scopes.md), shared by the CPU
// reference (src/runtime) and the GPU kernel (ScopeTablesV1) so both fill the same words.
//
// A measure is one array of unsigned 32-bit counts:
//   words[0] pixels counted, [1] not finite, [2] no chromaticity, [3] chromaticity outside,
//   [4] vectorscope outside, [5..7] zero;
//   Histogram:    4 channels (red, green, blue, luma) x 1026: bins 0..1023, under, over;
//   Waveform:     4 channels x `columns` x (levels + 2): levels 0..levels-1, under, over;
//   Vectorscope:  `cells` rows (Cr) x `cells` (Cb);
//   Chromaticity: `cells` rows (y) x `cells` (x).
// Only the tables asked for are present, in that order.
//
// A measure has a detail d, 0, 1 or 2 (kScopeMaxDetail); 0 is full detail. At detail d the
// Vectorscope and Chromaticity tables are `512 >> d` cells a side (a cell is the full-detail cell
// shifted right by d) and the Waveform has `512 >> d` levels (the full-detail level shifted right
// by d, so a column has `(512 >> d) + 2` words); the Histogram (1024 bins, under, over), the
// counters and the Waveform's columns do not change with the detail.

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
inline constexpr std::uint32_t kScopeMaxDetail = 2U; // the coarsest detail; 0 is full detail

// The values above are the full-detail (detail 0) ones. These give a detail's.
[[nodiscard]] constexpr bool validScopeDetail(const std::uint32_t detail) noexcept {
    return detail <= kScopeMaxDetail;
}
[[nodiscard]] constexpr std::uint32_t scopeCellsAtDetail(const std::uint32_t detail) noexcept {
    return kScopeCells >> detail;
}
[[nodiscard]] constexpr std::uint32_t
scopeWaveformLevelsAtDetail(const std::uint32_t detail) noexcept {
    return kScopeWaveformLevels >> detail;
}

struct ScopeTableLayout final {
    std::uint32_t scopes = 0;
    std::uint32_t columns = 0; // Waveform columns; 0 without the Waveform
    std::uint32_t detail = 0;  // 0 (full), 1 or 2
    std::uint32_t histogramOffset = 0;
    std::uint32_t waveformOffset = 0;
    std::uint32_t vectorscopeOffset = 0;
    std::uint32_t chromaticityOffset = 0;
    std::uint32_t wordCount = kScopeCounterWords;

    [[nodiscard]] constexpr bool has(const std::uint32_t scope) const noexcept {
        return (scopes & scope) != 0U;
    }
    // Waveform levels in a column, not counting the under and over words.
    [[nodiscard]] constexpr std::uint32_t waveformLevels() const noexcept {
        return kScopeWaveformLevels >> detail;
    }
    [[nodiscard]] constexpr std::uint32_t waveformLevelUnder() const noexcept {
        return waveformLevels();
    }
    [[nodiscard]] constexpr std::uint32_t waveformLevelOver() const noexcept {
        return waveformLevels() + 1U;
    }
    [[nodiscard]] constexpr std::uint32_t waveformColumnWords() const noexcept {
        return waveformLevels() + 2U;
    }
    [[nodiscard]] constexpr std::uint32_t waveformChannelWords() const noexcept {
        return columns * waveformColumnWords();
    }
    // The side of the Vectorscope and Chromaticity tables, in cells.
    [[nodiscard]] constexpr std::uint32_t cells() const noexcept { return kScopeCells >> detail; }
    [[nodiscard]] constexpr std::uint32_t histogramWords() const noexcept {
        return kScopeChannels * kScopeHistogramChannelWords;
    }
    [[nodiscard]] constexpr std::uint32_t waveformWords() const noexcept {
        return kScopeChannels * waveformChannelWords();
    }
    [[nodiscard]] constexpr std::uint32_t gridWords() const noexcept { return cells() * cells(); }
    // The bytes of the whole measure, which is what a readback moves.
    [[nodiscard]] constexpr std::uint64_t byteCount() const noexcept {
        return static_cast<std::uint64_t>(wordCount) * 4U;
    }

    friend constexpr bool operator==(const ScopeTableLayout&, const ScopeTableLayout&) = default;
};

// The layout of a measure of `scopes` over an area `measuredWidth` pixels wide at `detail`
// (0 full, 1, 2; a larger detail is clamped to kScopeMaxDetail: callers validate with
// validScopeDetail first). An absent table's offset is 0.
[[nodiscard]] constexpr ScopeTableLayout
scopeTableLayout(const std::uint32_t scopes, const std::uint32_t measuredWidth,
                 const std::uint32_t detail = 0U) noexcept {
    ScopeTableLayout layout;
    layout.scopes = scopes & kScopeTableAll;
    layout.detail = detail <= kScopeMaxDetail ? detail : kScopeMaxDetail;
    std::uint32_t next = kScopeCounterWords;
    if (layout.has(kScopeTableHistogram)) {
        layout.histogramOffset = next;
        next += layout.histogramWords();
    }
    if (layout.has(kScopeTableWaveform)) {
        const std::uint32_t maxColumns = kScopeWaveformMaxColumns >> layout.detail;
        layout.columns = measuredWidth < maxColumns ? measuredWidth : maxColumns;
        layout.waveformOffset = next;
        next += layout.waveformWords();
    }
    if (layout.has(kScopeTableVectorscope)) {
        layout.vectorscopeOffset = next;
        next += layout.gridWords();
    }
    if (layout.has(kScopeTableChromaticity)) {
        layout.chromaticityOffset = next;
        next += layout.gridWords();
    }
    layout.wordCount = next;
    return layout;
}

} // namespace bloom::render

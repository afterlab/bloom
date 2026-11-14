#pragma once

// The layout of the scope count tables (#724, #730, docs/architecture/scopes.md), shared by the
// CPU reference (src/runtime) and the GPU kernel (ScopeTablesV1) so both fill the same words.
//
// A measure is one array of unsigned 32-bit counts:
//   words[0] pixels counted, [1] not finite, [2] no chromaticity, [3] chromaticity outside,
//   [4] vectorscope outside, [5..7] zero;
//   Histogram:    4 channels (red, green, blue, luma) x 1026: bins 0..1023, under, over;
//   Waveform:     4 channels x `columns` x 1026: levels 0..1023 (the 10-bit code), under, over;
//   Vectorscope:  640 rows (Cr) x 640 (Cb) counts, then 640 x 640 luma sums: the sum, over the
//                 pixels of a cell, of the pixel's luma code shifted right by 4 (0..63), from
//                 which a cell is drawn in the mean color of its pixels;
//   Chromaticity: 512 rows (y) x 512 (x).
// Only the tables asked for are present, in that order. There is one measure per frame and no
// coarser detail.

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
inline constexpr std::uint32_t kScopeWaveformLevels = 1024U;
inline constexpr std::uint32_t kScopeWaveformLevelUnder = 1024U;
inline constexpr std::uint32_t kScopeWaveformLevelOver = 1025U;
inline constexpr std::uint32_t kScopeWaveformColumnWords = 1026U;
// The Vectorscope: cells of 1/512 in Cb and Cr from -0.625 to 0.625, so the graticule ring
// (|CbCr| = 0.60) and every 100% bar lie inside the table.
inline constexpr std::uint32_t kScopeVectorscopeCells = 640U;
inline constexpr std::uint32_t kScopeVectorscopeCellsPerUnit = 512U;
// The luma code (0..1023) of a pixel is added to its cell's luma sum shifted right by this.
inline constexpr std::uint32_t kScopeVectorscopeLumaShift = 4U;
// The CIE chromaticity table: 512 cells, x / 0.8 and y / 0.9.
inline constexpr std::uint32_t kScopeChromaticityCells = 512U;

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
    [[nodiscard]] constexpr std::uint32_t histogramWords() const noexcept {
        return kScopeChannels * kScopeHistogramChannelWords;
    }
    [[nodiscard]] constexpr std::uint32_t waveformWords() const noexcept {
        return kScopeChannels * waveformChannelWords();
    }
    // The cells of the Vectorscope's count grid; its luma sums follow, as many again.
    [[nodiscard]] constexpr std::uint32_t vectorscopeGridWords() const noexcept {
        return kScopeVectorscopeCells * kScopeVectorscopeCells;
    }
    [[nodiscard]] constexpr std::uint32_t vectorscopeWords() const noexcept {
        return 2U * vectorscopeGridWords();
    }
    [[nodiscard]] constexpr std::uint32_t vectorscopeLumaOffset() const noexcept {
        return vectorscopeOffset + vectorscopeGridWords();
    }
    [[nodiscard]] constexpr std::uint32_t chromaticityWords() const noexcept {
        return kScopeChromaticityCells * kScopeChromaticityCells;
    }
    // The bytes of the whole measure, which is what a readback moves.
    [[nodiscard]] constexpr std::uint64_t byteCount() const noexcept {
        return static_cast<std::uint64_t>(wordCount) * 4U;
    }

    friend constexpr bool operator==(const ScopeTableLayout&, const ScopeTableLayout&) = default;
};

// The layout of a measure of `scopes` over an area `measuredWidth` pixels wide: the Waveform has
// min(512, measuredWidth) columns. An absent table's offset is 0.
[[nodiscard]] constexpr ScopeTableLayout
scopeTableLayout(const std::uint32_t scopes, const std::uint32_t measuredWidth) noexcept {
    ScopeTableLayout layout;
    layout.scopes = scopes & kScopeTableAll;
    std::uint32_t next = kScopeCounterWords;
    if (layout.has(kScopeTableHistogram)) {
        layout.histogramOffset = next;
        next += layout.histogramWords();
    }
    if (layout.has(kScopeTableWaveform)) {
        layout.columns =
            measuredWidth < kScopeWaveformMaxColumns ? measuredWidth : kScopeWaveformMaxColumns;
        layout.waveformOffset = next;
        next += layout.waveformWords();
    }
    if (layout.has(kScopeTableVectorscope)) {
        layout.vectorscopeOffset = next;
        next += layout.vectorscopeWords();
    }
    if (layout.has(kScopeTableChromaticity)) {
        layout.chromaticityOffset = next;
        next += layout.chromaticityWords();
    }
    layout.wordCount = next;
    return layout;
}

// The greatest measure: every table, 512 Waveform columns. 3,186,704 words, 12,746,816 bytes: the
// bound of the coverage contract's `prep.scopes.result_readback`.
inline constexpr std::uint64_t kScopeTablesMaxBytes =
    scopeTableLayout(kScopeTableAll, kScopeWaveformMaxColumns).byteCount();
static_assert(kScopeTablesMaxBytes == 12'746'816U);

// The source pixel columns of a measured area `measuredWidth` wide that fall in Waveform column
// `column` of `columns`: the x with floor(x * columns / measuredWidth) == column. The drawing
// normalizes a column by this times the area's height.
[[nodiscard]] constexpr std::uint32_t
scopeWaveformColumnSpan(const std::uint32_t column, const std::uint32_t columns,
                        const std::uint32_t measuredWidth) noexcept {
    if (columns == 0U || column >= columns)
        return 0U;
    const std::uint64_t first =
        (static_cast<std::uint64_t>(column) * measuredWidth + columns - 1U) / columns;
    const std::uint64_t last =
        (static_cast<std::uint64_t>(column + 1U) * measuredWidth + columns - 1U) / columns;
    return static_cast<std::uint32_t>(last - first);
}

} // namespace bloom::render

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
// Only the tables asked for are present, in that order.
//
// #733: a measure for drawing is counted at the size its panes draw. A ScopeTableReduction halves
// a table axis `shift` times, and a cell of the reduced table is the exact sum of the cells of the
// full table it covers: the reduced index of a pixel is its full index shifted right. The sizes
// above are the full table (every shift 0), which a headless read still counts. The Histogram is
// never reduced; under and over are never merged with a code.

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

// How many times each table axis is halved (0: the full table).
struct ScopeTableReduction final {
    std::uint8_t waveformColumnShift = 0; // columns: ceil(full columns / 2^shift)
    std::uint8_t waveformLevelShift = 0;  // levels: 1024 >> shift
    std::uint8_t vectorscopeShift = 0;    // cells an axis: 640 >> shift
    std::uint8_t chromaticityShift = 0;   // cells an axis: 512 >> shift

    friend constexpr bool operator==(const ScopeTableReduction&,
                                     const ScopeTableReduction&) = default;
};
// The most an axis may be halved: 16 Waveform levels, 1 column, 20 Vectorscope and 16 CIE cells.
inline constexpr std::uint8_t kScopeWaveformColumnShiftMaximum = 9U;
inline constexpr std::uint8_t kScopeWaveformLevelShiftMaximum = 6U;
inline constexpr std::uint8_t kScopeVectorscopeShiftMaximum = 5U;
inline constexpr std::uint8_t kScopeChromaticityShiftMaximum = 5U;
[[nodiscard]] constexpr bool validScopeTableReduction(const ScopeTableReduction reduction) noexcept {
    return reduction.waveformColumnShift <= kScopeWaveformColumnShiftMaximum &&
           reduction.waveformLevelShift <= kScopeWaveformLevelShiftMaximum &&
           reduction.vectorscopeShift <= kScopeVectorscopeShiftMaximum &&
           reduction.chromaticityShift <= kScopeChromaticityShiftMaximum;
}
// The shift of the smallest table axis not below `devicePixels` cells, for an axis of `fullCells`:
// the greatest shift, up to `maximumShift`, whose `fullCells >> shift` is at least `devicePixels`.
[[nodiscard]] constexpr std::uint8_t scopeAxisShift(const std::uint32_t fullCells,
                                                    const std::uint32_t devicePixels,
                                                    const std::uint8_t maximumShift) noexcept {
    std::uint8_t shift = 0;
    while (shift < maximumShift && (fullCells >> (shift + 1U)) >= devicePixels &&
           (fullCells >> (shift + 1U)) > 0U) {
        ++shift;
    }
    return shift;
}

struct ScopeTableLayout final {
    std::uint32_t scopes = 0;
    // Waveform columns of this table (reduced when `reduction` says so); 0 without the Waveform.
    std::uint32_t columns = 0;
    // Waveform columns of the full table: min(512, measured width). A pixel's column is its full
    // column shifted right by reduction.waveformColumnShift.
    std::uint32_t fullColumns = 0;
    ScopeTableReduction reduction{};
    std::uint32_t histogramOffset = 0;
    std::uint32_t waveformOffset = 0;
    std::uint32_t vectorscopeOffset = 0;
    std::uint32_t chromaticityOffset = 0;
    std::uint32_t wordCount = kScopeCounterWords;

    [[nodiscard]] constexpr bool has(const std::uint32_t scope) const noexcept {
        return (scopes & scope) != 0U;
    }
    // The Waveform's levels a column (1024 for the full table), then its under and over cells.
    [[nodiscard]] constexpr std::uint32_t waveformLevels() const noexcept {
        return kScopeWaveformLevels >> reduction.waveformLevelShift;
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
    // The cell of a column for a code 0..1023, kScopeCodeUnder or kScopeCodeOver.
    [[nodiscard]] constexpr std::uint32_t waveformLevel(const std::uint32_t code) const noexcept {
        if (code < kScopeCodes) {
            return code >> reduction.waveformLevelShift;
        }
        return code == kScopeCodeUnder ? waveformLevelUnder() : waveformLevelOver();
    }
    [[nodiscard]] constexpr std::uint32_t waveformChannelWords() const noexcept {
        return columns * waveformColumnWords();
    }
    // The cells along an axis of the Vectorscope and the CIE table.
    [[nodiscard]] constexpr std::uint32_t vectorscopeCells() const noexcept {
        return kScopeVectorscopeCells >> reduction.vectorscopeShift;
    }
    [[nodiscard]] constexpr std::uint32_t chromaticityCells() const noexcept {
        return kScopeChromaticityCells >> reduction.chromaticityShift;
    }
    [[nodiscard]] constexpr std::uint32_t histogramWords() const noexcept {
        return kScopeChannels * kScopeHistogramChannelWords;
    }
    [[nodiscard]] constexpr std::uint32_t waveformWords() const noexcept {
        return kScopeChannels * waveformChannelWords();
    }
    // The cells of the Vectorscope's count grid; its luma sums follow, as many again.
    [[nodiscard]] constexpr std::uint32_t vectorscopeGridWords() const noexcept {
        return vectorscopeCells() * vectorscopeCells();
    }
    [[nodiscard]] constexpr std::uint32_t vectorscopeWords() const noexcept {
        return 2U * vectorscopeGridWords();
    }
    [[nodiscard]] constexpr std::uint32_t vectorscopeLumaOffset() const noexcept {
        return vectorscopeOffset + vectorscopeGridWords();
    }
    [[nodiscard]] constexpr std::uint32_t chromaticityWords() const noexcept {
        return chromaticityCells() * chromaticityCells();
    }
    // The bytes of the whole measure, which is what a readback moves.
    [[nodiscard]] constexpr std::uint64_t byteCount() const noexcept {
        return static_cast<std::uint64_t>(wordCount) * 4U;
    }

    friend constexpr bool operator==(const ScopeTableLayout&, const ScopeTableLayout&) = default;
};

// The layout of a measure of `scopes` over an area `measuredWidth` pixels wide: the full Waveform
// has min(512, measuredWidth) columns. An absent table's offset is 0. `reduction` must be valid
// (validScopeTableReduction).
[[nodiscard]] constexpr ScopeTableLayout
scopeTableLayout(const std::uint32_t scopes, const std::uint32_t measuredWidth,
                 const ScopeTableReduction reduction = {}) noexcept {
    ScopeTableLayout layout;
    layout.scopes = scopes & kScopeTableAll;
    layout.reduction = reduction;
    std::uint32_t next = kScopeCounterWords;
    if (layout.has(kScopeTableHistogram)) {
        layout.histogramOffset = next;
        next += layout.histogramWords();
    }
    if (layout.has(kScopeTableWaveform)) {
        layout.fullColumns =
            measuredWidth < kScopeWaveformMaxColumns ? measuredWidth : kScopeWaveformMaxColumns;
        const std::uint32_t group = 1U << reduction.waveformColumnShift;
        layout.columns = (layout.fullColumns + group - 1U) >> reduction.waveformColumnShift;
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

// The same for a column of `layout`, whose columns may each cover several full columns.
[[nodiscard]] constexpr std::uint32_t
scopeWaveformColumnSpan(const ScopeTableLayout& layout, const std::uint32_t column,
                        const std::uint32_t measuredWidth) noexcept {
    if (layout.fullColumns == 0U || column >= layout.columns)
        return 0U;
    const std::uint64_t firstFull = static_cast<std::uint64_t>(column)
                                    << layout.reduction.waveformColumnShift;
    std::uint64_t endFull = static_cast<std::uint64_t>(column + 1U)
                            << layout.reduction.waveformColumnShift;
    if (endFull > layout.fullColumns)
        endFull = layout.fullColumns;
    const std::uint64_t first =
        (firstFull * measuredWidth + layout.fullColumns - 1U) / layout.fullColumns;
    const std::uint64_t last =
        (endFull * measuredWidth + layout.fullColumns - 1U) / layout.fullColumns;
    return static_cast<std::uint32_t>(last - first);
}

} // namespace bloom::render

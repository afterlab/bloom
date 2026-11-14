#pragma once

// #724 Scopes: the measurement behind the Scopes panel and the scope reads of MCP and Python.
// docs/architecture/scopes.md is the specification.
//
// A measure counts the pixels of a measured area of one float picture (the signal) into the count
// tables of render/scope_table_layout.hpp: the Histogram, the Waveform (which the Parade also
// draws), the Vectorscope and the CIE chromaticity table. Every cell is an exact count of pixels.
// The CPU reference here and the GPU kernel (render::GpuScopeTables, ScopeTablesV1) run the same
// binary32 operations in the same order without contraction, so their tables are equal for the
// same float input.
//
// Per pixel of the measured area (a pixel outside the picture is transparent black):
//   1. The signal is (r, g, b): the picture's stored red, green and blue, which are the color
//      taken over black (straight color times alpha); a is its alpha.
//   2. Any of r, g, b, a not finite: the pixel is counted nowhere and adds 1 to `notFinite`.
//   3. With `ignoreTransparent`, a == 0: the pixel is not counted at all.
//   4. Otherwise it adds 1 to `counted` and to one cell of every table asked for:
//      luma  y = (Kr r + Kg g) + Kb b.
//      code  c(v) = floor((v * (1 / M)) * 1023 + 0.5): 0..1023; below 0 (or not a number) is
//            `under`, above 1023 is `over`. M, the axis maximum, is a power of two.
//      Histogram    channel k of (r, g, b, y): bin c(v_k), or its under / over.
//      Waveform     column floor(x * columns / W) for the pixel's x in the measured area of
//                   width W, columns = min(512 >> d, W); level (c(v_k) >> 1) >> d, or the
//                   column's under / over.
//      Vectorscope  cb = (b - y) / (2 (1 - Kb)), cr = (r - y) / (2 (1 - Kr));
//                   full cell (floor((cb + 0.5) * 512), floor((cr + 0.5) * 512)); a cell
//                   outside 0..511 adds 1 to `vectorscopeOutside`; otherwise the pixel adds to
//                   the full cell shifted right by d on each axis.
//      Chromaticity (X, Y, Z) is the pixel of the XYZ picture; s = (X + Y) + Z. Any of them not
//                   finite, or s <= 0: 1 to `noChromaticity`. Otherwise x = X / s, y = Y / s;
//                   full cell (floor(x / 0.8 * 512), floor(y / 0.9 * 512)) with 0.8 and 0.9
//                   the nearest binary32; a cell outside 0..511 adds 1 to `chromaticityOutside`;
//                   otherwise the pixel adds to the full cell shifted right by d on each axis.
// d is the measure's detail (0 full, 1, 2). At detail d the Vectorscope and Chromaticity tables
// equal the full ones with each 2^d x 2^d block of cells summed, and a Waveform column's levels
// equal sums of 2^d adjacent full-detail levels (of a measure with the same columns); the
// Histogram and the counters are the same at every detail.

#include <bloom/render/image_types.hpp>
#include <bloom/render/scope_table_layout.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace bloom::runtime {

// Display: the picture after the Viewer's display, view and look, in float, before it is packed
// to 8 bits. Working: the scene-linear process picture before the display transform.
enum class ScopeSignal : std::uint8_t { Display = 0, Working = 1 };

// Which picture a measure counted. Rendered: the frame as the evaluator or the device produced it,
// in float (both signals). Retained: the packed 8-bit display picture a retained preview frame
// carries (Display signal only; its values are quantized to 255 steps). A sidecar of one source is
// never served for the other.
enum class ScopeSource : std::uint8_t { Rendered = 0, Retained = 1 };

// The route that counted a measure. A result always names it.
enum class ScopeRoute : std::uint8_t { Cpu = 0, Gpu = 1 };

inline constexpr std::uint32_t kScopeChannelRed = 0;
inline constexpr std::uint32_t kScopeChannelGreen = 1;
inline constexpr std::uint32_t kScopeChannelBlue = 2;
inline constexpr std::uint32_t kScopeChannelLuma = 3;

// A rectangle in composition pixels.
struct ScopeRegion final {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const ScopeRegion&, const ScopeRegion&) = default;
};

// The measurement identity: with the frame's identity it is the identity of a scope sidecar.
struct ScopeMeasureSettings final {
    std::uint32_t scopes = render::kScopeTableAll; // render::kScopeTable* bits
    ScopeSignal signal = ScopeSignal::Display;
    std::uint32_t axisMaximum = 1; // M: 1 for Display; 1, 2, 4, 8 or 16 for Working
    std::optional<ScopeRegion> region;
    bool ignoreTransparent = false;
    // 0 is full detail; 1 and 2 count the Vectorscope, CIE and Waveform tables into 2x and 4x
    // coarser cells (render/scope_table_layout.hpp), which a device measure reads back in a
    // quarter and a sixteenth of the bytes. The Histogram and the counters do not change.
    std::uint32_t detail = 0;

    friend bool operator==(const ScopeMeasureSettings&, const ScopeMeasureSettings&) = default;
};

// True for 1, 2, 4, 8 and 16.
[[nodiscard]] bool validScopeAxisMaximum(std::uint32_t axisMaximum) noexcept;

// The luma coefficients of the signal's color space and the two Vectorscope denominators, each
// rounded once to binary32 here so the CPU and the GPU divide by the same number.
struct ScopeLuma final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float cbDenominator = 1.0F; // 2 * (1 - blue), computed in binary32
    float crDenominator = 1.0F; // 2 * (1 - red), computed in binary32

    friend bool operator==(const ScopeLuma&, const ScopeLuma&) = default;
};
[[nodiscard]] ScopeLuma scopeLuma(float red, float green, float blue) noexcept;

struct ScopeMeasureInput final {
    // The signal picture, row-major, `width` x `height`.
    std::span<const render::Rgba32f> signal;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The signal converted to CIE XYZ (D65), of the same extent. Required with, and read only
    // for, the Chromaticity table.
    std::span<const render::Rgba32f> xyz;
    // The measured area in picture pixels from the picture's first pixel; it may reach outside.
    std::int32_t areaX = 0;
    std::int32_t areaY = 0;
    std::uint32_t areaWidth = 0;
    std::uint32_t areaHeight = 0;
    std::uint32_t scopes = render::kScopeTableAll;
    std::uint32_t axisMaximum = 1;
    ScopeLuma luma;
    bool ignoreTransparent = false;
    std::uint32_t detail = 0; // 0, 1 or 2 (render::kScopeMaxDetail); a larger one gives nothing
};

// The count tables of one measure, as unsigned 32-bit words in the layout of
// render/scope_table_layout.hpp. The accessors return 0 for a table that was not measured.
struct ScopeCounts final {
    render::ScopeTableLayout layout;
    std::vector<std::uint32_t> words;

    [[nodiscard]] std::uint32_t counted() const noexcept;
    [[nodiscard]] std::uint32_t notFinite() const noexcept;
    [[nodiscard]] std::uint32_t noChromaticity() const noexcept;
    [[nodiscard]] std::uint32_t chromaticityOutside() const noexcept;
    [[nodiscard]] std::uint32_t vectorscopeOutside() const noexcept;
    // `index` is a code 0..1023, render::kScopeCodeUnder or render::kScopeCodeOver.
    [[nodiscard]] std::uint32_t histogram(std::uint32_t channel, std::uint32_t index) const noexcept;
    // `level` is 0..layout.waveformLevels() - 1, layout.waveformLevelUnder() or
    // layout.waveformLevelOver() (512 levels, 512 and 513 at full detail).
    [[nodiscard]] std::uint32_t waveform(std::uint32_t channel, std::uint32_t column,
                                         std::uint32_t level) const noexcept;
    [[nodiscard]] std::uint32_t vectorscope(std::uint32_t cbCell,
                                            std::uint32_t crCell) const noexcept;
    [[nodiscard]] std::uint32_t chromaticity(std::uint32_t xCell,
                                             std::uint32_t yCell) const noexcept;

    friend bool operator==(const ScopeCounts&, const ScopeCounts&) = default;
};

// The CPU reference. `cancelled`, when set, is asked between rows; a cancelled measure and an
// input that is not valid (an empty area, a picture smaller than its extent says, no XYZ picture
// with the Chromaticity table, an axis maximum that is not valid, more than 2^32 - 1 pixels in
// the area) give nothing.
[[nodiscard]] std::optional<ScopeCounts>
measureScopesCpu(const ScopeMeasureInput& input, const std::function<bool()>& cancelled = {});

// The measured area of a picture, in picture pixels from the first stored pixel of its data window.
struct ScopeMeasuredArea final {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const ScopeMeasuredArea&, const ScopeMeasuredArea&) = default;
};

// The area a measure counts: the picture's display window, cut to the frame's region of interest
// when it has one and to `region` when one is set. `region` is in composition pixels of a
// composition `compositionWidth` x `compositionHeight` and is scaled to the picture's display
// window outward (its first pixel floored, its end raised), so a region drawn at full resolution
// measures the same part of a proxy picture. Nothing when the area is empty.
[[nodiscard]] std::optional<ScopeMeasuredArea>
scopeMeasuredArea(const render::ImageWindow& dataWindow, const render::ImageWindow& displayWindow,
                  const std::optional<render::ImageWindow>& roi,
                  const std::optional<ScopeRegion>& region, std::uint32_t compositionWidth,
                  std::uint32_t compositionHeight) noexcept;

// A measure as a frame's sidecar keeps it: every table packed to the narrowest of 8, 16 or 32 bits
// that holds its largest count, exactly. The counters stay 32 bits.
class ScopeTables final {
  public:
    ScopeTables() = default;
    [[nodiscard]] static ScopeTables pack(const ScopeCounts& counts, ScopeRoute route);

    [[nodiscard]] const render::ScopeTableLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] ScopeRoute route() const noexcept { return route_; }
    [[nodiscard]] bool empty() const noexcept { return layout_.scopes == 0U; }
    // The bytes this sidecar holds, for the preview cache's budget.
    [[nodiscard]] std::size_t byteSize() const noexcept;
    // The bytes per count of one table (render::kScopeTable* bit): 1, 2 or 4; 0 when absent.
    [[nodiscard]] std::uint32_t tableWidthBytes(std::uint32_t scope) const noexcept;
    // The largest count in one table (the under and over cells included); 0 when absent.
    [[nodiscard]] std::uint32_t tableMaximum(std::uint32_t scope) const noexcept;
    // The same words pack() was given.
    [[nodiscard]] ScopeCounts unpack() const;

    [[nodiscard]] std::uint32_t counted() const noexcept;
    [[nodiscard]] std::uint32_t notFinite() const noexcept;
    [[nodiscard]] std::uint32_t noChromaticity() const noexcept;
    [[nodiscard]] std::uint32_t chromaticityOutside() const noexcept;
    [[nodiscard]] std::uint32_t vectorscopeOutside() const noexcept;
    [[nodiscard]] std::uint32_t histogram(std::uint32_t channel, std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t waveform(std::uint32_t channel, std::uint32_t column,
                                         std::uint32_t level) const noexcept;
    [[nodiscard]] std::uint32_t vectorscope(std::uint32_t cbCell,
                                            std::uint32_t crCell) const noexcept;
    [[nodiscard]] std::uint32_t chromaticity(std::uint32_t xCell,
                                             std::uint32_t yCell) const noexcept;

    friend bool operator==(const ScopeTables&, const ScopeTables&) = default;

  private:
    struct Table final {
        std::uint32_t widthBytes = 0;
        std::uint32_t maximum = 0;
        std::vector<std::uint8_t> bytes; // little-endian counts of `widthBytes` each

        friend bool operator==(const Table&, const Table&) = default;
    };
    [[nodiscard]] std::uint32_t read(const Table& table, std::size_t index) const noexcept;

    render::ScopeTableLayout layout_;
    ScopeRoute route_ = ScopeRoute::Cpu;
    std::array<std::uint32_t, render::kScopeCounterWords> counters_{};
    Table histogram_;
    Table waveform_;
    Table vectorscope_;
    Table chromaticity_;
};

// The summary a scope read can ask for instead of tables, computed from the Histogram and the
// Vectorscope tables (at a coarser detail the mean saturation and hue are from the centers of the
// detail's cells: within sqrt(2) * 2^d / 512 of full detail's saturation) (a value is 0 and
// `available` false when its table was not measured or counted nothing).
struct ScopeChannelSummary final {
    bool available = false;
    // Code values (0..1023) of the counts inside the axis: the lowest and highest bin that holds a
    // pixel, and the mean of the bins weighted by their counts.
    std::uint32_t minimum = 0;
    std::uint32_t maximum = 0;
    double mean = 0.0;
    // The code at or below which 1%, 50% and 99% of the counted pixels lie, the under cell taken
    // as below code 0 (reported as 0) and the over cell as above code 1023 (reported as 1023): the
    // smallest code whose cumulative count reaches ceil(p * total).
    std::uint32_t percentile1 = 0;
    std::uint32_t percentile50 = 0;
    std::uint32_t percentile99 = 0;
    std::uint32_t clippedUnder = 0;
    std::uint32_t clippedOver = 0;

    friend bool operator==(const ScopeChannelSummary&, const ScopeChannelSummary&) = default;
};

struct ScopeSummary final {
    std::array<ScopeChannelSummary, render::kScopeChannels> channels; // red, green, blue, luma
    bool vectorscopeAvailable = false;
    // From the centers of the Vectorscope cells, weighted by their counts: the mean distance from
    // the center, as a fraction of the graticule circle's radius (0.5 in cb, cr), and the angle of
    // the mean (cb, cr) in degrees counter-clockwise from +Cb, 0 to 360 (0 when the mean is the
    // center).
    double meanSaturation = 0.0;
    double meanHueDegrees = 0.0;

    friend bool operator==(const ScopeSummary&, const ScopeSummary&) = default;
};
[[nodiscard]] ScopeSummary summarizeScopes(const ScopeCounts& counts);

} // namespace bloom::runtime

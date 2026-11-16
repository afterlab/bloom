#include <bloom/runtime/scope_picture.hpp>

#include <bloom/core/specified_log2.hpp>
#include <bloom/render/scope_table_layout.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace bloom::runtime::scopes {
namespace {

namespace render = bloom::render;

[[nodiscard]] bool cancelledNow(const Cancelled& cancelled) { return cancelled && cancelled(); }

[[nodiscard]] ScopeImage makeImage(const PixelSize size) {
    ScopeImage image;
    image.width = size.width;
    image.height = size.height;
    image.pixels.assign(
        static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height), 0U);
    return image;
}

[[nodiscard]] std::uint32_t* rowOf(ScopeImage& image, const int y) {
    return image.pixels.data() +
           static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width);
}

[[nodiscard]] unsigned byteOf(const double value) {
    return static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

// A linear light value per component, clamped to 1, as a premultiplied pixel whose alpha is its
// largest component: drawn additively over a black canvas it is that light.
[[nodiscard]] std::uint32_t lightPixel(const double red, const double green, const double blue) {
    const auto r = byteOf(red);
    const auto g = byteOf(green);
    const auto b = byteOf(blue);
    return packArgb(std::max({r, g, b}), r, g, b);
}

// The source cells [first, last) of one output pixel `index` of `count` pixels over `cells` cells:
// one cell when magnifying, the cells it covers when reducing (every cell in exactly one pixel).
struct Span final {
    std::uint32_t first = 0;
    std::uint32_t last = 0;

    [[nodiscard]] std::uint32_t size() const noexcept { return last - first; }
};
[[nodiscard]] Span spanOf(const int index, const int count, const std::uint32_t cells) {
    const auto first = static_cast<std::uint32_t>(static_cast<std::uint64_t>(index) * cells /
                                                  static_cast<std::uint64_t>(count));
    const auto end = static_cast<std::uint32_t>(static_cast<std::uint64_t>(index + 1) * cells /
                                                static_cast<std::uint64_t>(count));
    return {first, std::min(cells, std::max(first + 1U, end))};
}

// A count is drawn spread over its neighbor levels by a tent one 8-bit step wide on each side:
// level c + d takes (4 - |d|) / 16 of it for |d| < 4. Codes 4 apart then add up to a flat line, so
// a picture whose values come in 8-bit steps (a code every 4.01) draws a continuous trace instead
// of a comb, and a 10-bit picture is softened by a couple of levels. Levels beyond the axis are
// mirrored back into it (level -1 onto 0, 1024 onto 1023), which keeps a flat spread flat to its
// ends. Values under and over the axis are not spread: they stay on the 0 and 1023 lines and in the
// end bins.
//
// #733: a table counted at pane size has cells of 2^shift codes. The same tent, in codes, is then
// applied to a count taken to lie evenly over its cell: cell c + d takes the mean, over the codes
// a of cell c, of the tent's weights that fall in cell c + d. For shift 0 that is the tent itself;
// the weights always add up to 1, and a picture in 8-bit steps still draws a continuous trace.
constexpr int kLevelSpread = 4;

struct LevelSpread final {
    // Cells on each side that take a share: 3, 2, or 1 for cells of four codes and more.
    int reach = kLevelSpread - 1;
    // weights[reach + d] for d in -reach..reach.
    std::array<double, 2 * kLevelSpread - 1> weights{};
};

[[nodiscard]] LevelSpread levelSpread(const std::uint8_t shift) {
    LevelSpread spread;
    const int cell = 1 << shift;
    spread.reach = std::min(kLevelSpread - 1, (cell + kLevelSpread - 2) / cell);
    constexpr double kNorm = static_cast<double>(kLevelSpread * kLevelSpread);
    for (int offset = -spread.reach; offset <= spread.reach; ++offset) {
        std::int64_t sum = 0;
        for (int from = 0; from < cell; ++from) {
            // The codes of cell `offset` within the tent around `from`.
            const int first = std::max(offset * cell, from - kLevelSpread + 1);
            const int last = std::min(offset * cell + cell - 1, from + kLevelSpread - 1);
            for (int to = first; to <= last; ++to)
                sum += kLevelSpread - std::abs(to - from);
        }
        spread.weights[static_cast<std::size_t>(spread.reach + offset)] =
            static_cast<double>(sum) / (kNorm * static_cast<double>(cell));
    }
    return spread;
}

void spreadLevels(const std::vector<double>& counts, std::vector<double>& spread,
                  const LevelSpread& kernel) {
    const auto levels = static_cast<int>(counts.size());
    std::fill(spread.begin(), spread.end(), 0.0);
    for (int level = 0; level < levels; ++level) {
        const double count = counts[static_cast<std::size_t>(level)];
        if (count <= 0.0)
            continue;
        for (int offset = -kernel.reach; offset <= kernel.reach; ++offset) {
            int target = level + offset;
            if (target < 0)
                target = -target - 1;
            else if (target >= levels)
                target = 2 * levels - 1 - target;
            // A table of very few levels: what would fall outside twice stays on the end cell.
            target = std::clamp(target, 0, levels - 1);
            spread[static_cast<std::size_t>(target)] +=
                count * kernel.weights[static_cast<std::size_t>(kernel.reach + offset)];
        }
    }
}

// One pane of a Waveform or Parade. Each screen column sums the table columns it shows into the
// table's levels (1024 in the full table), spread by the 8-bit step tent (values under the axis on
// the first level, over it on the last), and divides by the pixels those columns hold; each screen
// row sums the levels it covers.
void paintWaveformPane(ScopeImage& image, const int paneX, const int paneWidth,
                       const ScopeTables& tables, const std::vector<std::uint32_t>& channels,
                       const Cancelled& cancelled) {
    const auto columns = tables.layout().columns;
    if (columns == 0 || paneWidth <= 0)
        return;
    const int height = image.height;
    const auto& layout = tables.layout();
    const auto kLevels = layout.waveformLevels();
    const auto kernel = levelSpread(layout.reduction.waveformLevelShift);
    std::vector<Span> rows(static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        rows[static_cast<std::size_t>(y)] = spanOf(height - 1 - y, height, kLevels);
    // Prefix sums of the levels of one channel: prefix[l] is the count of the levels below l.
    std::vector<double> prefix(kLevels + 1U);
    std::vector<double> counts(kLevels);
    std::vector<double> spread(kLevels);
    std::vector<double> light(static_cast<std::size_t>(height) * 3U);
    for (int x = 0; x < paneWidth; ++x) {
        if (cancelledNow(cancelled))
            return;
        const auto cells = spanOf(x, paneWidth, columns);
        std::fill(light.begin(), light.end(), 0.0);
        bool any = false;
        for (const auto channel : channels) {
            std::uint64_t under = 0;
            std::uint64_t over = 0;
            for (std::uint32_t level = 0; level < kLevels; ++level) {
                std::uint64_t count = 0;
                for (auto column = cells.first; column < cells.last; ++column)
                    count += tables.waveform(channel, column, level);
                counts[level] = static_cast<double>(count);
            }
            for (auto column = cells.first; column < cells.last; ++column) {
                under += tables.waveform(channel, column, layout.waveformLevelUnder());
                over += tables.waveform(channel, column, layout.waveformLevelOver());
            }
            spreadLevels(counts, spread, kernel);
            spread[0] += static_cast<double>(under);
            spread[kLevels - 1U] += static_cast<double>(over);
            prefix[0] = 0.0;
            for (std::uint32_t level = 0; level < kLevels; ++level)
                prefix[level + 1U] = prefix[level] + spread[level];
            const double pixels = prefix[kLevels];
            if (pixels <= 0.0)
                continue;
            any = true;
            const auto& color = kWaveformTraceColors[channel];
            for (int y = 0; y < height; ++y) {
                const auto& span = rows[static_cast<std::size_t>(y)];
                const double sum = prefix[span.last] - prefix[span.first];
                if (sum <= 0.0)
                    continue;
                const double density =
                    sum / pixels * static_cast<double>(kLevels) / static_cast<double>(span.size());
                const double intensity = static_cast<double>(kWaveformTraceGain) * density;
                auto* out = light.data() + static_cast<std::size_t>(y) * 3U;
                out[0] += static_cast<double>(color.red) * intensity;
                out[1] += static_cast<double>(color.green) * intensity;
                out[2] += static_cast<double>(color.blue) * intensity;
            }
        }
        if (!any)
            continue;
        for (int y = 0; y < height; ++y) {
            const auto* in = light.data() + static_cast<std::size_t>(y) * 3U;
            if (in[0] > 0.0 || in[1] > 0.0 || in[2] > 0.0)
                rowOf(image, y)[paneX + x] = lightPixel(in[0], in[1], in[2]);
        }
    }
}

// CIE xy at Y = 1 to a displayable color: the Rec. 709 matrix, out-of-gamut chromaticities
// desaturated to the nearest in-gamut color, then scaled to full channel and encoded.
[[nodiscard]] std::array<double, 3> chromaticityColor(const double x, const double y) {
    const double yy = std::max(y, 1e-4);
    const double X = x / yy;
    const double Z = (1.0 - x - y) / yy;
    std::array<double, 3> linear{3.2406 * X - 1.5372 - 0.4986 * Z,
                                 -0.9689 * X + 1.8758 + 0.0415 * Z,
                                 0.0557 * X - 0.2040 + 1.0570 * Z};
    const double lowest = std::min({linear[0], linear[1], linear[2], 0.0});
    for (auto& value : linear)
        value -= lowest;
    const double highest = std::max({linear[0], linear[1], linear[2], 1e-9});
    std::array<double, 3> result{};
    for (std::size_t index = 0; index < 3; ++index)
        result[index] = std::pow(linear[index] / highest, 1.0 / 2.2);
    return result;
}

} // namespace

std::vector<std::uint32_t> waveformChannelList(const WaveformChannels channels) {
    if (channels == WaveformChannels::Luma)
        return {kScopeChannelLuma};
    return {kScopeChannelRed, kScopeChannelGreen, kScopeChannelBlue};
}

std::vector<std::uint32_t> histogramChannelList(const HistogramChannels channels) {
    switch (channels) {
    case HistogramChannels::Luma:
        return {kScopeChannelLuma};
    case HistogramChannels::RgbAndLuma:
        return {kScopeChannelRed, kScopeChannelGreen, kScopeChannelBlue, kScopeChannelLuma};
    case HistogramChannels::Rgb:
        break;
    }
    return {kScopeChannelRed, kScopeChannelGreen, kScopeChannelBlue};
}

PixelSize chromaticityImageSize(const int side) {
    return {std::max(1, static_cast<int>(std::lround(side * kChromaticityXMaximum))),
            std::max(1, static_cast<int>(std::lround(side * kChromaticityYMaximum)))};
}

ScopeImage mapWaveform(const ScopeTables& tables, const WaveformChannels channels,
                       const PixelSize size, const Cancelled& cancelled) {
    if (size.isEmpty() || tables.layout().columns == 0 ||
        !tables.layout().has(render::kScopeTableWaveform))
        return {};
    auto image = makeImage(size);
    paintWaveformPane(image, 0, size.width, tables, waveformChannelList(channels), cancelled);
    return cancelledNow(cancelled) ? ScopeImage{} : image;
}

ScopeImage mapParade(const ScopeTables& tables, const PixelSize size, const int gapPixels,
                     const Cancelled& cancelled) {
    if (size.isEmpty() || tables.layout().columns == 0 ||
        !tables.layout().has(render::kScopeTableWaveform))
        return {};
    auto image = makeImage(size);
    const int gap = std::max(0, gapPixels);
    const int pane = std::max(1, (size.width - 2 * gap) / 3);
    for (std::uint32_t channel = 0; channel < 3; ++channel) {
        const int left = static_cast<int>(channel) * (pane + gap);
        if (left >= size.width)
            break;
        paintWaveformPane(image, left, std::min(pane, size.width - left), tables, {channel},
                          cancelled);
    }
    return cancelledNow(cancelled) ? ScopeImage{} : image;
}

ScopeImage mapVectorscope(const ScopeTables& tables, const ScopeLuma& luma, const PixelSize size,
                          const Cancelled& cancelled) {
    if (size.isEmpty() || !tables.layout().has(render::kScopeTableVectorscope))
        return {};
    auto image = makeImage(size);
    const double counted = tables.counted();
    if (counted <= 0.0)
        return image;
    // #733: the table may be counted at pane size; a cell then covers more of the plane.
    const auto kCells = tables.layout().vectorscopeCells();
    const double kGrid = static_cast<double>(kCells) * static_cast<double>(kCells);
    constexpr double kCodeStep = static_cast<double>(1U << render::kScopeVectorscopeLumaShift);
    for (int y = 0; y < size.height; ++y) {
        if (cancelledNow(cancelled))
            return {};
        auto* row = rowOf(image, y);
        // Cr is positive up, so row y from the top is the cells from the top down.
        const auto crCells = spanOf(size.height - 1 - y, size.height, kCells);
        const double cr =
            (1.0 - (static_cast<double>(y) + 0.5) / size.height * 2.0) * kVectorscopeExtent;
        for (int x = 0; x < size.width; ++x) {
            const auto cbCells = spanOf(x, size.width, kCells);
            std::uint64_t count = 0;
            std::uint64_t lumaSum = 0;
            for (auto crCell = crCells.first; crCell < crCells.last; ++crCell)
                for (auto cbCell = cbCells.first; cbCell < cbCells.last; ++cbCell) {
                    const auto cellCount = tables.vectorscope(cbCell, crCell);
                    if (cellCount == 0)
                        continue;
                    count += cellCount;
                    lumaSum += tables.vectorscopeLuma(cbCell, crCell);
                }
            if (count == 0)
                continue;
            const double density = static_cast<double>(count) / counted * kGrid /
                                   static_cast<double>(crCells.size() * cbCells.size());
            const double intensity = static_cast<double>(kVectorscopeTraceGain) * density;
            // The mean color of the pixels: their mean luma (the middle of the shifted code) and
            // the screen pixel's own Cb and Cr, through the signal's coefficients.
            const double meanLuma =
                (static_cast<double>(lumaSum) / static_cast<double>(count) * kCodeStep +
                 kCodeStep / 2.0) /
                1023.0;
            const double cb =
                ((static_cast<double>(x) + 0.5) / size.width * 2.0 - 1.0) * kVectorscopeExtent;
            const double red = meanLuma + static_cast<double>(luma.crDenominator) * cr;
            const double blue = meanLuma + static_cast<double>(luma.cbDenominator) * cb;
            const double green = luma.green > 0.0F
                                     ? (meanLuma - static_cast<double>(luma.red) * red -
                                        static_cast<double>(luma.blue) * blue) /
                                           static_cast<double>(luma.green)
                                     : meanLuma;
            std::array<double, 3> base{std::clamp(red, 0.0, 1.0), std::clamp(green, 0.0, 1.0),
                                       std::clamp(blue, 0.0, 1.0)};
            const double largest = std::max({base[0], base[1], base[2]});
            // The hue at full strength over a third of white, like the Waveform's colors, so a
            // cell whitens as it gets denser.
            for (auto& value : base)
                value = 1.0 / 3.0 + 2.0 / 3.0 * (largest > 1e-6 ? value / largest : 1.0);
            row[x] = lightPixel(base[0] * intensity, base[1] * intensity, base[2] * intensity);
        }
    }
    return image;
}

ScopeImage mapChromaticity(const ScopeTables& tables, const PixelSize size,
                           const Cancelled& cancelled) {
    if (size.isEmpty() || !tables.layout().has(render::kScopeTableChromaticity))
        return {};
    auto image = makeImage(size);
    const auto kCells = tables.layout().chromaticityCells();
    for (int y = 0; y < size.height; ++y) {
        if (cancelledNow(cancelled))
            return {};
        auto* row = rowOf(image, y);
        const auto yCells = spanOf(size.height - 1 - y, size.height, kCells);
        const double cy = (1.0 - (y + 0.5) / size.height) * kChromaticityYMaximum;
        for (int x = 0; x < size.width; ++x) {
            const auto xCells = spanOf(x, size.width, kCells);
            bool present = false;
            for (auto yCell = yCells.first; yCell < yCells.last && !present; ++yCell)
                for (auto xCell = xCells.first; xCell < xCells.last && !present; ++xCell)
                    present = tables.chromaticity(xCell, yCell) != 0;
            if (!present)
                continue;
            const double cx = (x + 0.5) / size.width * kChromaticityXMaximum;
            const auto color = chromaticityColor(cx, cy);
            row[x] = packArgb(255U, byteOf(color[0]), byteOf(color[1]), byteOf(color[2]));
        }
    }
    return image;
}

ScopeImage mapHistogram(const ScopeTables& tables, const HistogramChannels channels,
                        const HistogramScale scale, const int rowHeight, const int width,
                        const Cancelled& cancelled) {
    if (rowHeight <= 0 || width <= 0 || !tables.layout().has(render::kScopeTableHistogram))
        return {};
    const auto list = histogramChannelList(channels);
    auto image = makeImage({width, rowHeight * static_cast<int>(list.size())});
    constexpr auto kCodes = render::kScopeCodes;
    // The Histogram is never counted smaller: its bins are the codes.
    const auto kernel = levelSpread(0);
    // The mean count of the bins of every screen column, values outside the axis in the end bins.
    const auto columns = static_cast<std::size_t>(width);
    std::vector<double> means(list.size() * columns, 0.0);
    std::vector<double> counts(kCodes);
    std::vector<double> spread(kCodes);
    double largest = 0.0;
    for (std::size_t slot = 0; slot < list.size(); ++slot) {
        const auto channel = list[slot];
        for (std::uint32_t code = 0; code < kCodes; ++code)
            counts[code] = static_cast<double>(tables.histogram(channel, code));
        spreadLevels(counts, spread, kernel);
        spread[0] += static_cast<double>(tables.histogram(channel, render::kScopeCodeUnder));
        spread[kCodes - 1U] +=
            static_cast<double>(tables.histogram(channel, render::kScopeCodeOver));
        for (int x = 0; x < width; ++x) {
            const auto bins = spanOf(x, width, kCodes);
            double sum = 0.0;
            for (auto code = bins.first; code < bins.last; ++code)
                sum += spread[code];
            const double mean = sum / static_cast<double>(bins.size());
            means[slot * columns + static_cast<std::size_t>(x)] = mean;
            largest = std::max(largest, mean);
        }
    }
    if (largest <= 0.0)
        return image;
    const auto logLargest =
        static_cast<double>(core::specifiedLog2(1.0F + static_cast<float>(largest)));
    std::vector<int> tops(columns);
    for (std::size_t slot = 0; slot < list.size(); ++slot) {
        if (cancelledNow(cancelled))
            return {};
        const auto& tint = kHistogramTraceColors[list[slot]];
        const auto red = static_cast<double>(tint.red);
        const auto green = static_cast<double>(tint.green);
        const auto blue = static_cast<double>(tint.blue);
        const auto outline = lightPixel(red, green, blue);
        // The fill lets the graticule under it show through: premultiplied at the fill share.
        const auto share = static_cast<double>(kHistogramFillShare);
        const auto fill = packArgb(byteOf(share), byteOf(red * share), byteOf(green * share),
                                   byteOf(blue * share));
        const int rowTop = static_cast<int>(slot) * rowHeight;
        for (int x = 0; x < width; ++x) {
            const double mean = means[slot * columns + static_cast<std::size_t>(x)];
            int pixels = 0;
            if (mean > 0.0) {
                const double fraction = scale == HistogramScale::Linear
                                            ? mean / largest
                                            : static_cast<double>(core::specifiedLog2(
                                                  1.0F + static_cast<float>(mean))) /
                                                  logLargest;
                // A bin with a count is at least one pixel tall.
                pixels = std::clamp(
                    static_cast<int>(std::lround(fraction * kHistogramHeadroom * rowHeight)), 1,
                    rowHeight);
            }
            tops[static_cast<std::size_t>(x)] = pixels;
            for (int step = 0; step < pixels; ++step)
                rowOf(image, rowTop + rowHeight - 1 - step)[x] = fill;
        }
        // The outline runs along the top of the curve; a step between columns is joined.
        for (int x = 0; x < width; ++x) {
            const int pixels = tops[static_cast<std::size_t>(x)];
            if (pixels == 0)
                continue;
            const int previous = x > 0 ? tops[static_cast<std::size_t>(x - 1)] : pixels;
            const int low = std::min(pixels, std::max(previous, 1));
            for (int level = low; level <= pixels; ++level)
                rowOf(image, rowTop + rowHeight - level)[x] = outline;
        }
    }
    return image;
}

ScopePaneCells scopePaneCells(const Kind kind, const PixelSize traceSize,
                              const int paradeGapPixels) {
    ScopePaneCells cells;
    if (traceSize.isEmpty())
        return cells;
    const auto width = static_cast<std::uint32_t>(traceSize.width);
    const auto height = static_cast<std::uint32_t>(traceSize.height);
    switch (kind) {
    case Kind::Waveform:
        cells.waveformColumns = width;
        cells.waveformLevels = height;
        break;
    case Kind::Parade: {
        // The channel pane mapParade() paints.
        const int gap = std::max(0, paradeGapPixels);
        cells.waveformColumns =
            static_cast<std::uint32_t>(std::max(1, (traceSize.width - 2 * gap) / 3));
        cells.waveformLevels = height;
        break;
    }
    case Kind::Vectorscope:
        cells.vectorscope = std::max(width, height);
        break;
    case Kind::Histogram:
        cells.histogram = true;
        break;
    case Kind::Chromaticity: {
        // The image renderTrace() maps for an axes' side of `traceSize.width`.
        const auto image = chromaticityImageSize(traceSize.width);
        cells.chromaticity = static_cast<std::uint32_t>(std::max(image.width, image.height));
        break;
    }
    }
    return cells;
}

ScopePaneCells mergeScopePaneCells(const ScopePaneCells& left,
                                   const ScopePaneCells& right) noexcept {
    return {.waveformColumns = std::max(left.waveformColumns, right.waveformColumns),
            .waveformLevels = std::max(left.waveformLevels, right.waveformLevels),
            .vectorscope = std::max(left.vectorscope, right.vectorscope),
            .chromaticity = std::max(left.chromaticity, right.chromaticity),
            .histogram = left.histogram || right.histogram};
}

std::uint32_t scopeTablesFor(const ScopePaneCells& cells) noexcept {
    std::uint32_t tables = 0;
    if (cells.histogram)
        tables |= render::kScopeTableHistogram;
    if (cells.waveformColumns != 0U && cells.waveformLevels != 0U)
        tables |= render::kScopeTableWaveform;
    if (cells.vectorscope != 0U)
        tables |= render::kScopeTableVectorscope;
    if (cells.chromaticity != 0U)
        tables |= render::kScopeTableChromaticity;
    return tables;
}

render::ScopeTableReduction scopeTableReductionFor(const ScopePaneCells& cells) noexcept {
    render::ScopeTableReduction reduction;
    if (cells.waveformColumns != 0U && cells.waveformLevels != 0U) {
        reduction.waveformColumnShift =
            render::scopeAxisShift(render::kScopeWaveformMaxColumns, cells.waveformColumns,
                                   render::kScopeWaveformColumnShiftMaximum);
        reduction.waveformLevelShift =
            render::scopeAxisShift(render::kScopeWaveformLevels, cells.waveformLevels,
                                   render::kScopeWaveformLevelShiftMaximum);
    }
    if (cells.vectorscope != 0U)
        reduction.vectorscopeShift =
            render::scopeAxisShift(render::kScopeVectorscopeCells, cells.vectorscope,
                                   render::kScopeVectorscopeShiftMaximum);
    if (cells.chromaticity != 0U)
        reduction.chromaticityShift =
            render::scopeAxisShift(render::kScopeChromaticityCells, cells.chromaticity,
                                   render::kScopeChromaticityShiftMaximum);
    return reduction;
}

ScopeImage renderTrace(const TraceRequest& request, const Cancelled& cancelled) {
    if (request.tables == nullptr || request.tables->empty())
        return {};
    const auto& tables = *request.tables;
    switch (request.kind) {
    case Kind::Waveform:
        return mapWaveform(tables, request.waveformChannels, request.size, cancelled);
    case Kind::Parade:
        return mapParade(tables, request.size, request.gapPixels, cancelled);
    case Kind::Vectorscope:
        if (request.luma != nullptr)
            return mapVectorscope(tables, *request.luma, request.size, cancelled);
        return {};
    case Kind::Histogram:
        return mapHistogram(tables, request.histogramChannels, request.histogramScale,
                            request.size.height, request.size.width, cancelled);
    case Kind::Chromaticity:
        return mapChromaticity(tables, chromaticityImageSize(request.size.width), cancelled);
    }
    return {};
}

} // namespace bloom::runtime::scopes

#include <bloom/ui/scopes_drawing.hpp>

#include <bloom/core/specified_log2.hpp>
#include <bloom/render/scope_table_layout.hpp>

#include <QColor>

#include <algorithm>
#include <cmath>

namespace bloom::ui::scopes {
namespace {

namespace render = bloom::render;

// A dark tint of a channel color: the histogram's filled area.
constexpr float kHistogramFillOpacity = 0.35F;

[[nodiscard]] bool cancelledNow(const Cancelled& cancelled) { return cancelled && cancelled(); }

[[nodiscard]] QImage makeImage(const QSize size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

[[nodiscard]] std::uint8_t byteOf(const float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

// The source cells [first, last) of one output pixel `index` of `count` pixels over `cells` cells:
// one cell when magnifying, the cells it covers when reducing.
struct Span final {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
};
[[nodiscard]] Span spanOf(const int index, const int count, const std::uint32_t cells) {
    const auto first = static_cast<std::uint32_t>(static_cast<std::uint64_t>(index) * cells /
                                                  static_cast<std::uint64_t>(count));
    const auto end = static_cast<std::uint32_t>(static_cast<std::uint64_t>(index + 1) * cells /
                                                static_cast<std::uint64_t>(count));
    return {first, std::min(cells, std::max(first + 1U, end))};
}

struct Accumulator final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;

    void add(const QRgb color, const float weight) {
        red += static_cast<float>(qRed(color)) * weight;
        green += static_cast<float>(qGreen(color)) * weight;
        blue += static_cast<float>(qBlue(color)) * weight;
    }
    [[nodiscard]] QRgb premultiplied() const {
        const auto r = static_cast<int>(std::lround(std::min(red, 255.0F)));
        const auto g = static_cast<int>(std::lround(std::min(green, 255.0F)));
        const auto b = static_cast<int>(std::lround(std::min(blue, 255.0F)));
        return qRgba(r, g, b, std::max({r, g, b}));
    }
};

void paintWaveformPane(QImage& image, const int paneX, const int paneWidth,
                       const runtime::ScopeTables& tables,
                       const std::vector<std::uint32_t>& channels, const TraceColors& colors,
                       const float gain, const Cancelled& cancelled) {
    const auto columns = tables.layout().columns;
    const auto largest = tables.tableMaximum(render::kScopeTableWaveform);
    if (columns == 0 || paneWidth <= 0)
        return;
    const int height = image.height();
    for (int y = 0; y < height; ++y) {
        if (cancelledNow(cancelled))
            return;
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        // Row y from the top is the levels from the top down.
        const auto levels = spanOf(height - 1 - y, height, render::kScopeWaveformLevels);
        for (int x = 0; x < paneWidth; ++x) {
            const auto cells = spanOf(x, paneWidth, columns);
            Accumulator sum;
            bool any = false;
            for (const auto channel : channels) {
                std::uint32_t count = 0;
                for (auto column = cells.first; column < cells.last; ++column)
                    for (auto level = levels.first; level < levels.last; ++level)
                        count = std::max(count, tables.waveform(channel, column, level));
                if (count == 0)
                    continue;
                any = true;
                sum.add(colors.channel[channel], traceBrightness(count, largest, gain));
            }
            if (any)
                row[paneX + x] = sum.premultiplied();
        }
    }
}

[[nodiscard]] QRgb lerpToWhite(const std::array<float, 3>& base, const float amount,
                               const float brightness) {
    const auto channel = [&](const float value) {
        return byteOf((value + (1.0F - value) * amount) * brightness);
    };
    const auto alpha = byteOf(brightness);
    // Premultiplied: no channel may exceed alpha.
    return qRgba(std::min<int>(channel(base[0]), alpha), std::min<int>(channel(base[1]), alpha),
                 std::min<int>(channel(base[2]), alpha), alpha);
}

} // namespace

float traceBrightness(const std::uint32_t count, const std::uint32_t largest,
                      const float gain) noexcept {
    if (count == 0 || largest == 0)
        return 0.0F;
    const auto ratio = core::specifiedLog2(1.0F + static_cast<float>(count)) /
                       core::specifiedLog2(1.0F + static_cast<float>(largest));
    return std::max(kTraceFloor, std::min(1.0F, ratio * gain));
}

std::vector<std::uint32_t> waveformChannelList(const WaveformChannels channels) {
    if (channels == WaveformChannels::Luma)
        return {runtime::kScopeChannelLuma};
    return {runtime::kScopeChannelRed, runtime::kScopeChannelGreen, runtime::kScopeChannelBlue};
}

std::vector<std::uint32_t> histogramChannelList(const HistogramChannels channels) {
    switch (channels) {
    case HistogramChannels::Luma:
        return {runtime::kScopeChannelLuma};
    case HistogramChannels::RgbAndLuma:
        return {runtime::kScopeChannelRed, runtime::kScopeChannelGreen, runtime::kScopeChannelBlue,
                runtime::kScopeChannelLuma};
    case HistogramChannels::Rgb:
        break;
    }
    return {runtime::kScopeChannelRed, runtime::kScopeChannelGreen, runtime::kScopeChannelBlue};
}

QSize chromaticityImageSize(const int side) {
    return {std::max(1, static_cast<int>(std::lround(side * kChromaticityXMaximum))),
            std::max(1, static_cast<int>(std::lround(side * kChromaticityYMaximum)))};
}

QImage mapWaveform(const runtime::ScopeTables& tables, const WaveformChannels channels,
                   const TraceColors& colors, const float gain, const QSize size,
                   const Cancelled& cancelled) {
    if (size.isEmpty() || tables.layout().columns == 0 ||
        !tables.layout().has(render::kScopeTableWaveform))
        return {};
    auto image = makeImage(size);
    paintWaveformPane(image, 0, size.width(), tables, waveformChannelList(channels), colors, gain,
                      cancelled);
    return cancelledNow(cancelled) ? QImage{} : image;
}

QImage mapParade(const runtime::ScopeTables& tables, const TraceColors& colors, const float gain,
                 const QSize size, const int gapPixels, const Cancelled& cancelled) {
    if (size.isEmpty() || tables.layout().columns == 0 ||
        !tables.layout().has(render::kScopeTableWaveform))
        return {};
    auto image = makeImage(size);
    const int gap = std::max(0, gapPixels);
    const int pane = std::max(1, (size.width() - 2 * gap) / 3);
    for (std::uint32_t channel = 0; channel < 3; ++channel) {
        const int left = static_cast<int>(channel) * (pane + gap);
        if (left >= size.width())
            break;
        paintWaveformPane(image, left, std::min(pane, size.width() - left), tables, {channel},
                          colors, gain, cancelled);
    }
    return cancelledNow(cancelled) ? QImage{} : image;
}

QImage mapVectorscope(const runtime::ScopeTables& tables, const runtime::ScopeLuma& luma,
                      const float gain, const QSize size, const Cancelled& cancelled) {
    if (size.isEmpty() || !tables.layout().has(render::kScopeTableVectorscope))
        return {};
    auto image = makeImage(size);
    const auto largest = tables.tableMaximum(render::kScopeTableVectorscope);
    const auto cells = render::kScopeCells;
    for (int y = 0; y < size.height(); ++y) {
        if (cancelledNow(cancelled))
            return {};
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        // Cr is positive up, so row y from the top is the cells from the top down.
        const auto crCells = spanOf(size.height() - 1 - y, size.height(), cells);
        const float cr = 0.5F - (static_cast<float>(y) + 0.5F) / static_cast<float>(size.height());
        for (int x = 0; x < size.width(); ++x) {
            const auto cbCells = spanOf(x, size.width(), cells);
            std::uint32_t count = 0;
            for (auto crCell = crCells.first; crCell < crCells.last; ++crCell)
                for (auto cbCell = cbCells.first; cbCell < cbCells.last; ++cbCell)
                    count = std::max(count, tables.vectorscope(cbCell, crCell));
            if (count == 0)
                continue;
            const float cb =
                (static_cast<float>(x) + 0.5F) / static_cast<float>(size.width()) - 0.5F;
            // The cell's own chroma at a fixed luma, through the same coefficients as the measure.
            const float red = kVectorscopeCellLuma + luma.crDenominator * cr;
            const float blue = kVectorscopeCellLuma + luma.cbDenominator * cb;
            const float green =
                luma.green > 0.0F
                    ? (kVectorscopeCellLuma - luma.red * red - luma.blue * blue) / luma.green
                    : kVectorscopeCellLuma;
            const std::array<float, 3> base{std::clamp(red, 0.0F, 1.0F),
                                            std::clamp(green, 0.0F, 1.0F),
                                            std::clamp(blue, 0.0F, 1.0F)};
            const auto brightness = traceBrightness(count, largest, gain);
            row[x] = lerpToWhite(base, brightness * brightness, brightness);
        }
    }
    return image;
}

namespace {

// CIE xy at Y = 1 to a displayable color: the Rec. 709 matrix, out-of-gamut chromaticities
// desaturated to the nearest in-gamut color, then scaled to full channel and encoded.
[[nodiscard]] std::array<float, 3> chromaticityColor(const double x, const double y) {
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
    std::array<float, 3> result{};
    for (std::size_t index = 0; index < 3; ++index)
        result[index] = static_cast<float>(std::pow(linear[index] / highest, 1.0 / 2.2));
    return result;
}

} // namespace

QImage mapChromaticity(const runtime::ScopeTables& tables, const float gain, const QSize size,
                       const Cancelled& cancelled) {
    if (size.isEmpty() || !tables.layout().has(render::kScopeTableChromaticity))
        return {};
    auto image = makeImage(size);
    const auto largest = tables.tableMaximum(render::kScopeTableChromaticity);
    const auto cells = render::kScopeCells;
    for (int y = 0; y < size.height(); ++y) {
        if (cancelledNow(cancelled))
            return {};
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        const auto yCells = spanOf(size.height() - 1 - y, size.height(), cells);
        const double cy = (1.0 - (y + 0.5) / size.height()) * kChromaticityYMaximum;
        for (int x = 0; x < size.width(); ++x) {
            const auto xCells = spanOf(x, size.width(), cells);
            std::uint32_t count = 0;
            for (auto yCell = yCells.first; yCell < yCells.last; ++yCell)
                for (auto xCell = xCells.first; xCell < xCells.last; ++xCell)
                    count = std::max(count, tables.chromaticity(xCell, yCell));
            if (count == 0)
                continue;
            const double cx = (x + 0.5) / size.width() * kChromaticityXMaximum;
            const auto brightness = traceBrightness(count, largest, gain);
            row[x] = lerpToWhite(chromaticityColor(cx, cy), brightness * brightness, brightness);
        }
    }
    return image;
}

QImage mapHistogram(const runtime::ScopeTables& tables, const HistogramChannels channels,
                    const HistogramScale scale, const TraceColors& colors, const int rowHeight,
                    const int width, const Cancelled& cancelled) {
    if (rowHeight <= 0 || width <= 0 || !tables.layout().has(render::kScopeTableHistogram))
        return {};
    const auto list = histogramChannelList(channels);
    auto image = makeImage({width, rowHeight * static_cast<int>(list.size())});
    // One scale for every row: the largest bin of the channels drawn.
    std::uint32_t largest = 0;
    for (const auto channel : list)
        for (std::uint32_t code = 0; code < render::kScopeCodes; ++code)
            largest = std::max(largest, tables.histogram(channel, code));
    if (largest == 0)
        return image;
    const float logLargest = core::specifiedLog2(1.0F + static_cast<float>(largest));
    std::vector<int> tops(static_cast<std::size_t>(width));
    for (std::size_t slot = 0; slot < list.size(); ++slot) {
        if (cancelledNow(cancelled))
            return {};
        const auto channel = list[slot];
        const QColor tint = QColor::fromRgb(colors.channel[channel]);
        const auto outline = qRgba(tint.red(), tint.green(), tint.blue(), 255);
        const auto fill =
            qRgba(byteOf(tint.redF() * kHistogramFillOpacity),
                  byteOf(tint.greenF() * kHistogramFillOpacity),
                  byteOf(tint.blueF() * kHistogramFillOpacity), byteOf(kHistogramFillOpacity));
        const int rowTop = static_cast<int>(slot) * rowHeight;
        for (int x = 0; x < width; ++x) {
            const auto bins = spanOf(x, width, render::kScopeCodes);
            std::uint32_t count = 0;
            for (auto code = bins.first; code < bins.last; ++code)
                count = std::max(count, tables.histogram(channel, code));
            int pixels = 0;
            if (count != 0) {
                const float fraction =
                    scale == HistogramScale::Linear
                        ? static_cast<float>(count) / static_cast<float>(largest)
                        : core::specifiedLog2(1.0F + static_cast<float>(count)) / logLargest;
                // A single pixel is never hidden: a bin with a count is at least one pixel tall.
                pixels = std::clamp(
                    static_cast<int>(std::ceil(fraction * static_cast<float>(rowHeight))), 1,
                    rowHeight);
            }
            tops[static_cast<std::size_t>(x)] = pixels;
            for (int step = 0; step < pixels; ++step)
                reinterpret_cast<QRgb*>(image.scanLine(rowTop + rowHeight - 1 - step))[x] = fill;
        }
        // The outline runs along the top of the curve; a step between columns is joined.
        for (int x = 0; x < width; ++x) {
            const int pixels = tops[static_cast<std::size_t>(x)];
            if (pixels == 0)
                continue;
            const int previous = x > 0 ? tops[static_cast<std::size_t>(x - 1)] : pixels;
            const int low = std::min(pixels, std::max(previous, 1));
            for (int level = low; level <= pixels; ++level)
                reinterpret_cast<QRgb*>(image.scanLine(rowTop + rowHeight - level))[x] = outline;
        }
    }
    return image;
}

ClipSummary clipSummary(const runtime::ScopeTables& tables, const Kind kind,
                        const WaveformChannels waveformChannels,
                        const HistogramChannels histogramChannels) {
    ClipSummary summary;
    if (kind == Kind::Histogram && tables.layout().has(render::kScopeTableHistogram)) {
        for (const auto channel : histogramChannelList(histogramChannels)) {
            summary.under[channel] = tables.histogram(channel, render::kScopeCodeUnder);
            summary.over[channel] = tables.histogram(channel, render::kScopeCodeOver);
        }
    } else if ((kind == Kind::Waveform || kind == Kind::Parade) &&
               tables.layout().has(render::kScopeTableWaveform)) {
        const auto channels = kind == Kind::Parade ? waveformChannelList(WaveformChannels::Rgb)
                                                   : waveformChannelList(waveformChannels);
        summary.columns.assign(tables.layout().columns, 0);
        for (std::uint32_t column = 0; column < tables.layout().columns; ++column)
            for (const auto channel : channels) {
                if (tables.waveform(channel, column, render::kScopeWaveformLevelUnder) != 0)
                    summary.columns[column] |= 1U;
                if (tables.waveform(channel, column, render::kScopeWaveformLevelOver) != 0)
                    summary.columns[column] |= 2U;
            }
    }
    return summary;
}

TraceResult renderTrace(const TraceRequest& request, const Cancelled& cancelled) {
    TraceResult result;
    if (request.tables == nullptr || request.tables->empty())
        return result;
    const auto& tables = *request.tables;
    switch (request.kind) {
    case Kind::Waveform:
        result.image = mapWaveform(tables, request.waveformChannels, request.colors, request.gain,
                                   request.size, cancelled);
        break;
    case Kind::Parade:
        result.image = mapParade(tables, request.colors, request.gain, request.size,
                                 request.gapPixels, cancelled);
        break;
    case Kind::Vectorscope:
        if (request.color != nullptr)
            result.image = mapVectorscope(tables, request.color->luma(), request.gain, request.size,
                                          cancelled);
        break;
    case Kind::Histogram:
        result.image =
            mapHistogram(tables, request.histogramChannels, request.histogramScale, request.colors,
                         request.size.height(), request.size.width(), cancelled);
        break;
    case Kind::Chromaticity:
        result.image = mapChromaticity(tables, request.gain,
                                       chromaticityImageSize(request.size.width()), cancelled);
        break;
    }
    result.clip =
        clipSummary(tables, request.kind, request.waveformChannels, request.histogramChannels);
    return result;
}

} // namespace bloom::ui::scopes

#include <bloom/runtime/scope_measure.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

// The binary32 arithmetic below is the definition the GPU kernel repeats operation for operation:
// every intermediate lives in its own float, nothing is contracted (this file is built with strict
// floating point), and no double is involved in a count.

namespace bloom::runtime {
namespace {

using render::ScopeTableLayout;

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

[[nodiscard]] bool finiteBits(const float value) noexcept {
    return (std::bit_cast<std::uint32_t>(value) & 0x7F800000U) != 0x7F800000U;
}

[[nodiscard]] std::uint32_t scopeCode(const float value, const float inverseMaximum) noexcept {
    const float scaled = value * inverseMaximum;
    const float coded = scaled * 1023.0F;
    const float rounded = coded + 0.5F;
    const float whole = std::floor(rounded);
    if (!(whole >= 0.0F)) {
        return render::kScopeCodeUnder;
    }
    if (!(whole <= 1023.0F)) {
        return render::kScopeCodeOver;
    }
    return static_cast<std::uint32_t>(whole);
}

[[nodiscard]] std::uint32_t scopeCell(const float value) noexcept {
    const float scaled = value * 512.0F;
    const float whole = std::floor(scaled);
    if (!(whole >= 0.0F) || !(whole <= 511.0F)) {
        return 512U;
    }
    return static_cast<std::uint32_t>(whole);
}

[[nodiscard]] std::uint32_t scopeLevel(const std::uint32_t code) noexcept {
    return code < 1024U ? code >> 1U : code - 512U;
}

// The word index of a cell, or kNone when the table is absent or the cell is out of range.
[[nodiscard]] std::size_t histogramIndex(const ScopeTableLayout& layout,
                                         const std::uint32_t channel,
                                         const std::uint32_t index) noexcept {
    if (!layout.has(render::kScopeTableHistogram) || channel >= render::kScopeChannels ||
        index >= render::kScopeHistogramChannelWords) {
        return kNone;
    }
    return static_cast<std::size_t>(layout.histogramOffset) +
           static_cast<std::size_t>(channel) * render::kScopeHistogramChannelWords + index;
}

[[nodiscard]] std::size_t waveformIndex(const ScopeTableLayout& layout, const std::uint32_t channel,
                                        const std::uint32_t column,
                                        const std::uint32_t level) noexcept {
    if (!layout.has(render::kScopeTableWaveform) || channel >= render::kScopeChannels ||
        column >= layout.columns || level >= render::kScopeWaveformColumnWords) {
        return kNone;
    }
    return static_cast<std::size_t>(layout.waveformOffset) +
           static_cast<std::size_t>(channel) * layout.waveformChannelWords() +
           static_cast<std::size_t>(column) * render::kScopeWaveformColumnWords + level;
}

[[nodiscard]] std::size_t gridIndex(const ScopeTableLayout& layout, const std::uint32_t scope,
                                    const std::uint32_t offset, const std::uint32_t xCell,
                                    const std::uint32_t yCell) noexcept {
    if (!layout.has(scope) || xCell >= render::kScopeCells || yCell >= render::kScopeCells) {
        return kNone;
    }
    return static_cast<std::size_t>(offset) +
           static_cast<std::size_t>(yCell) * render::kScopeCells + xCell;
}

[[nodiscard]] std::uint32_t wordAt(const std::vector<std::uint32_t>& words,
                                   const std::size_t index) noexcept {
    return index < words.size() ? words[index] : 0U;
}

} // namespace

bool validScopeAxisMaximum(const std::uint32_t axisMaximum) noexcept {
    return axisMaximum == 1U || axisMaximum == 2U || axisMaximum == 4U || axisMaximum == 8U ||
           axisMaximum == 16U;
}

ScopeLuma scopeLuma(const float red, const float green, const float blue) noexcept {
    ScopeLuma luma;
    luma.red = red;
    luma.green = green;
    luma.blue = blue;
    const float oneMinusBlue = 1.0F - blue;
    const float oneMinusRed = 1.0F - red;
    luma.cbDenominator = 2.0F * oneMinusBlue;
    luma.crDenominator = 2.0F * oneMinusRed;
    return luma;
}

std::uint32_t ScopeCounts::counted() const noexcept {
    return wordAt(words, render::kScopeCounterCounted);
}
std::uint32_t ScopeCounts::notFinite() const noexcept {
    return wordAt(words, render::kScopeCounterNotFinite);
}
std::uint32_t ScopeCounts::noChromaticity() const noexcept {
    return wordAt(words, render::kScopeCounterNoChromaticity);
}
std::uint32_t ScopeCounts::chromaticityOutside() const noexcept {
    return wordAt(words, render::kScopeCounterChromaticityOutside);
}
std::uint32_t ScopeCounts::vectorscopeOutside() const noexcept {
    return wordAt(words, render::kScopeCounterVectorscopeOutside);
}
std::uint32_t ScopeCounts::histogram(const std::uint32_t channel,
                                     const std::uint32_t index) const noexcept {
    const std::size_t at = histogramIndex(layout, channel, index);
    return at == kNone ? 0U : wordAt(words, at);
}
std::uint32_t ScopeCounts::waveform(const std::uint32_t channel, const std::uint32_t column,
                                    const std::uint32_t level) const noexcept {
    const std::size_t at = waveformIndex(layout, channel, column, level);
    return at == kNone ? 0U : wordAt(words, at);
}
std::uint32_t ScopeCounts::vectorscope(const std::uint32_t cbCell,
                                       const std::uint32_t crCell) const noexcept {
    const std::size_t at =
        gridIndex(layout, render::kScopeTableVectorscope, layout.vectorscopeOffset, cbCell, crCell);
    return at == kNone ? 0U : wordAt(words, at);
}
std::uint32_t ScopeCounts::chromaticity(const std::uint32_t xCell,
                                        const std::uint32_t yCell) const noexcept {
    const std::size_t at =
        gridIndex(layout, render::kScopeTableChromaticity, layout.chromaticityOffset, xCell, yCell);
    return at == kNone ? 0U : wordAt(words, at);
}

std::optional<ScopeCounts> measureScopesCpu(const ScopeMeasureInput& input,
                                            const std::function<bool()>& cancelled) {
    if (input.areaWidth == 0U || input.areaHeight == 0U ||
        !validScopeAxisMaximum(input.axisMaximum)) {
        return std::nullopt;
    }
    const std::uint64_t areaPixels = static_cast<std::uint64_t>(input.areaWidth) * input.areaHeight;
    if (areaPixels > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    const std::uint64_t pictureCount = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.signal.size() < pictureCount) {
        return std::nullopt;
    }
    const std::uint32_t scopes = input.scopes & render::kScopeTableAll;
    const bool wantChromaticity = (scopes & render::kScopeTableChromaticity) != 0U;
    if (wantChromaticity && input.xyz.size() < pictureCount) {
        return std::nullopt;
    }

    ScopeCounts counts;
    counts.layout = render::scopeTableLayout(scopes, input.areaWidth);
    counts.words.assign(counts.layout.wordCount, 0U);
    const ScopeTableLayout& layout = counts.layout;
    std::vector<std::uint32_t>& words = counts.words;

    const float inverseMaximum = 1.0F / static_cast<float>(input.axisMaximum);
    const float limitX = std::bit_cast<float>(0x3F4CCCCDU); // 0.8
    const float limitY = std::bit_cast<float>(0x3F666666U); // 0.9
    const ScopeLuma& k = input.luma;

    for (std::uint32_t y = 0; y < input.areaHeight; ++y) {
        if (cancelled && cancelled()) {
            return std::nullopt;
        }
        for (std::uint32_t x = 0; x < input.areaWidth; ++x) {
            const std::int64_t px = static_cast<std::int64_t>(input.areaX) + x;
            const std::int64_t py = static_cast<std::int64_t>(input.areaY) + y;
            const bool inside = px >= 0 && py >= 0 && px < static_cast<std::int64_t>(input.width) &&
                                py < static_cast<std::int64_t>(input.height);
            const std::size_t index =
                inside ? static_cast<std::size_t>(py) * input.width + static_cast<std::size_t>(px)
                       : 0U;
            const render::Rgba32f signal =
                inside ? input.signal[index] : render::Rgba32f::transparent();
            const float r = signal.red();
            const float g = signal.green();
            const float b = signal.blue();
            const float a = signal.alpha();
            if (!finiteBits(r) || !finiteBits(g) || !finiteBits(b) || !finiteBits(a)) {
                ++words[render::kScopeCounterNotFinite];
                continue;
            }
            if (input.ignoreTransparent && a == 0.0F) {
                continue;
            }
            ++words[render::kScopeCounterCounted];

            const float lumaRed = k.red * r;
            const float lumaGreen = k.green * g;
            const float lumaBlue = k.blue * b;
            const float lumaRedGreen = lumaRed + lumaGreen;
            const float luma = lumaRedGreen + lumaBlue;
            const std::uint32_t codes[4] = {
                scopeCode(r, inverseMaximum), scopeCode(g, inverseMaximum),
                scopeCode(b, inverseMaximum), scopeCode(luma, inverseMaximum)};
            if (layout.has(render::kScopeTableHistogram)) {
                for (std::uint32_t channel = 0; channel < render::kScopeChannels; ++channel) {
                    ++words[histogramIndex(layout, channel, codes[channel])];
                }
            }
            if (layout.has(render::kScopeTableWaveform)) {
                const std::uint32_t column = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(x) * layout.columns / input.areaWidth);
                for (std::uint32_t channel = 0; channel < render::kScopeChannels; ++channel) {
                    ++words[waveformIndex(layout, channel, column, scopeLevel(codes[channel]))];
                }
            }
            if (layout.has(render::kScopeTableVectorscope)) {
                const float cbDifference = b - luma;
                const float cb = cbDifference / k.cbDenominator;
                const float cbShifted = cb + 0.5F;
                const float crDifference = r - luma;
                const float cr = crDifference / k.crDenominator;
                const float crShifted = cr + 0.5F;
                const std::uint32_t column = scopeCell(cbShifted);
                const std::uint32_t row = scopeCell(crShifted);
                if (column == 512U || row == 512U) {
                    ++words[render::kScopeCounterVectorscopeOutside];
                } else {
                    ++words[gridIndex(layout, render::kScopeTableVectorscope,
                                      layout.vectorscopeOffset, column, row)];
                }
            }
            if (wantChromaticity) {
                const render::Rgba32f xyz =
                    inside ? input.xyz[index] : render::Rgba32f::transparent();
                const float xy = xyz.red() + xyz.green();
                const float sum = xy + xyz.blue();
                if (!finiteBits(xyz.red()) || !finiteBits(xyz.green()) || !finiteBits(xyz.blue()) ||
                    !(sum > 0.0F)) {
                    ++words[render::kScopeCounterNoChromaticity];
                } else {
                    const float cx = xyz.red() / sum;
                    const float cxScaled = cx / limitX;
                    const float cy = xyz.green() / sum;
                    const float cyScaled = cy / limitY;
                    const std::uint32_t column = scopeCell(cxScaled);
                    const std::uint32_t row = scopeCell(cyScaled);
                    if (column == 512U || row == 512U) {
                        ++words[render::kScopeCounterChromaticityOutside];
                    } else {
                        ++words[gridIndex(layout, render::kScopeTableChromaticity,
                                          layout.chromaticityOffset, column, row)];
                    }
                }
            }
        }
    }
    return counts;
}

// ---------------------------------------------------------------------------------------------
// ScopeTables

namespace {

[[nodiscard]] std::uint32_t widthFor(const std::uint32_t maximum) noexcept {
    if (maximum <= 0xFFU) {
        return 1U;
    }
    return maximum <= 0xFFFFU ? 2U : 4U;
}

} // namespace

ScopeTables ScopeTables::pack(const ScopeCounts& counts, const ScopeRoute route) {
    ScopeTables tables;
    if (counts.words.size() != counts.layout.wordCount ||
        counts.words.size() < render::kScopeCounterWords) {
        return tables;
    }
    const ScopeTableLayout& layout = counts.layout;
    tables.layout_ = layout;
    tables.route_ = route;
    for (std::uint32_t i = 0; i < render::kScopeCounterWords; ++i) {
        tables.counters_[i] = counts.words[i];
    }
    const auto packTable = [&counts](Table& table, const std::uint32_t offset,
                                     const std::size_t size) {
        std::uint32_t maximum = 0;
        for (std::size_t i = 0; i < size; ++i) {
            maximum = std::max(maximum, counts.words[offset + i]);
        }
        table.maximum = maximum;
        table.widthBytes = widthFor(maximum);
        table.bytes.resize(size * table.widthBytes);
        std::size_t at = 0;
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint32_t value = counts.words[offset + i];
            for (std::uint32_t byte = 0; byte < table.widthBytes; ++byte) {
                table.bytes[at++] = static_cast<std::uint8_t>((value >> (8U * byte)) & 0xFFU);
            }
        }
    };
    if (layout.has(render::kScopeTableHistogram)) {
        packTable(tables.histogram_, layout.histogramOffset,
                  static_cast<std::size_t>(render::kScopeChannels) *
                      render::kScopeHistogramChannelWords);
    }
    if (layout.has(render::kScopeTableWaveform)) {
        packTable(tables.waveform_, layout.waveformOffset,
                  static_cast<std::size_t>(render::kScopeChannels) * layout.waveformChannelWords());
    }
    if (layout.has(render::kScopeTableVectorscope)) {
        packTable(tables.vectorscope_, layout.vectorscopeOffset,
                  static_cast<std::size_t>(render::kScopeCells) * render::kScopeCells);
    }
    if (layout.has(render::kScopeTableChromaticity)) {
        packTable(tables.chromaticity_, layout.chromaticityOffset,
                  static_cast<std::size_t>(render::kScopeCells) * render::kScopeCells);
    }
    return tables;
}

std::size_t ScopeTables::byteSize() const noexcept {
    return sizeof(ScopeTables) + histogram_.bytes.size() + waveform_.bytes.size() +
           vectorscope_.bytes.size() + chromaticity_.bytes.size();
}

std::uint32_t ScopeTables::tableWidthBytes(const std::uint32_t scope) const noexcept {
    switch (scope) {
    case render::kScopeTableHistogram:
        return histogram_.widthBytes;
    case render::kScopeTableWaveform:
        return waveform_.widthBytes;
    case render::kScopeTableVectorscope:
        return vectorscope_.widthBytes;
    case render::kScopeTableChromaticity:
        return chromaticity_.widthBytes;
    default:
        return 0U;
    }
}

std::uint32_t ScopeTables::tableMaximum(const std::uint32_t scope) const noexcept {
    switch (scope) {
    case render::kScopeTableHistogram:
        return histogram_.maximum;
    case render::kScopeTableWaveform:
        return waveform_.maximum;
    case render::kScopeTableVectorscope:
        return vectorscope_.maximum;
    case render::kScopeTableChromaticity:
        return chromaticity_.maximum;
    default:
        return 0U;
    }
}

std::uint32_t ScopeTables::read(const Table& table, const std::size_t index) const noexcept {
    const std::size_t at = index * table.widthBytes;
    if (table.widthBytes == 0U || at + table.widthBytes > table.bytes.size()) {
        return 0U;
    }
    std::uint32_t value = 0;
    for (std::uint32_t byte = 0; byte < table.widthBytes; ++byte) {
        value |= static_cast<std::uint32_t>(table.bytes[at + byte]) << (8U * byte);
    }
    return value;
}

ScopeCounts ScopeTables::unpack() const {
    ScopeCounts counts;
    if (empty()) {
        return counts;
    }
    counts.layout = layout_;
    counts.words.assign(layout_.wordCount, 0U);
    for (std::uint32_t i = 0; i < render::kScopeCounterWords; ++i) {
        counts.words[i] = counters_[i];
    }
    const auto unpackTable = [this, &counts](const Table& table, const std::uint32_t offset) {
        const std::size_t size =
            table.widthBytes == 0U ? 0U : table.bytes.size() / table.widthBytes;
        for (std::size_t i = 0; i < size; ++i) {
            counts.words[offset + i] = read(table, i);
        }
    };
    if (layout_.has(render::kScopeTableHistogram)) {
        unpackTable(histogram_, layout_.histogramOffset);
    }
    if (layout_.has(render::kScopeTableWaveform)) {
        unpackTable(waveform_, layout_.waveformOffset);
    }
    if (layout_.has(render::kScopeTableVectorscope)) {
        unpackTable(vectorscope_, layout_.vectorscopeOffset);
    }
    if (layout_.has(render::kScopeTableChromaticity)) {
        unpackTable(chromaticity_, layout_.chromaticityOffset);
    }
    return counts;
}

std::uint32_t ScopeTables::counted() const noexcept {
    return counters_[render::kScopeCounterCounted];
}
std::uint32_t ScopeTables::notFinite() const noexcept {
    return counters_[render::kScopeCounterNotFinite];
}
std::uint32_t ScopeTables::noChromaticity() const noexcept {
    return counters_[render::kScopeCounterNoChromaticity];
}
std::uint32_t ScopeTables::chromaticityOutside() const noexcept {
    return counters_[render::kScopeCounterChromaticityOutside];
}
std::uint32_t ScopeTables::vectorscopeOutside() const noexcept {
    return counters_[render::kScopeCounterVectorscopeOutside];
}
std::uint32_t ScopeTables::histogram(const std::uint32_t channel,
                                     const std::uint32_t index) const noexcept {
    const std::size_t at = histogramIndex(layout_, channel, index);
    return at == kNone ? 0U : read(histogram_, at - layout_.histogramOffset);
}
std::uint32_t ScopeTables::waveform(const std::uint32_t channel, const std::uint32_t column,
                                    const std::uint32_t level) const noexcept {
    const std::size_t at = waveformIndex(layout_, channel, column, level);
    return at == kNone ? 0U : read(waveform_, at - layout_.waveformOffset);
}
std::uint32_t ScopeTables::vectorscope(const std::uint32_t cbCell,
                                       const std::uint32_t crCell) const noexcept {
    const std::size_t at = gridIndex(layout_, render::kScopeTableVectorscope,
                                     layout_.vectorscopeOffset, cbCell, crCell);
    return at == kNone ? 0U : read(vectorscope_, at - layout_.vectorscopeOffset);
}
std::uint32_t ScopeTables::chromaticity(const std::uint32_t xCell,
                                        const std::uint32_t yCell) const noexcept {
    const std::size_t at = gridIndex(layout_, render::kScopeTableChromaticity,
                                     layout_.chromaticityOffset, xCell, yCell);
    return at == kNone ? 0U : read(chromaticity_, at - layout_.chromaticityOffset);
}

// ---------------------------------------------------------------------------------------------
// Summary

namespace {

[[nodiscard]] ScopeChannelSummary summarizeChannel(const ScopeCounts& counts,
                                                   const std::uint32_t channel) {
    ScopeChannelSummary summary;
    if (!counts.layout.has(render::kScopeTableHistogram)) {
        return summary;
    }
    const std::uint64_t under = counts.histogram(channel, render::kScopeCodeUnder);
    const std::uint64_t over = counts.histogram(channel, render::kScopeCodeOver);
    std::uint64_t inBins = 0;
    long double weighted = 0.0L;
    bool any = false;
    for (std::uint32_t code = 0; code < render::kScopeCodes; ++code) {
        const std::uint64_t count = counts.histogram(channel, code);
        if (count == 0U) {
            continue;
        }
        if (!any) {
            summary.minimum = code;
            any = true;
        }
        summary.maximum = code;
        inBins += count;
        weighted += static_cast<long double>(count) * static_cast<long double>(code);
    }
    const std::uint64_t total = inBins + under + over;
    if (total == 0U) {
        return summary;
    }
    summary.available = true;
    summary.clippedUnder = static_cast<std::uint32_t>(under);
    summary.clippedOver = static_cast<std::uint32_t>(over);
    summary.mean =
        inBins == 0U ? 0.0 : static_cast<double>(weighted / static_cast<long double>(inBins));

    const auto percentile = [&](const std::uint64_t percent) {
        const std::uint64_t target = std::max<std::uint64_t>(1U, (percent * total + 99U) / 100U);
        std::uint64_t cumulative = under;
        if (cumulative >= target) {
            return 0U;
        }
        for (std::uint32_t code = 0; code < render::kScopeCodes; ++code) {
            cumulative += counts.histogram(channel, code);
            if (cumulative >= target) {
                return code;
            }
        }
        return render::kScopeCodes - 1U;
    };
    summary.percentile1 = percentile(1U);
    summary.percentile50 = percentile(50U);
    summary.percentile99 = percentile(99U);
    return summary;
}

} // namespace

ScopeSummary summarizeScopes(const ScopeCounts& counts) {
    ScopeSummary summary;
    for (std::uint32_t channel = 0; channel < render::kScopeChannels; ++channel) {
        summary.channels[channel] = summarizeChannel(counts, channel);
    }
    if (!counts.layout.has(render::kScopeTableVectorscope)) {
        return summary;
    }
    double total = 0.0;
    double sumCb = 0.0;
    double sumCr = 0.0;
    double sumDistance = 0.0;
    for (std::uint32_t row = 0; row < render::kScopeCells; ++row) {
        const double cr = (static_cast<double>(row) + 0.5) / 512.0 - 0.5;
        for (std::uint32_t column = 0; column < render::kScopeCells; ++column) {
            const std::uint32_t count = counts.vectorscope(column, row);
            if (count == 0U) {
                continue;
            }
            const double cb = (static_cast<double>(column) + 0.5) / 512.0 - 0.5;
            const double weight = static_cast<double>(count);
            total += weight;
            sumCb += weight * cb;
            sumCr += weight * cr;
            sumDistance += weight * std::sqrt(cb * cb + cr * cr);
        }
    }
    if (total <= 0.0) {
        return summary;
    }
    summary.vectorscopeAvailable = true;
    summary.meanSaturation = (sumDistance / total) / 0.5;
    const double meanCb = sumCb / total;
    const double meanCr = sumCr / total;
    if (std::hypot(meanCb, meanCr) > 1e-12) {
        constexpr double kPi = 3.14159265358979323846;
        double degrees = std::atan2(meanCr, meanCb) * 180.0 / kPi;
        if (degrees < 0.0) {
            degrees += 360.0;
        }
        if (degrees >= 360.0) {
            degrees = 0.0;
        }
        summary.meanHueDegrees = degrees;
    }
    return summary;
}

std::optional<ScopeMeasuredArea> scopeMeasuredArea(const render::ImageWindow& dataWindow,
                                                   const render::ImageWindow& displayWindow,
                                                   const std::optional<render::ImageWindow>& roi,
                                                   const std::optional<ScopeRegion>& region,
                                                   const std::uint32_t compositionWidth,
                                                   const std::uint32_t compositionHeight) noexcept {
    std::int64_t left = displayWindow.originX();
    std::int64_t top = displayWindow.originY();
    std::int64_t right = displayWindow.maxXExclusive();
    std::int64_t bottom = displayWindow.maxYExclusive();
    if (roi) {
        left = std::max(left, roi->originX());
        top = std::max(top, roi->originY());
        right = std::min(right, roi->maxXExclusive());
        bottom = std::min(bottom, roi->maxYExclusive());
    }
    if (region) {
        if (compositionWidth == 0 || compositionHeight == 0)
            return std::nullopt;
        const auto floorDivide = [](const std::int64_t value, const std::int64_t by) {
            const auto quotient = value / by;
            return (value % by != 0 && value < 0) ? quotient - 1 : quotient;
        };
        const auto ceilDivide = [&floorDivide](const std::int64_t value, const std::int64_t by) {
            return -floorDivide(-value, by);
        };
        const auto width = static_cast<std::int64_t>(displayWindow.extent().width());
        const auto height = static_cast<std::int64_t>(displayWindow.extent().height());
        const std::int64_t regionRight = std::int64_t{region->x} + region->width;
        const std::int64_t regionBottom = std::int64_t{region->y} + region->height;
        left = std::max(left, displayWindow.originX() +
                                  floorDivide(std::int64_t{region->x} * width, compositionWidth));
        top = std::max(top, displayWindow.originY() +
                                floorDivide(std::int64_t{region->y} * height, compositionHeight));
        right = std::min(right, displayWindow.originX() +
                                    ceilDivide(regionRight * width, compositionWidth));
        bottom = std::min(bottom, displayWindow.originY() +
                                      ceilDivide(regionBottom * height, compositionHeight));
    }
    if (right <= left || bottom <= top)
        return std::nullopt;
    const auto x = left - dataWindow.originX();
    const auto y = top - dataWindow.originY();
    constexpr std::int64_t low = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t high = std::numeric_limits<std::int32_t>::max();
    if (x < low || x > high || y < low || y > high || right - left > high || bottom - top > high)
        return std::nullopt;
    return ScopeMeasuredArea{.x = static_cast<std::int32_t>(x),
                             .y = static_cast<std::int32_t>(y),
                             .width = static_cast<std::uint32_t>(right - left),
                             .height = static_cast<std::uint32_t>(bottom - top)};
}

} // namespace bloom::runtime

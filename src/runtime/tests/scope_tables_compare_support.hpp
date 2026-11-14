#pragma once

// The comparators of the scope device tests (#724): the device's tables against the CPU
// reference's, cell for cell, or within one cell along every axis where OCIO's CPU and GPU
// processors may round a pixel into the neighboring cell. Every pixel is counted in both.

#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bloom::runtime::scope_test {

using bloom::render::kScopeChannels;
using bloom::render::kScopeCodeOver;
using bloom::render::kScopeCodeUnder;
using bloom::render::kScopeTableChromaticity;
using bloom::render::kScopeTableHistogram;
using bloom::render::kScopeTableVectorscope;
using bloom::render::kScopeTableWaveform;

// True when no count of `left` is more than one cell from where `right` has it, along one axis:
// each running total of one lies between the other's running totals one cell before and at it.
[[nodiscard]] inline bool withinOneCell(const std::vector<std::uint64_t>& left,
                                 const std::vector<std::uint64_t>& right) {
    if (left.size() != right.size())
        return false;
    std::uint64_t leftTotal = 0;
    std::uint64_t rightTotal = 0;
    std::uint64_t leftBefore = 0;
    std::uint64_t rightBefore = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        leftTotal += left[index];
        rightTotal += right[index];
        if (leftBefore > rightTotal || rightBefore > leftTotal)
            return false;
        leftBefore = leftTotal;
        rightBefore = rightTotal;
    }
    return leftTotal == rightTotal;
}

// One channel's Histogram along the code axis: under, 0..1023, over.
[[nodiscard]] inline std::vector<std::uint64_t> histogramAxis(const ScopeCounts& counts,
                                                       const std::uint32_t channel) {
    std::vector<std::uint64_t> axis;
    axis.push_back(counts.histogram(channel, kScopeCodeUnder));
    for (std::uint32_t code = 0; code < 1024; ++code)
        axis.push_back(counts.histogram(channel, code));
    axis.push_back(counts.histogram(channel, kScopeCodeOver));
    return axis;
}

// One Waveform column along the level axis of the counts' detail: under, the levels, over.
[[nodiscard]] inline std::vector<std::uint64_t>
waveformAxis(const ScopeCounts& counts, const std::uint32_t channel, const std::uint32_t column) {
    std::vector<std::uint64_t> axis;
    axis.push_back(counts.waveform(channel, column, counts.layout.waveformLevelUnder()));
    for (std::uint32_t level = 0; level < counts.layout.waveformLevels(); ++level)
        axis.push_back(counts.waveform(channel, column, level));
    axis.push_back(counts.waveform(channel, column, counts.layout.waveformLevelOver()));
    return axis;
}

// A table of `cells` x `cells` (the detail's side) summed onto one of its axes, with the pixels
// outside it as a last cell.
template <typename Cell>
[[nodiscard]] inline std::vector<std::uint64_t> planeAxis(const Cell& cell, const std::uint32_t cells,
                                                   const bool alongFirst,
                                                   const std::uint32_t outside) {
    std::vector<std::uint64_t> axis(static_cast<std::size_t>(cells) + 1U, 0);
    for (std::uint32_t first = 0; first < cells; ++first) {
        for (std::uint32_t second = 0; second < cells; ++second)
            axis[alongFirst ? first : second] += cell(first, second);
    }
    axis[cells] = outside;
    return axis;
}

// The pixels outside a 512 x 512 table are one count for both axes, so a pixel that crosses the
// table's edge is not placed along an axis: the axes are compared only when both sides agree on
// how many are outside.
[[nodiscard]] inline bool planesWithinOneCell(const ScopeCounts& device, const ScopeCounts& reference,
                                       const bool chromaticity) {
    const auto cellOf = [chromaticity](const ScopeCounts& counts) {
        return [&counts, chromaticity](const std::uint32_t first, const std::uint32_t second) {
            return chromaticity ? counts.chromaticity(first, second)
                                : counts.vectorscope(first, second);
        };
    };
    const auto outsideOf = [chromaticity](const ScopeCounts& counts) {
        return chromaticity ? counts.chromaticityOutside() + counts.noChromaticity()
                            : counts.vectorscopeOutside();
    };
    for (const bool alongFirst : {true, false}) {
        const std::uint32_t cells = device.layout.cells();
        auto left = planeAxis(cellOf(device), cells, alongFirst, outsideOf(device));
        auto right = planeAxis(cellOf(reference), cells, alongFirst, outsideOf(reference));
        if (left[cells] != right[cells]) {
            // Some pixels sit on the table's edge: only the totals can be compared.
            std::uint64_t leftTotal = 0;
            std::uint64_t rightTotal = 0;
            for (std::size_t index = 0; index < left.size(); ++index) {
                leftTotal += left[index];
                rightTotal += right[index];
            }
            if (leftTotal != rightTotal)
                return false;
            continue;
        }
        if (!withinOneCell(left, right))
            return false;
    }
    return true;
}

[[nodiscard]] inline bool tablesWithinOneCell(const ScopeCounts& device, const ScopeCounts& reference) {
    if (device.layout != reference.layout || device.counted() != reference.counted() ||
        device.notFinite() != reference.notFinite())
        return false;
    const auto& layout = device.layout;
    for (std::uint32_t channel = 0; channel < kScopeChannels; ++channel) {
        if (layout.has(kScopeTableHistogram) &&
            !withinOneCell(histogramAxis(device, channel), histogramAxis(reference, channel)))
            return false;
        if (layout.has(kScopeTableWaveform)) {
            for (std::uint32_t column = 0; column < layout.columns; ++column) {
                if (!withinOneCell(waveformAxis(device, channel, column),
                                   waveformAxis(reference, channel, column)))
                    return false;
            }
        }
    }
    if (layout.has(kScopeTableVectorscope) && !planesWithinOneCell(device, reference, false))
        return false;
    if (layout.has(kScopeTableChromaticity) && !planesWithinOneCell(device, reference, true))
        return false;
    return true;
}

} // namespace bloom::runtime::scope_test

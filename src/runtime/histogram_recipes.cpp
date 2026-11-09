#include "histogram_recipes.hpp"

#include <utility>

namespace bloom::runtime::detail {

OperationIndex appendHistogram(RecipeEmitter& emitter, const OperationIndex input,
                               const HistogramOperands& operands) {
    const auto kernel = [&](const HistogramRole role) {
        HistogramKernel result;
        result.role = role;
        result.channels = operands.channels;
        result.straight = operands.straight;
        result.logarithmic = operands.logarithmic;
        result.bins = operands.bins;
        result.height = operands.height;
        result.grid = kHistogramTileGrid;
        return result;
    };
    // Only the counting pass reads the range, so only its identity holds it.
    auto bin = kernel(HistogramRole::Bin);
    bin.values = operands.values;
    const auto counted = emitter.effect(input, std::move(bin));
    const auto summed = emitter.effect(counted, kernel(HistogramRole::Sum));
    return emitter.effect(summed, kernel(HistogramRole::Plot));
}

} // namespace bloom::runtime::detail

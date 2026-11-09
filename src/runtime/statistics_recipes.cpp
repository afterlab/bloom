#include "statistics_recipes.hpp"

#include <utility>

namespace bloom::runtime::detail {

OperationIndex appendStatistics(RecipeEmitter& emitter, const OperationIndex input,
                                const StatisticsOperands& operands) {
    const auto kernel = [&](const StatisticsRole role) {
        StatisticsKernel result;
        result.role = role;
        result.channel = operands.channel;
        result.straight = operands.straight;
        result.opaqueOnly = operands.opaqueOnly;
        result.grid = kStatisticsTileGrid;
        return result;
    };
    const auto reduced = emitter.effect(input, kernel(StatisticsRole::Reduce));
    return emitter.effect(reduced, kernel(StatisticsRole::Sum));
}

} // namespace bloom::runtime::detail

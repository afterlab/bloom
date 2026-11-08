#include "auto_exposure_recipes.hpp"

#include "look_back_recipes.hpp"

#include <utility>
#include <vector>

namespace bloom::runtime::detail {

std::optional<OperationIndex> appendAutoExposure(RecipeEmitter& emitter, const OperationIndex input,
                                                 const AutoExposureOperands& operands) {
    // A filter needs a newest mean and at least one older.
    if (operands.lookBackFrames < 2)
        return std::nullopt;
    const auto samples = appendLookBackSamples(emitter, input, operands.lookBackFrames);
    if (samples.empty())
        return std::nullopt;
    const auto reduce = [&](const OperationIndex image, const std::int64_t grid) {
        AutoExposureKernel kernel;
        kernel.role = AutoExposureRole::Reduce;
        kernel.grid = grid;
        return emitter.effect(image, std::move(kernel));
    };
    std::vector<OperationIndex> means;
    means.reserve(samples.size());
    for (const auto sample : samples)
        means.push_back(reduce(reduce(sample, kAutoExposureTileGrid), 1));

    AutoExposureKernel filterKernel;
    filterKernel.role = AutoExposureRole::Filter;
    filterKernel.values = operands.values;
    // The newest mean is the filter's input; the older ones follow it oldest first.
    const auto filter = emitter.effect(means.back(), std::move(filterKernel));
    emitter.setImages(filter, std::vector<OperationIndex>(means.begin(), means.end() - 1));

    AutoExposureKernel applyKernel;
    applyKernel.role = AutoExposureRole::Apply;
    const auto applied = emitter.effect(input, std::move(applyKernel));
    emitter.setImages(applied, {filter});
    return applied;
}

} // namespace bloom::runtime::detail

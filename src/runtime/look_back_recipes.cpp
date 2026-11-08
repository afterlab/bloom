#include "look_back_recipes.hpp"

namespace bloom::runtime::detail {

std::vector<OperationIndex> appendLookBackSamples(RecipeEmitter& emitter, const OperationIndex input,
                                                  const std::int64_t frames) {
    std::vector<OperationIndex> samples;
    if (frames < 1 || frames > kMaximumLookBackFrames)
        return samples;
    samples.reserve(static_cast<std::size_t>(frames));
    for (std::int64_t lag = frames - 1; lag >= 1; --lag) {
        TimeSampleKernel sample;
        sample.lookBack = true;
        sample.lagFrames = lag;
        samples.push_back(emitter.effect(input, sample));
    }
    samples.push_back(input);
    return samples;
}

} // namespace bloom::runtime::detail

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

std::optional<OperationIndex> appendTemporalDenoise(RecipeEmitter& emitter,
                                                   const OperationIndex input,
                                                   const std::int64_t frames,
                                                   const CompiledScalarParameter& strength) {
    if (frames < 2)
        return std::nullopt;
    const auto samples = appendLookBackSamples(emitter, input, frames);
    if (samples.size() < 2)
        return std::nullopt;
    const auto current = samples.back();
    const auto steps = samples.size() - 1;
    auto mean = current;
    for (std::size_t step = 0; step < steps; ++step) {
        DenoiseKernel kernel{DenoiseMode::Temporal, CompiledScalarParameter{},
                             step == 0 ? strength : emitter.reuse(strength)};
        kernel.first = step == 0;
        kernel.last = step + 1 == steps;
        mean = emitter.effect(mean, std::move(kernel));
        emitter.setImages(mean, {current, samples[steps - 1 - step]});
    }
    return mean;
}

} // namespace bloom::runtime::detail

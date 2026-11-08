#include "codec_degrade_recipes.hpp"

#include "motion_blur_recipes.hpp"

#include <bloom/document/motion_blur_parameters.hpp>

#include <utility>

namespace bloom::runtime::detail {

OperationIndex appendCodecDegrade(RecipeEmitter& emitter, const OperationIndex input,
                                  const CodecDegradeOperands& operands) {
    const auto kernelOf = [&](const CodecDegradeRole role, const std::int64_t step) {
        CodecDegradeKernel kernel;
        kernel.role = role;
        kernel.blockPixels = operands.blockPixels;
        kernel.gopFrames = operands.gopFrames;
        kernel.step = step;
        kernel.chromaBleed = operands.chromaBleed;
        return kernel;
    };
    // The source frame of the chain's step (lag 0) or the one before it (lag 1), on the time sample's
    // own clock; a step after the current frame samples the current frame's pair.
    const auto sampled = [&](const std::int64_t step, const std::int64_t lag) {
        TimeSampleKernel sample;
        sample.gopLength = operands.gopFrames;
        sample.gopStep = step;
        sample.gopLag = lag;
        return emitter.effect(input, sample);
    };
    const auto encoded = [&](const OperationIndex linear) {
        return emitter.effect(linear, kernelOf(CodecDegradeRole::Encode, 0));
    };
    const auto flowKernelOf = flowKernel(document::kFlowMethodDis, document::kMotionQualityDraft,
                                         document::kFlowBorderExtend);
    OperationIndex chain = OperationIndex::fromRaw(0);
    for (std::int64_t step = 0; step < operands.gopFrames; ++step) {
        auto kernel = kernelOf(CodecDegradeRole::Step, step);
        // An authored value belongs to one operation; every other step reads a copy of it.
        kernel.values[document::codecDegradeScalarIndex("quality")] =
            step == 0 ? operands.quality : emitter.reuse(operands.quality);
        kernel.values[document::codecDegradeScalarIndex("persistence")] =
            step == 0 ? operands.persistence : emitter.reuse(operands.persistence);
        const auto colour = sampled(step, 0);
        if (step == 0) {
            chain = emitter.effect(encoded(colour), std::move(kernel));
            continue;
        }
        const auto previous = sampled(step, 1);
        const auto flow = appendFlowSolve(emitter, colour, previous, flowKernelOf);
        const auto coded = encoded(colour);
        const auto next = emitter.effect(chain, std::move(kernel));
        emitter.setImages(next, {coded, flow});
        chain = next;
    }
    return emitter.effect(chain, kernelOf(CodecDegradeRole::Finish, 0));
}

} // namespace bloom::runtime::detail

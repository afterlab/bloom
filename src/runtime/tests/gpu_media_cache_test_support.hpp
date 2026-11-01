#pragma once

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

namespace {
// The evaluator's media context. Nothing keeps a converted upload between builds (ADR 0037): a
// warm build converts again from the compact decoded or encoded source the media caches hold.
[[nodiscard]] inline bloom::runtime::GpuSceneMediaContext
deterministicMediaContext(const bloom::runtime::CpuCompositionEvaluator& evaluator) {
    auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    // Deterministic per-build statistics: read-ahead is exercised by its own fixture.
    context.sequenceReadAhead.reset();
    return context;
}

} // namespace

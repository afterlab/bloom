#ifndef BLOOM_RUNTIME_TESTS_DENOISE_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_DENOISE_TEST_SUPPORT_HPP

// #514: plans that run Temporal denoise the way the snapshot compiler lowers it
// (look_back_recipes.hpp appendTemporalDenoise, the function the compiler lowers with) over
// synthetic media only: an HDR checker with a partial-alpha block that moves on an animated
// Transform 2D (or stands still), under Film Grain drawn anew each frame, so the CPU reference tests,
// the GPU coverage fixtures and the native parity harness exercise what an authored node evaluates.

#include "film_grain_test_support.hpp"
#include "look_back_recipes.hpp"
#include "plan_time_scopes.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace bloom::denoise_test {

using document::ParameterId;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kDenoiseWidth = 40;
inline constexpr std::uint32_t kDenoiseHeight = 24;

struct TemporalValues final {
    std::int64_t frames = 4;
    double strength = 0.25;
    double grain = 0.15;
    // The block stands still: only the grain differs from frame to frame.
    bool still = false;
};

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

// The grained picture -> Temporal denoise -> plain Merge -> Composition Output.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
temporalPlan(const TemporalValues& values = {}, const std::uint64_t idBase = 51400) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kDenoiseWidth, kDenoiseHeight),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), operations,
                                  detail::definitionValueSink(definition));
    const auto vec2 = [](const std::uint64_t id, const double x, const double y) {
        return CompiledVec2Parameter{ParameterId::fromRaw(id), Vec2d{x, y}};
    };
    // A partial-alpha HDR block over an HDR checker. Its odd size ends inside a pixel of the
    // half-size proxy case, so the window also holds a partly covered edge (#641).
    const auto block = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 1),
                      {ParameterId::fromRaw(idBase + 2), core::Color4d{1.6, 0.4, 0.2, 0.75}},
                      scalar(idBase + 3, 9.0),
                      scalar(idBase + 4, 7.0)});
    CompiledVec2Parameter translate = vec2(idBase + 5, 6.0, 8.0);
    if (!values.still) {
        // Two pixels right a frame.
        CompiledVec2Curve curve;
        curve.id = document::AnimationCurveId::fromRaw(idBase + 6);
        const auto key = [&](const std::uint64_t id, const std::int64_t frame, const double value) {
            return CompiledScalarKeyframe{document::KeyframeId::fromRaw(id),
                                          *core::RationalTime::create(frame, 24), value,
                                          CompiledKeyframeInterpolation::Linear};
        };
        curve.components[0] = {key(idBase + 7, 0, 4.0), key(idBase + 8, 10, 24.0)};
        curve.components[1] = {key(idBase + 9, 0, 8.0), key(idBase + 10, 10, 8.0)};
        definition.vec2Curves.push_back(std::move(curve));
        translate.source = Vec2CurveIndex::fromRaw(0);
    }
    const auto moved = emitter.effect(
        block, Transform2DKernel{translate, vec2(idBase + 11, 0.0, 0.0), vec2(idBase + 12, 1.0, 1.0),
                                 scalar(idBase + 13, 0.0)});
    const auto checker = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledChecker{
        NodeId::fromRaw(idBase + 14),
        {ParameterId::fromRaw(idBase + 15), core::Color4d{0.45, 0.4, 0.3, 1.0}},
        {ParameterId::fromRaw(idBase + 16), core::Color4d{0.08, 0.1, 0.2, 1.0}},
        5U,
        4U,
        kDenoiseWidth,
        kDenoiseHeight});
    const auto picture =
        emitter.merge({{moved, core::BlendMode::Normal}, {checker, core::BlendMode::Normal}});
    film_grain_test::GrainValues grain;
    grain.scalars = {{"intensity", values.grain}, {"size", 1.0}};
    grain.seed = 7;
    const auto grained = emitter.effect(picture, film_grain_test::grainKernel(grain, idBase + 40));
    const auto denoised = detail::appendTemporalDenoise(emitter, grained, values.frames,
                                                        scalar(idBase + 90, values.strength));
    if (!denoised)
        throw std::logic_error("the temporal denoise window is out of bounds");
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, *denoised}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    if (const auto failure = detail::scopeLayerTime(definition))
        throw std::logic_error("the temporal plan scopes its time samples: " + failure->summary);
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::denoise_test

#endif // BLOOM_RUNTIME_TESTS_DENOISE_TEST_SUPPORT_HPP

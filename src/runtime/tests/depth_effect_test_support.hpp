#ifndef BLOOM_RUNTIME_TESTS_DEPTH_EFFECT_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_DEPTH_EFFECT_TEST_SUPPORT_HPP

// #483: plans that run Z Defocus and Depth Fog through the PRODUCTION chains
// (depth_effect_recipes.hpp, the functions the snapshot compiler lowers with) over an HDR,
// partial-alpha gradient beauty and three depth bands, so the CPU reference tests, the GPU coverage
// fixtures and the native parity harness all exercise exactly what an authored node evaluates.

#include "depth_effect_recipes.hpp"

#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::depth_test {

using document::ParameterId;
using runtime::CompiledColorParameter;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kDepthWidth = 24;
inline constexpr std::uint32_t kDepthHeight = 20;

enum class Effect : std::uint8_t { Defocus, Fog };

struct DepthValues final {
    Effect effect = Effect::Defocus;
    // The depth bands, left to right: `nearDepth`, `focusDistance` and `farDepth` (Z, scene
    // units); `mist` writes 0, 0.375 and 1 instead.
    double nearDepth = 2.0;
    double farDepth = 30.0;
    bool mist = false;
    // Z Defocus.
    std::int64_t mode = document::kDefocusModeDepth;
    std::int64_t shape = document::kBokehDisk;
    std::int64_t quality = document::kDefocusQualityDraft;
    std::int64_t focusFrom = document::kFocusFromDistance;
    std::int64_t edgeExpand = 1;
    double focusDistance = 8.0;
    double size = 4.0;
    double maxSize = 5.0;
    double highlightGain = 0.0;
    double chromatic = 0.0;
    bool bokeh = false;
    // Depth Fog.
    std::int64_t falloff = document::kFogFalloffLinear;
    bool fillEmpty = false;
    bool fogImage = false;
    // The beauty is an HDR, partial-alpha solid instead of the gradient (exact on every device).
    bool solidInput = false;
};

struct DepthPlan final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    OperationIndex effect = OperationIndex::fromRaw(0);
};

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

[[nodiscard]] inline runtime::CompiledSolid ramp(const std::uint64_t idBase,
                                                 const core::Color4d from, const core::Color4d to,
                                                 const document::Vec2d start,
                                                 const document::Vec2d end) {
    using document::Vec2d;
    runtime::CompiledSolid gradient{document::NodeId::fromRaw(idBase),
                                    {ParameterId::fromRaw(idBase + 1), from},
                                    scalar(idBase + 2, static_cast<double>(kDepthWidth)),
                                    scalar(idBase + 3, static_cast<double>(kDepthHeight))};
    gradient.gradient = runtime::CompiledGradient{
        false,
        {ParameterId::fromRaw(idBase + 4), document::GradientValue{{{0.0, from}, {1.0, to}}}},
        {ParameterId::fromRaw(idBase + 5), start},
        {ParameterId::fromRaw(idBase + 6), end},
        ParameterId::fromRaw(idBase + 7),
        0};
    return gradient;
}

// Beauty and depth -> Z Defocus or Depth Fog -> plain Merge -> Composition Output.
[[nodiscard]] inline DepthPlan depthPlan(const DepthValues& values = {},
                                         const std::uint64_t idBase = 48300) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kDepthWidth, kDepthHeight),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    const auto beauty = OperationIndex::fromRaw(operations.size());
    if (values.solidInput)
        operations.emplace_back(
            CompiledSolid{NodeId::fromRaw(idBase + 1),
                          {ParameterId::fromRaw(idBase + 2), core::Color4d{3.0, 1.5, 0.75, 0.6}},
                          scalar(idBase + 3, static_cast<double>(kDepthWidth)),
                          scalar(idBase + 4, static_cast<double>(kDepthHeight))});
    else
        operations.emplace_back(ramp(idBase + 1, core::Color4d{0.5, 0.25, 0.1, 0.6},
                                     core::Color4d{6.0, 3.0, 1.5, 1.0}, Vec2d{2.0, 2.0},
                                     Vec2d{20.0, 17.0}));
    // Uniform reads no depth (a plan holds no unreachable operation). The depth is three opaque
    // solid bands merged left to right (near, the focus distance, far): a solid and an opaque
    // source-over are exact on every device, so a depth-derived coverage is too, and the bands'
    // edges are a foreground over an in-focus subject over a background.
    const bool readsDepth =
        values.effect == Effect::Fog || values.mode != document::kDefocusModeUniform;
    const double nearValue = values.mist ? 0.0 : values.nearDepth;
    const double farValue = values.mist ? 1.0 : values.farDepth;
    const double midValue = values.mist ? 0.375 : values.focusDistance;
    auto depth = OperationIndex::fromRaw(0);
    if (readsDepth) {
        std::vector<CompiledMergeInput> bands;
        const std::array<std::pair<double, std::uint32_t>, 3> steps{
            {{nearValue, kDepthWidth / 3}, {midValue, 2 * kDepthWidth / 3}, {farValue, kDepthWidth}}};
        std::uint64_t id = idBase + 70;
        for (const auto& [value, width] : steps) {
            bands.push_back({{}, {}, OperationIndex::fromRaw(operations.size())});
            operations.emplace_back(
                CompiledSolid{NodeId::fromRaw(id),
                              {ParameterId::fromRaw(id + 1), core::Color4d{value, value, value, 1.0}},
                              scalar(id + 2, static_cast<double>(width)),
                              scalar(id + 3, static_cast<double>(kDepthHeight))});
            id += 4;
        }
        depth = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(CompiledMerge{NodeId::fromRaw(id), std::move(bands)});
    }
    std::optional<OperationIndex> extra;
    if (values.bokeh || values.fogImage) {
        extra = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(ramp(idBase + 20, core::Color4d{1.0, 0.5, 0.25, 1.0},
                                     core::Color4d{0.25, 1.0, 2.0, 1.0}, Vec2d{0.0, 0.0},
                                     Vec2d{0.0, static_cast<double>(kDepthHeight)}));
    }
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), operations,
                                  detail::definitionValueSink(definition));
    DepthMapKernel map;
    map.interpretation =
        values.mist ? document::kDepthInterpretationMist : document::kDepthInterpretationZ;
    map.nearDistance = scalar(idBase + 31, values.mist ? values.nearDepth : 0.1);
    map.farDistance = scalar(idBase + 32, values.mist ? values.farDepth : 100.0);
    DepthPlan result;
    if (values.effect == Effect::Defocus) {
        detail::ZDefocusOperands lens;
        lens.map = map;
        lens.map.focusFrom = values.focusFrom;
        lens.map.edgeExpand = values.edgeExpand;
        lens.map.focusDistance = scalar(idBase + 33, values.focusDistance);
        lens.map.focusPoint = runtime::CompiledVec2Parameter{ParameterId::fromRaw(idBase + 34),
                                                             Vec2d{-4.0, 0.0}};
        lens.map.depthOfField = scalar(idBase + 35, 0.5);
        lens.map.focalLength = scalar(idBase + 36, 50.0);
        lens.map.sensorWidth = scalar(idBase + 37, 36.0);
        lens.map.fStop = scalar(idBase + 38, 1.4);
        lens.gather.mode = values.mode;
        lens.gather.shape = values.shape;
        lens.gather.blades = 5;
        lens.gather.quality = values.quality;
        lens.gather.size = scalar(idBase + 40, values.size);
        lens.gather.maxSize = scalar(idBase + 41, values.maxSize);
        lens.gather.rotation = scalar(idBase + 42, 15.0);
        lens.gather.roundness = scalar(idBase + 43, 0.25);
        lens.gather.highlightGain = scalar(idBase + 44, values.highlightGain);
        lens.gather.highlightThreshold = scalar(idBase + 45, 1.0);
        lens.gather.chromatic = scalar(idBase + 46, values.chromatic);
        result.effect = detail::appendZDefocus(
            emitter, beauty,
            readsDepth ? std::optional(depth) : std::nullopt,
            extra, std::move(lens));
    } else {
        detail::DepthFogOperands fog;
        fog.map = map;
        fog.map.falloff = values.falloff;
        fog.map.amount = scalar(idBase + 50, 0.8);
        fog.map.start = scalar(idBase + 51, values.mist ? values.nearDepth : 4.0);
        fog.map.end = scalar(idBase + 52, values.mist ? values.farDepth : 24.0);
        fog.map.density = scalar(idBase + 53, 0.15);
        if (values.falloff == document::kFogFalloffCurve)
            fog.map.curve = runtime::CompiledPathParameter{
                ParameterId::fromRaw(idBase + 54),
                document::gradeCurvePath({Vec2d{0.0, 0.0}, Vec2d{0.4, 0.7}, Vec2d{1.0, 1.0}})};
        fog.color = CompiledColorParameter{ParameterId::fromRaw(idBase + 55),
                                           core::Color4d{0.6, 0.7, 0.9, 1.0}};
        fog.fillEmpty = values.fillEmpty;
        result.effect = detail::appendDepthFog(emitter, beauty, depth,
                                               values.fogImage ? extra : std::nullopt,
                                               std::move(fog));
    }
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 60), {{{}, {}, result.effect}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 61), merged});
    result.plan = std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    return result;
}

} // namespace bloom::depth_test

#endif // BLOOM_RUNTIME_TESTS_DEPTH_EFFECT_TEST_SUPPORT_HPP

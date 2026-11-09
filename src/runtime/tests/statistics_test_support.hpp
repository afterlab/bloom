#ifndef BLOOM_RUNTIME_TESTS_STATISTICS_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_STATISTICS_TEST_SUPPORT_HPP

// #349: plans that run Image Statistics the way the snapshot compiler lowers it (statistics_recipes.hpp,
// the function the compiler lowers with) over synthetic media only: a picture, its reduction to two
// pixels as the plan's output (a statistics plan), and a frame whose size an Image Statistics value
// drives. The CPU reference tests, the GPU coverage fixtures and the native parity harness exercise
// what an authored node evaluates.

#include "statistics_recipes.hpp"

#include <bloom/document/image_math_parameters.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace bloom::statistics_test {

using document::NodeId;
using document::ParameterId;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kStatisticsSize = 64;

struct StatisticsValues final {
    std::int64_t channel = document::kStatisticsLuminance;
    bool straight = true;
    bool opaqueOnly = false;
    // A signed, high dynamic range, partly transparent gradient across the picture, or one color.
    bool gradient = true;
    core::Color4d color{0.5, 0.25, 0.125, 1.0};
    std::uint32_t pictureWidth = kStatisticsSize;
    std::uint32_t pictureHeight = 48;
};

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

// The picture alone: a Solid, the 64 by 64 composition's only layer.
inline void appendPicture(runtime::CompiledCompositionPlanDefinition& definition,
                          const StatisticsValues& values, const std::uint64_t idBase) {
    using namespace runtime;
    using document::Vec2d;
    runtime::CompiledSolid source{NodeId::fromRaw(idBase + 1),
                                  {ParameterId::fromRaw(idBase + 2),
                                   values.gradient ? core::Color4d{-0.25, 2.0, 0.5, 0.75}
                                                   : values.color},
                                  scalar(idBase + 3, values.pictureWidth),
                                  scalar(idBase + 4, values.pictureHeight)};
    if (values.gradient)
        source.gradient = CompiledGradient{
            false,
            {ParameterId::fromRaw(idBase + 5),
             document::GradientValue{{{0.0, core::Color4d{-0.25, 2.0, 0.5, 0.75}},
                                      {0.4, core::Color4d{3.0, 0.1, 0.0, 1.0}},
                                      {1.0, core::Color4d{1.0, -0.5, 0.25, 0.25}}}}},
            {ParameterId::fromRaw(idBase + 6), Vec2d{0.0, 0.0}},
            {ParameterId::fromRaw(idBase + 7), Vec2d{static_cast<double>(values.pictureWidth), 0.0}},
            ParameterId::fromRaw(idBase + 8), 0};
    definition.operations.emplace_back(std::move(source));
}

// The statistics plan: the picture followed by Reduce and Sum, shown through a Merge. The same
// shape SnapshotCompiler lowers an Image Statistics node's tap plan to.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
statisticsPlan(const StatisticsValues& values = {}, const std::uint64_t idBase = 34910) {
    using namespace runtime;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kStatisticsSize, kStatisticsSize),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    appendPicture(definition, values, idBase);
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), definition.operations,
                                  detail::definitionValueSink(definition));
    detail::StatisticsOperands operands;
    operands.channel = values.channel;
    operands.straight = values.straight;
    operands.opaqueOnly = values.opaqueOnly;
    const auto reduced = detail::appendStatistics(emitter, OperationIndex::fromRaw(0), operands);
    const auto merged = OperationIndex::fromRaw(definition.operations.size());
    definition.operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, reduced}}});
    definition.output = OperationIndex::fromRaw(definition.operations.size());
    definition.operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

// A frame whose Solid is 40 pixels wide and as high as the statistic `output` (0 minimum, 1 maximum,
// 2 mean, 3 deviation, 4 count) of `tap` says, in a 64 by 64 composition.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
drivenPlan(const std::shared_ptr<const runtime::CompiledCompositionPlan>& tap,
           const std::uint32_t output, const std::uint64_t idBase = 34950) {
    using namespace runtime;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kStatisticsSize, kStatisticsSize),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    definition.valueOperations.push_back(CompiledValueOperation{
        NodeId::fromRaw(idBase + 20), ValueOutputIndex::fromRaw(0), 5,
        CompiledValueImageStatistics{NodeId::fromRaw(idBase + 20), tap}});
    definition.valueOutputCount = 5;
    definition.operations.emplace_back(CompiledSolid{
        NodeId::fromRaw(idBase + 1),
        {ParameterId::fromRaw(idBase + 2), core::Color4d{1.0, 1.0, 1.0, 1.0}},
        scalar(idBase + 3, 40.0),
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 4), ValueOutputIndex::fromRaw(output)}});
    const auto merged = OperationIndex::fromRaw(definition.operations.size());
    definition.operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, OperationIndex::fromRaw(0)}}});
    definition.output = OperationIndex::fromRaw(definition.operations.size());
    definition.operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::statistics_test

#endif // BLOOM_RUNTIME_TESTS_STATISTICS_TEST_SUPPORT_HPP

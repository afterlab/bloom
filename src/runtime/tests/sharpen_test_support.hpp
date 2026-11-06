#ifndef BLOOM_RUNTIME_TESTS_SHARPEN_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_SHARPEN_TEST_SUPPORT_HPP

// #486: plans that run Sharpen through the production chain (compositing_recipes.hpp's appendSharpen,
// the function the snapshot compiler lowers with) over a picture with real edges: an HDR, partial-alpha
// gradient under a small bright solid, so the CPU reference tests, the GPU coverage fixtures and the
// native parity harness exercise what an authored node evaluates.

#include "compositing_recipes.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace bloom::sharpen_test {

using document::ParameterId;
using runtime::OperationIndex;

inline constexpr std::uint32_t kSharpenWidth = 64;
inline constexpr std::uint32_t kSharpenHeight = 48;

struct SharpenValues final {
    double amount = 1.0;
    double radius = 3.0;
    double threshold = 0.0;
};

// Beauty and a bright solid -> Sharpen -> plain Merge -> Composition Output.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
sharpenPlan(const SharpenValues& values = {}, const std::uint64_t idBase = 48700) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kSharpenWidth, kSharpenHeight),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    const auto scalar = [](const std::uint64_t id, const double value) {
        return CompiledScalarParameter{ParameterId::fromRaw(id), value};
    };
    const auto beauty = OperationIndex::fromRaw(operations.size());
    CompiledSolid gradient{NodeId::fromRaw(idBase + 1),
                           {ParameterId::fromRaw(idBase + 2), core::Color4d{0.05, 0.02, 0.01, 0.6}},
                           scalar(idBase + 3, static_cast<double>(kSharpenWidth)),
                           scalar(idBase + 4, static_cast<double>(kSharpenHeight))};
    gradient.gradient = CompiledGradient{
        false,
        {ParameterId::fromRaw(idBase + 5),
         document::GradientValue{{{0.0, core::Color4d{0.05, 0.02, 0.01, 0.6}},
                                  {1.0, core::Color4d{6.0, 3.0, 1.5, 1.0}}}}},
        {ParameterId::fromRaw(idBase + 6), Vec2d{2.0, 2.0}},
        {ParameterId::fromRaw(idBase + 7),
         Vec2d{static_cast<double>(kSharpenWidth) - 4.0, static_cast<double>(kSharpenHeight) - 3.0}},
        ParameterId::fromRaw(idBase + 8),
        0};
    operations.emplace_back(std::move(gradient));
    const auto bright = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledSolid{
        NodeId::fromRaw(idBase + 9),
        {ParameterId::fromRaw(idBase + 10), core::Color4d{18.0, 0.25, 4.0, 0.75}},
        scalar(idBase + 11, 20.0),
        scalar(idBase + 12, 16.0)});
    const auto picture = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 13), {{{}, {}, bright}, {{}, {}, beauty}}});
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 20), operations,
                                  detail::definitionValueSink(definition));
    const auto effect = detail::appendSharpen(emitter, picture,
                                              {scalar(idBase + 100, values.amount),
                                               scalar(idBase + 101, values.radius),
                                               scalar(idBase + 102, values.threshold)});
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 60), {{{}, {}, effect}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 61), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::sharpen_test

#endif // BLOOM_RUNTIME_TESTS_SHARPEN_TEST_SUPPORT_HPP

#ifndef BLOOM_RUNTIME_TESTS_HISTOGRAM_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_HISTOGRAM_TEST_SUPPORT_HPP

// #349: plans that run Histogram the way the snapshot compiler lowers it (histogram_recipes.hpp, the
// function the compiler lowers with) over synthetic media only: a high dynamic range gradient with
// negative values and partial alpha, so the CPU reference tests, the GPU coverage fixtures and the
// native parity harness exercise what an authored node evaluates.

#include "histogram_recipes.hpp"

#include <bloom/document/image_math_parameters.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace bloom::histogram_test {

using document::ParameterId;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kHistogramSize = 64;

struct HistogramValues final {
    std::int64_t channels = document::kHistogramRgbLuminance;
    bool straight = true;
    bool logarithmic = false;
    std::int64_t bins = 64;
    std::int64_t height = 64;
    double rangeMin = 0.0;
    double rangeMax = 1.0;
    // The source is this wide and high; the composition is kHistogramSize square.
    std::uint32_t sourceWidth = kHistogramSize;
    std::uint32_t sourceHeight = 48;
};

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

// The gradient -> Histogram -> plain Merge -> Composition Output, in a square composition.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
histogramPlan(const HistogramValues& values = {}, const std::uint64_t idBase = 34900) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kHistogramSize, kHistogramSize),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), operations,
                                  detail::definitionValueSink(definition));
    CompiledSolid source{NodeId::fromRaw(idBase + 1),
                         {ParameterId::fromRaw(idBase + 2), core::Color4d{-0.25, 2.0, 0.5, 0.75}},
                         scalar(idBase + 3, values.sourceWidth),
                         scalar(idBase + 4, values.sourceHeight)};
    source.gradient = CompiledGradient{
        false,
        {ParameterId::fromRaw(idBase + 5),
         document::GradientValue{{{0.0, core::Color4d{-0.25, 2.0, 0.5, 0.75}},
                                  {0.4, core::Color4d{3.0, 0.1, 0.0, 1.0}},
                                  {1.0, core::Color4d{1.0, -0.5, 0.25, 0.25}}}}},
        {ParameterId::fromRaw(idBase + 6), Vec2d{0.0, 0.0}},
        {ParameterId::fromRaw(idBase + 7), Vec2d{static_cast<double>(values.sourceWidth), 0.0}},
        ParameterId::fromRaw(idBase + 8), 0};
    operations.emplace_back(std::move(source));
    detail::HistogramOperands operands;
    operands.channels = values.channels;
    operands.straight = values.straight;
    operands.logarithmic = values.logarithmic;
    operands.bins = values.bins;
    operands.height = values.height;
    operands.values = {scalar(idBase + 10, values.rangeMin), scalar(idBase + 11, values.rangeMax)};
    const auto graph = detail::appendHistogram(emitter, OperationIndex::fromRaw(0), operands);
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, graph}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::histogram_test

#endif // BLOOM_RUNTIME_TESTS_HISTOGRAM_TEST_SUPPORT_HPP

#ifndef BLOOM_RUNTIME_TESTS_FILM_GRAIN_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_FILM_GRAIN_TEST_SUPPORT_HPP

// #486: kernels and plans that run Film Grain the way the snapshot compiler lowers it (one
// FilmGrainKernel over its input) over an HDR, partial-alpha beauty, so the CPU reference tests, the
// GPU coverage fixtures and the native parity harness exercise what an authored node evaluates.

#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::film_grain_test {

using document::ParameterId;
using runtime::OperationIndex;

inline constexpr std::uint32_t kGrainWidth = 64;
inline constexpr std::uint32_t kGrainHeight = 48;

// A value differing from the node's default; every other value keeps its default.
struct GrainValues final {
    std::map<std::string, double> scalars{};
    std::int64_t animation = document::kFilmGrainEveryFrame;
    std::int64_t seed = 1;
    bool monochrome = false;
};

[[nodiscard]] inline runtime::FilmGrainKernel grainKernel(const GrainValues& values,
                                                          const std::uint64_t idBase) {
    runtime::FilmGrainKernel kernel;
    kernel.animation = values.animation;
    kernel.seed = values.seed;
    kernel.monochrome = values.monochrome;
    std::uint64_t id = idBase;
    for (const auto& spec : document::filmGrainParameters()) {
        if (spec.kind != document::FilmGrainParameterKind::Scalar)
            continue;
        const auto found = values.scalars.find(std::string(spec.role));
        kernel.values[document::filmGrainScalarIndex(spec.role)] = {
            ParameterId::fromRaw(id++), found != values.scalars.end() ? found->second : spec.value};
    }
    return kernel;
}

// The operands in the kernel's canonical order, then the frame (sampleColorGradeOperands).
[[nodiscard]] inline std::vector<double> grainOperands(const GrainValues& values,
                                                       const double frame = 0.0) {
    std::vector<double> operands;
    for (const auto& spec : document::filmGrainParameters()) {
        if (spec.kind != document::FilmGrainParameterKind::Scalar)
            continue;
        const auto found = values.scalars.find(std::string(spec.role));
        operands.push_back(found != values.scalars.end() ? found->second : spec.value);
    }
    operands.push_back(frame);
    return operands;
}

// Strong grain with every channel and response distinct: the program's every path.
[[nodiscard]] inline GrainValues everyPath() {
    GrainValues values;
    values.scalars = {{"intensity", 0.2},   {"size", 2.5},     {"redAmount", 1.5},
                      {"greenAmount", 0.5}, {"blueAmount", 2.0}, {"shadows", 0.75},
                      {"midtones", 1.25},   {"highlights", 0.5}};
    values.seed = 42;
    return values;
}

// The same with one grain pattern shared by every channel.
[[nodiscard]] inline GrainValues everyPathMonochrome() {
    GrainValues values = everyPath();
    values.monochrome = true;
    return values;
}

// Beauty -> Film Grain -> plain Merge -> Composition Output.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
grainPlan(const GrainValues& values = {}, const std::uint64_t idBase = 48600) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kGrainWidth, kGrainHeight),
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
                           scalar(idBase + 3, static_cast<double>(kGrainWidth)),
                           scalar(idBase + 4, static_cast<double>(kGrainHeight))};
    gradient.gradient = CompiledGradient{
        false,
        {ParameterId::fromRaw(idBase + 5),
         document::GradientValue{{{0.0, core::Color4d{0.05, 0.02, 0.01, 0.6}},
                                  {1.0, core::Color4d{6.0, 3.0, 1.5, 1.0}}}}},
        {ParameterId::fromRaw(idBase + 6), Vec2d{2.0, 2.0}},
        {ParameterId::fromRaw(idBase + 7),
         Vec2d{static_cast<double>(kGrainWidth) - 4.0, static_cast<double>(kGrainHeight) - 3.0}},
        ParameterId::fromRaw(idBase + 8),
        0};
    operations.emplace_back(std::move(gradient));
    const auto effect = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(
        CompiledImageEffect{NodeId::fromRaw(idBase + 20), beauty, grainKernel(values, idBase + 100)});
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 60), {{{}, {}, effect}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 61), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::film_grain_test

#endif // BLOOM_RUNTIME_TESTS_FILM_GRAIN_TEST_SUPPORT_HPP

#ifndef BLOOM_RUNTIME_TESTS_DITHER_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_DITHER_TEST_SUPPORT_HPP

// #527: kernels and plans that run Dither the way the snapshot compiler lowers it (one DitherKernel
// over its input) over an HDR, partial-alpha beauty, so the CPU reference tests, the GPU coverage
// fixtures and the native parity harness exercise what an authored node evaluates.

#include <bloom/core/dither_pattern.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace bloom::dither_test {

using document::ParameterId;
using runtime::OperationIndex;

inline constexpr std::uint32_t kDitherWidth = 64;
inline constexpr std::uint32_t kDitherHeight = 48;

struct DitherSettings final {
    bool blueNoise = true;
    std::int64_t side = core::dither::kBlueNoiseSide;
    std::int64_t bits = 8;
    bool stored = false;
    bool everyFrame = true;
    std::int64_t seed = 1;
    bool monochrome = false;
    double amplitude = 1.0;
};

[[nodiscard]] inline DitherSettings ordered(const std::int64_t side) {
    DitherSettings settings;
    settings.blueNoise = false;
    settings.side = side;
    return settings;
}

[[nodiscard]] inline runtime::DitherKernel ditherKernel(const DitherSettings& settings,
                                                        const std::uint64_t idBase) {
    runtime::DitherKernel kernel;
    kernel.blueNoise = settings.blueNoise;
    kernel.side = settings.side;
    kernel.bits = settings.bits;
    kernel.stored = settings.stored;
    kernel.everyFrame = settings.everyFrame;
    kernel.seed = settings.seed;
    kernel.monochrome = settings.monochrome;
    kernel.amplitude = {ParameterId::fromRaw(idBase), settings.amplitude};
    return kernel;
}

// The operands in the kernel's canonical order (the amplitude), then the frame
// (sampleColorGradeOperands).
[[nodiscard]] inline std::vector<double> ditherOperands(const DitherSettings& settings,
                                                        const double frame = 0.0) {
    return {settings.amplitude, settings.everyFrame ? frame : 0.0};
}

// A strong dither (4 LSBs of 8 bit) so every path moves the picture visibly: Blue noise per channel
// in the display encoding, and its variants.
[[nodiscard]] inline DitherSettings everyPath() {
    DitherSettings settings;
    settings.amplitude = 4.0;
    settings.seed = 42;
    return settings;
}

// Beauty -> Dither -> plain Merge -> Composition Output. The beauty is an HDR gradient whose alpha
// runs from 0.6 to 1, so the premultiplication, values above 1 and both transfer segments are read.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
ditherPlan(const DitherSettings& settings = {}, const std::uint64_t idBase = 52700,
           const std::uint32_t width = kDitherWidth, const std::uint32_t height = kDitherHeight) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(width, height),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    const auto scalar = [](const std::uint64_t id, const double value) {
        return CompiledScalarParameter{ParameterId::fromRaw(id), value};
    };
    const auto beauty = OperationIndex::fromRaw(operations.size());
    CompiledSolid gradient{NodeId::fromRaw(idBase + 1),
                           {ParameterId::fromRaw(idBase + 2), core::Color4d{0.001, 0.002, 0.0, 0.6}},
                           scalar(idBase + 3, static_cast<double>(width)),
                           scalar(idBase + 4, static_cast<double>(height))};
    gradient.gradient = CompiledGradient{
        false,
        {ParameterId::fromRaw(idBase + 5),
         document::GradientValue{{{0.0, core::Color4d{0.001, 0.002, 0.0, 0.6}},
                                  {1.0, core::Color4d{3.0, 0.5, 0.05, 1.0}}}}},
        {ParameterId::fromRaw(idBase + 6), Vec2d{2.0, 2.0}},
        {ParameterId::fromRaw(idBase + 7),
         Vec2d{static_cast<double>(width) - 4.0, static_cast<double>(height) - 3.0}},
        ParameterId::fromRaw(idBase + 8),
        0};
    operations.emplace_back(std::move(gradient));
    const auto effect = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledImageEffect{NodeId::fromRaw(idBase + 20), beauty,
                                                ditherKernel(settings, idBase + 100)});
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 60), {{{}, {}, effect}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 61), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::dither_test

#endif // BLOOM_RUNTIME_TESTS_DITHER_TEST_SUPPORT_HPP

#ifndef BLOOM_RUNTIME_TESTS_AUTO_EXPOSURE_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_AUTO_EXPOSURE_TEST_SUPPORT_HPP

// #566: plans that run Auto Exposure the way the snapshot compiler lowers it (auto_exposure_
// recipes.hpp, the function the compiler lowers with) over synthetic media only: a flat colour that
// changes brightness (and colour) in one frame, over a checker that gives the frame structure, so the
// CPU reference tests, the GPU coverage fixtures and the native parity harness exercise what an
// authored node evaluates. `expectedGain` is an independent double-precision reading of the
// specification for the tests to compare against.

#include "auto_exposure_recipes.hpp"
#include "plan_time_scopes.hpp"

#include <bloom/document/auto_exposure_nodes.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bloom::exposure_test {

using document::ParameterId;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

inline constexpr std::uint32_t kExposureWidth = 32;
inline constexpr std::uint32_t kExposureHeight = 24;
inline constexpr std::int64_t kExposureRate = 24;

// The values of a test plan: the node's defaults but for a look-back of a second, which a short test
// clip can hold.
struct ExposureValues final {
    double exposure = 1.0;
    double target = 0.18;
    double attack = 0.25;
    double release = 0.75;
    double minGain = -3.0;
    double maxGain = 3.0;
    double whiteBalance = 1.0;
    double balanceLag = 1.0;
    double balanceLimit = 1.0;
    std::int64_t lookBack = 24;
    // The flat colour (scene-linear) before and from the change frame.
    std::array<double, 3> before{0.18, 0.18, 0.18};
    std::array<double, 3> after{0.72, 0.72, 0.72};
    std::int64_t changeFrame = 12;
    // The footage never changes.
    bool steady = false;
};

struct ExposurePlan final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
};

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

// The footage -> Auto Exposure -> plain Merge -> Composition Output.
[[nodiscard]] inline ExposurePlan exposurePlan(const ExposureValues& values = {},
                                               const std::uint64_t idBase = 56600) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kExposureWidth, kExposureHeight),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), operations,
                                  detail::definitionValueSink(definition));
    const auto vec2 = [](const std::uint64_t id, const double x, const double y) {
        return CompiledVec2Parameter{ParameterId::fromRaw(id), Vec2d{x, y}};
    };
    // The flat colour, keyed to change across one frame, or constant.
    CompiledColorParameter colour{ParameterId::fromRaw(idBase + 2),
                                  core::Color4d{values.before[0], values.before[1], values.before[2],
                                                1.0}};
    if (!values.steady) {
        CompiledColor4Curve curve;
        curve.id = document::AnimationCurveId::fromRaw(idBase + 3);
        const auto key = [&](const std::uint64_t id, const std::int64_t frame, const double value) {
            return CompiledScalarKeyframe{document::KeyframeId::fromRaw(id),
                                          *core::RationalTime::create(frame, kExposureRate), value,
                                          CompiledKeyframeInterpolation::Linear};
        };
        for (std::size_t c = 0; c < 4; ++c) {
            const double from = c < 3 ? values.before[c] : 1.0;
            const double to = c < 3 ? values.after[c] : 1.0;
            curve.components[c] = {key(idBase + 10 + 2 * c, values.changeFrame - 1, from),
                                   key(idBase + 11 + 2 * c, values.changeFrame, to)};
        }
        definition.color4Curves.push_back(std::move(curve));
        colour.source = Color4CurveIndex::fromRaw(0);
    }
    const auto solid = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 1), colour,
                                          scalar(idBase + 4, kExposureWidth),
                                          scalar(idBase + 5, kExposureHeight)});
    const auto footage = emitter.effect(
        solid, Transform2DKernel{vec2(idBase + 6, 0.0, 0.0), vec2(idBase + 7, 0.0, 0.0),
                                 vec2(idBase + 8, 1.0, 1.0), scalar(idBase + 9, 0.0)});
    detail::AutoExposureOperands operands;
    operands.lookBackFrames = values.lookBack;
    const std::array<double, document::kAutoExposureScalarCount> scalars{
        values.exposure, values.target,   values.attack,       values.release,     values.minGain,
        values.maxGain,  values.whiteBalance, values.balanceLag, values.balanceLimit};
    for (const auto& spec : document::autoExposureParameters())
        if (spec.kind == document::AutoExposureParameterKind::Scalar) {
            const auto index = document::autoExposureScalarIndex(spec.role);
            operands.values[index] = scalar(idBase + 40 + index, scalars[index]);
        }
    const auto corrected = detail::appendAutoExposure(emitter, footage, operands);
    if (!corrected)
        throw std::logic_error("the exposure plan lowers its look-back window");
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, *corrected}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    if (const auto failure = detail::scopeLayerTime(definition))
        throw std::logic_error("the exposure plan scopes its time samples: " + failure->summary);
    return {std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition))};
}

// The gains in stops (exposure, red, blue) the specification asks for at `frame`, in double
// precision: the recursive filter over the means of the frames `max(frame - lag, 0)`, oldest first.
[[nodiscard]] inline std::array<double, 3> expectedGain(const ExposureValues& values,
                                                        const std::int64_t frame) {
    const auto mean = [&](const std::int64_t at) {
        return !values.steady && at >= values.changeFrame ? values.after : values.before;
    };
    const double dt = 1.0 / static_cast<double>(kExposureRate);
    const auto coefficient = [dt](const double seconds) {
        return seconds > 0.0 ? 1.0 - std::exp(-dt / seconds) : 1.0;
    };
    std::array<double, 3> state{};
    bool have = false;
    for (std::int64_t lag = values.lookBack - 1; lag >= 0; --lag) {
        const auto m = mean(std::max<std::int64_t>(frame - lag, 0));
        const double r = std::max(m[0], 1e-6);
        const double g = std::max(m[1], 1e-6);
        const double b = std::max(m[2], 1e-6);
        const double wr = std::clamp(std::log2(g / r), -values.balanceLimit, values.balanceLimit) *
                          values.whiteBalance;
        const double wb = std::clamp(std::log2(g / b), -values.balanceLimit, values.balanceLimit) *
                          values.whiteBalance;
        const double luma = 0.2126 * r * std::exp2(wr) + 0.7152 * g + 0.0722 * b * std::exp2(wb);
        const double e = std::clamp(std::log2(values.target) - std::log2(std::max(luma, 1e-6)),
                                    values.minGain, values.maxGain) *
                         values.exposure;
        const std::array<double, 3> desired{e, wr, wb};
        if (!have) {
            state = desired;
            have = true;
            continue;
        }
        const double ke = coefficient(desired[0] < state[0] ? values.attack : values.release);
        const double kb = coefficient(values.balanceLag);
        state[0] += ke * (desired[0] - state[0]);
        state[1] += kb * (desired[1] - state[1]);
        state[2] += kb * (desired[2] - state[2]);
    }
    return state;
}

// The colour of the footage at `frame` after the node: the flat colour times the gains.
[[nodiscard]] inline std::array<double, 3> expectedColour(const ExposureValues& values,
                                                          const std::int64_t frame) {
    const auto gain = expectedGain(values, frame);
    const auto m = !values.steady && frame >= values.changeFrame ? values.after : values.before;
    return {m[0] * std::exp2(gain[0] + gain[1]), m[1] * std::exp2(gain[0]),
            m[2] * std::exp2(gain[0] + gain[2])};
}

} // namespace bloom::exposure_test

#endif // BLOOM_RUNTIME_TESTS_AUTO_EXPOSURE_TEST_SUPPORT_HPP

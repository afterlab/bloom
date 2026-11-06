#ifndef BLOOM_RUNTIME_TESTS_LENS_FLARE_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_LENS_FLARE_TEST_SUPPORT_HPP

// #507: kernels and plans that run Lens Flare the way the snapshot compiler lowers it (one
// LensFlareKernel over its input, with the occlusion matte as its one image), over an HDR,
// partial-alpha beauty, so the CPU reference tests, the GPU coverage fixtures and the native parity
// harness exercise what an authored node evaluates.

#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::lens_flare_test {

using document::ParameterId;
using runtime::OperationIndex;

inline constexpr std::uint32_t kFlareWidth = 64;
inline constexpr std::uint32_t kFlareHeight = 48;

// A value differing from the node's default; every other value keeps its default.
struct FlareValues final {
    std::map<std::string, double> scalars{};
    std::map<std::string, std::int64_t> integers{};
    document::Vec2d position{-12.0, -8.0};
    core::Color4d color{1.0, 0.95, 0.85, 1.0};
    core::Color4d anamorphicColor{0.35, 0.55, 1.0, 1.0};
    // The beauty: smaller than the frame (the flare pads it to the frame), else frame-sized.
    bool smallInput = false;
    // A matte: an opaque solid covering the frame's left part up to this column.
    std::optional<std::uint32_t> matteWidth{};
};

[[nodiscard]] inline runtime::LensFlareKernel flareKernel(const FlareValues& values,
                                                          const std::uint64_t idBase) {
    runtime::LensFlareKernel kernel;
    std::uint64_t id = idBase;
    for (const auto& spec : document::lensFlareParameters()) {
        if (spec.kind == document::LensFlareParameterKind::Scalar) {
            const auto found = values.scalars.find(std::string(spec.role));
            kernel.values[document::lensFlareScalarIndex(spec.role)] = {
                ParameterId::fromRaw(id++),
                found != values.scalars.end() ? found->second : spec.value[0]};
        } else if (spec.kind == document::LensFlareParameterKind::Integer ||
                   spec.kind == document::LensFlareParameterKind::Choice) {
            const auto found = values.integers.find(std::string(spec.role));
            const auto value = found != values.integers.end()
                                   ? found->second
                                   : static_cast<std::int64_t>(spec.value[0]);
            if (spec.role == "lightMode")
                kernel.lightMode = value;
            else if (spec.role == "streakCount")
                kernel.streakCount = value;
            else if (spec.role == "ghostCount")
                kernel.ghostCount = value;
            else if (spec.role == "seed")
                kernel.seed = value;
            else if (spec.role == "irisCount")
                kernel.irisCount = value;
            else if (spec.role == "irisBlades")
                kernel.irisBlades = value;
            else if (spec.role == "occlusionChannel")
                kernel.occlusionChannel = value;
        }
    }
    kernel.position = {ParameterId::fromRaw(id++), values.position};
    kernel.color = {ParameterId::fromRaw(id++), values.color};
    kernel.anamorphicColor = {ParameterId::fromRaw(id++), values.anamorphicColor};
    return kernel;
}

// The operands in the kernel's canonical order (forEachColorGradeOperand).
[[nodiscard]] inline std::vector<double> flareOperands(const FlareValues& values) {
    std::vector<double> operands{values.position.x,
                                 values.position.y,
                                 values.color.red,
                                 values.color.green,
                                 values.color.blue,
                                 values.color.alpha,
                                 values.anamorphicColor.red,
                                 values.anamorphicColor.green,
                                 values.anamorphicColor.blue,
                                 values.anamorphicColor.alpha};
    for (const auto& spec : document::lensFlareParameters()) {
        if (spec.kind != document::LensFlareParameterKind::Scalar)
            continue;
        const auto found = values.scalars.find(std::string(spec.role));
        operands.push_back(found != values.scalars.end() ? found->second : spec.value[0]);
    }
    return operands;
}

// Every element on, with a moved light, chromatic split, iris and hoop: the program's every path.
[[nodiscard]] inline FlareValues everyElement() {
    FlareValues values;
    values.scalars = {{"anamorphicAmount", 0.5}, {"flareRotation", 20.0},  {"chromatic", 0.6},
                      {"ghostSize", 12.0},       {"irisSize", 14.0},  {"ringSize", 40.0},
                      {"ringWidth", 8.0},        {"hoopSize", 60.0},  {"hoopWidth", 10.0},
                      {"streakWidth", 2.0},      {"anamorphicWidth", 2.0}};
    values.integers = {{"ghostCount", 16}, {"irisCount", 12}, {"irisBlades", 7}, {"streakCount", 8}};
    return values;
}

// Beauty (and matte) -> Lens Flare -> plain Merge -> Composition Output.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
flarePlan(const FlareValues& values = {}, const std::uint64_t idBase = 50700) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kFlareWidth, kFlareHeight),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    const auto scalar = [](const std::uint64_t id, const double value) {
        return CompiledScalarParameter{ParameterId::fromRaw(id), value};
    };
    const auto beauty = OperationIndex::fromRaw(operations.size());
    const auto width = values.smallInput ? kFlareWidth / 2 : kFlareWidth;
    const auto height = values.smallInput ? kFlareHeight / 2 : kFlareHeight;
    CompiledSolid gradient{NodeId::fromRaw(idBase + 1),
                           {ParameterId::fromRaw(idBase + 2), core::Color4d{0.5, 0.25, 0.1, 0.6}},
                           scalar(idBase + 3, static_cast<double>(width)),
                           scalar(idBase + 4, static_cast<double>(height))};
    gradient.gradient = CompiledGradient{
        false,
        {ParameterId::fromRaw(idBase + 5),
         document::GradientValue{{{0.0, core::Color4d{0.5, 0.25, 0.1, 0.6}},
                                  {1.0, core::Color4d{6.0, 3.0, 1.5, 1.0}}}}},
        {ParameterId::fromRaw(idBase + 6), Vec2d{2.0, 2.0}},
        {ParameterId::fromRaw(idBase + 7),
         Vec2d{static_cast<double>(width) - 4.0, static_cast<double>(height) - 3.0}},
        ParameterId::fromRaw(idBase + 8),
        0};
    operations.emplace_back(std::move(gradient));
    std::vector<OperationIndex> images;
    if (values.matteWidth) {
        images.push_back(OperationIndex::fromRaw(operations.size()));
        operations.emplace_back(
            CompiledSolid{NodeId::fromRaw(idBase + 10),
                          {ParameterId::fromRaw(idBase + 11), core::Color4d{1.0, 1.0, 1.0, 1.0}},
                          scalar(idBase + 12, static_cast<double>(*values.matteWidth)),
                          scalar(idBase + 13, static_cast<double>(kFlareHeight))});
    }
    const auto effect = OperationIndex::fromRaw(operations.size());
    CompiledImageEffect flare{NodeId::fromRaw(idBase + 20), beauty, flareKernel(values, idBase + 100)};
    flare.fieldImages = std::move(images);
    operations.emplace_back(std::move(flare));
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 60), {{{}, {}, effect}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 61), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

} // namespace bloom::lens_flare_test

#endif // BLOOM_RUNTIME_TESTS_LENS_FLARE_TEST_SUPPORT_HPP

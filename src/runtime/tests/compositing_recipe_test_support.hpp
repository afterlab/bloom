#ifndef BLOOM_RUNTIME_TESTS_COMPOSITING_RECIPE_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_COMPOSITING_RECIPE_TEST_SUPPORT_HPP

// #339: plans that run Drop Shadow, Glow and Light Wrap through the PRODUCTION recipe chains
// (compositing_recipes.hpp, the functions the snapshot compiler lowers with) over an HDR,
// partial-alpha gradient, so the CPU reference tests, the GPU coverage fixtures and the native
// parity harness all exercise exactly what an authored node evaluates.

#include "compositing_recipes.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bloom::recipe_test {

using document::ParameterId;
using runtime::CompiledColorParameter;
using runtime::CompiledScalarParameter;
using runtime::OperationIndex;

enum class Recipe : std::uint8_t { DropShadow, Glow, LightWrap };

// Which image the plan's Composition Output publishes: the recipe, or one of the images it reads
// (an evaluation request may only name the terminal output).
enum class Published : std::uint8_t { Recipe, Input, Background, Mask };

struct RecipeValues final {
    // Drop Shadow
    core::Color4d shadowColor{0.05, 0.1, 0.4, 0.8};
    double opacity = 0.75;
    double direction = 180.0; // straight down: a whole-pixel offset (0, distance)
    double distance = 3.0;
    double softness = 2.0;
    bool shadowOnly = false;
    // Glow
    double threshold = 1.5;
    double knee = 1.0;
    double intensity = 2.0;
    double size = 4.0;
    double falloff = 0.6;
    core::Color4d tint{1.0, 0.8, 0.5, 1.0};
    std::int64_t octaves = 3;
    bool glowOnly = false;
    bool key = false;
    bool mask = false;
    // Light Wrap
    double radius = 3.0;
    double amount = 0.75;
    bool wrapOnly = false;
    // An animated Drop Shadow distance (0 at t = 0 to `distance` at t = 1) or Glow intensity.
    bool animated = false;
    // The input is an HDR, partial-alpha solid instead of the gradient. Luma Key's coverage is a
    // function of colour, and the generated gradient's colour is within 2e-6, not bit-exact, on a
    // device, so the device's exact-alpha gate keys a solid, as the Luma Key native cases do.
    bool solidInput = false;
};

struct RecipePlan final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    OperationIndex input = OperationIndex::fromRaw(0); // the gradient the recipe reads
    OperationIndex background =
        OperationIndex::fromRaw(0);                     // the solid Glow keys and Light Wrap wraps
    OperationIndex mask = OperationIndex::fromRaw(0);   // the Mask transport Glow's mask reads
    OperationIndex recipe = OperationIndex::fromRaw(0); // the recipe's published image
};

inline constexpr std::uint32_t kRecipeWidth = 24;
inline constexpr std::uint32_t kRecipeHeight = 20;

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return CompiledScalarParameter{ParameterId::fromRaw(id), value};
}

// Gradient input (HDR, partial alpha) -> recipe -> plain Merge -> Composition Output.
[[nodiscard]] inline RecipePlan recipePlan(const Recipe recipe, const RecipeValues& values = {},
                                           const std::uint64_t idBase = 3900,
                                           const Published published = Published::Recipe) {
    using namespace runtime;
    using document::NodeId;
    using document::Vec2d;
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(1),
        document::ProjectId::fromRaw(1),
        document::CompositionId::fromRaw(idBase),
        *document::CompositionFormat::create(kRecipeWidth, kRecipeHeight),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    RecipePlan result;
    CompiledSolid gradient{NodeId::fromRaw(idBase + 1),
                           {ParameterId::fromRaw(idBase + 2), core::Color4d{0.5, 0.25, 0.1, 0.6}},
                           scalar(idBase + 3, 12.0),
                           scalar(idBase + 4, 10.0)};
    gradient.gradient =
        CompiledGradient{false,
                         {ParameterId::fromRaw(idBase + 5), core::Color4d{6.0, 3.0, 1.5, 1.0}},
                         {ParameterId::fromRaw(idBase + 6), Vec2d{1.0, 1.0}},
                         {ParameterId::fromRaw(idBase + 7), Vec2d{11.0, 9.0}},
                         ParameterId::fromRaw(idBase + 8),
                         0};
    // Only the images the published one reads are emitted: a plan has no unreachable operation.
    const bool recipePublished = published == Published::Recipe;
    const bool needsInput = published != Published::Background;
    const bool needsBackground = published == Published::Background ||
                                 (recipePublished && (recipe == Recipe::LightWrap ||
                                                      (recipe == Recipe::Glow && values.key)));
    const bool needsMask =
        published == Published::Mask || (recipePublished && recipe == Recipe::Glow && values.mask);
    if (needsInput) {
        result.input = OperationIndex::fromRaw(operations.size());
        if (values.solidInput)
            operations.emplace_back(CompiledSolid{
                NodeId::fromRaw(idBase + 1),
                {ParameterId::fromRaw(idBase + 2), core::Color4d{3.0, 1.5, 0.75, 0.6}},
                scalar(idBase + 3, 12.0),
                scalar(idBase + 4, 10.0)});
        else
            operations.emplace_back(std::move(gradient));
    }
    if (needsBackground) {
        result.background = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(
            CompiledSolid{NodeId::fromRaw(idBase + 9),
                          {ParameterId::fromRaw(idBase + 10), core::Color4d{0.1, 0.6, 2.0, 1.0}},
                          scalar(idBase + 11, static_cast<double>(kRecipeWidth)),
                          scalar(idBase + 12, static_cast<double>(kRecipeHeight))});
    }
    // A Mask transport: the input's coverage through a Matte Blur (premultiplied white).
    if (needsMask) {
        result.mask = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(CompiledImageEffect{
            NodeId::fromRaw(idBase + 13), result.input,
            SeparableBlurKernel{SeparableBlurKind::Gaussian, scalar(idBase + 14, 1.5),
                                BlurEdgeMode::Transparent, BlurChannels::Coverage}});
    }
    const auto animatedScalar = [&](const std::uint64_t id, const double value) {
        if (!values.animated)
            return scalar(id, value);
        const auto curve = ScalarCurveIndex::fromRaw(definition.scalarCurves.size());
        definition.scalarCurves.push_back(CompiledScalarCurve{
            document::AnimationCurveId::fromRaw(id),
            {{document::KeyframeId::fromRaw(id), core::RationalTime::fromInteger(0), 0.0},
             {document::KeyframeId::fromRaw(id + 1), core::RationalTime::fromInteger(1), value}}});
        return CompiledScalarParameter{ParameterId::fromRaw(id), curve};
    };
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 20), operations,
                                  detail::definitionValueSink(definition));
    if (recipePublished) {
        switch (recipe) {
        case Recipe::DropShadow:
            result.recipe = detail::appendDropShadow(
                emitter, result.input,
                {CompiledColorParameter{ParameterId::fromRaw(idBase + 21), values.shadowColor},
                 scalar(idBase + 22, values.opacity), scalar(idBase + 23, values.direction),
                 animatedScalar(idBase + 24, values.distance), scalar(idBase + 26, values.softness),
                 values.shadowOnly});
            break;
        case Recipe::Glow:
            result.recipe = detail::appendGlow(
                emitter, result.input, values.key ? std::optional(result.background) : std::nullopt,
                values.mask ? std::optional(result.mask) : std::nullopt,
                {scalar(idBase + 31, values.threshold), scalar(idBase + 32, values.knee),
                 animatedScalar(idBase + 33, values.intensity), scalar(idBase + 35, values.size),
                 scalar(idBase + 36, values.falloff),
                 CompiledColorParameter{ParameterId::fromRaw(idBase + 37), values.tint},
                 values.octaves, values.glowOnly});
            break;
        case Recipe::LightWrap:
            result.recipe =
                detail::appendLightWrap(emitter, result.input, result.background,
                                        {scalar(idBase + 41, values.radius),
                                         scalar(idBase + 42, values.amount), values.wrapOnly});
            break;
        }
    }
    const auto merged = OperationIndex::fromRaw(operations.size());
    const auto publishedOperation = published == Published::Input        ? result.input
                                    : published == Published::Background ? result.background
                                    : published == Published::Mask       ? result.mask
                                                                         : result.recipe;
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 50), {{{}, {}, publishedOperation}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 51), merged});
    result.plan = std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    return result;
}

} // namespace bloom::recipe_test

#endif // BLOOM_RUNTIME_TESTS_COMPOSITING_RECIPE_TEST_SUPPORT_HPP

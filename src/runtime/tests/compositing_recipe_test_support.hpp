#ifndef BLOOM_RUNTIME_TESTS_COMPOSITING_RECIPE_TEST_SUPPORT_HPP
#define BLOOM_RUNTIME_TESTS_COMPOSITING_RECIPE_TEST_SUPPORT_HPP

// #339: plans that run Drop Shadow, Glow, Light Wrap (and #516 Edge Extend) through the PRODUCTION recipe chains
// (compositing_recipes.hpp, the functions the snapshot compiler lowers with; #506 adds the Glow
// modes and shared controls) over an HDR, partial-alpha gradient, so the CPU reference tests, the GPU coverage fixtures and the native
// parity harness all exercise exactly what an authored node evaluates.

#include "compositing_recipes.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <array>
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

enum class Recipe : std::uint8_t {
    DropShadow,
    Glow,
    LightWrap,
    EdgeExtend,
    ChannelBlur,
    Denoise,
    AlphaContour
};

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
    // #506 Glow modes and shared controls (the defaults are Classic).
    runtime::detail::GlowMode mode = runtime::detail::GlowMode::Classic;
    double exponent = 2.0;
    std::array<double, 3> channelRadius{1.0, 1.0, 1.0};
    bool gradient = false;
    core::Color4d outerTint{0.25, 0.5, 1.5, 1.0};
    // #541: a whole tint gradient in place of the tint-to-outer-tint pair, when set.
    std::optional<document::GradientValue> tintGradient;
    std::int64_t points = 4;
    double angle = 30.0;
    double length = 6.0;
    double chromatic = 0.0;
    bool dirt = false; // the Mask transport below doubles as the lens-dirt image
    double dirtAmount = 1.5;
    double rollOff = 0.0;
    // The input moved by (6, 5), so a glow of a few pixels stays inside the composition window
    // (an energy balance needs every scattered pixel).
    bool centred = false;
    // Light Wrap
    double radius = 3.0;
    double amount = 0.75;
    bool wrapOnly = false;
    // #516 Edge Extend
    double extendDistance = 4.0;
    double extendAmount = 1.0;
    // #524 Gaussian Blur with a radius per channel (five blurs and four channel combines): the
    // radius, the vertical radius with separate axes, and each channel's share of them.
    double blurRadius = 3.0;
    double blurRadiusY = 1.5;
    bool blurSeparate = false;
    std::array<double, 4> channelScale{1.0, 0.4, 0.0, 0.7};
    // #514 Denoise & Smooth Median: the radius and the threshold.
    double denoiseRadius = 2.0;
    double denoiseStrength = 0.0;
    // #509 Alpha Contour
    double contourWidth = 3.0;
    double contourSoftness = 1.0;
    double contourOffset = 1.0;
    core::Color4d contourColor{2.5, 0.5, 0.25, 0.8};
    runtime::detail::AlphaContourComposite contourComposite =
        runtime::detail::AlphaContourComposite::Over;
    // An animated Drop Shadow distance (0 at t = 0 to `distance` at t = 1), Glow intensity or
    // (#509) Alpha Contour width.
    bool animated = false;
    // The input is an HDR, partial-alpha solid instead of the gradient. Luma Key's coverage is a
    // function of colour, and the generated gradient's colour is within 2e-6, not bit-exact, on a
    // device, so the device's exact-alpha gate keys a solid, as the Luma Key native cases do. A
    // Glow keyed from the gradient is held to 2e-6 alpha instead (#545, gpu-backend.md).
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
                         {ParameterId::fromRaw(idBase + 5),
                          document::GradientValue{{{0.0, core::Color4d{0.5, 0.25, 0.1, 0.6}},
                                                   {1.0, core::Color4d{6.0, 3.0, 1.5, 1.0}}}}},
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
        published == Published::Mask ||
        (recipePublished && recipe == Recipe::Glow && (values.mask || values.dirt));
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
        if (values.centred) {
            const auto source = result.input;
            result.input = OperationIndex::fromRaw(operations.size());
            operations.emplace_back(CompiledImageEffect{
                NodeId::fromRaw(idBase + 15), source,
                Transform2DKernel{CompiledVec2Parameter{ParameterId::fromRaw(idBase + 16),
                                                        Vec2d{6.0, 5.0}},
                                  CompiledVec2Parameter{{}, Vec2d{}},
                                  CompiledVec2Parameter{{}, Vec2d{1.0, 1.0}}, scalar(idBase + 17, 0.0),
                                  Transform2DExtent::Transformed, kResampleFilterBilinear}});
        }
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
        case Recipe::Glow: {
            detail::GlowOperands operands{
                scalar(idBase + 31, values.threshold),
                scalar(idBase + 32, values.knee),
                animatedScalar(idBase + 33, values.intensity),
                scalar(idBase + 35, values.size),
                scalar(idBase + 36, values.falloff),
                CompiledColorParameter{ParameterId::fromRaw(idBase + 37), values.tint},
                values.octaves,
                values.glowOnly};
            operands.mode = values.mode;
            operands.exponent = scalar(idBase + 60, values.exponent);
            for (std::size_t channel = 0; channel < 3; ++channel)
                operands.radius[channel] = scalar(idBase + 61 + channel, values.channelRadius[channel]);
            operands.gradient = values.gradient;
            // #541: the octave tints run from the tint to the outer tint through a gradient.
            operands.tintGradient = bloom::runtime::CompiledGradientParameter{
                ParameterId::fromRaw(idBase + 64),
                values.tintGradient
                    ? *values.tintGradient
                    : document::GradientValue{{{0.0, values.tint}, {1.0, values.outerTint}}}};
            operands.points = values.points;
            operands.angle = scalar(idBase + 65, values.angle);
            operands.length = scalar(idBase + 66, values.length);
            operands.chromatic = scalar(idBase + 67, values.chromatic);
            operands.dirtAmount = scalar(idBase + 68, values.dirtAmount);
            operands.rollOff = scalar(idBase + 69, values.rollOff);
            result.recipe = detail::appendGlow(
                emitter, result.input, values.key ? std::optional(result.background) : std::nullopt,
                values.mask ? std::optional(result.mask) : std::nullopt, operands,
                values.dirt ? std::optional(result.mask) : std::nullopt);
            break;
        }
        case Recipe::LightWrap:
            result.recipe =
                detail::appendLightWrap(emitter, result.input, result.background,
                                        {scalar(idBase + 41, values.radius),
                                         scalar(idBase + 42, values.amount), values.wrapOnly});
            break;
        case Recipe::ChannelBlur: {
            detail::ChannelBlurOperands operands;
            operands.radius = scalar(idBase + 73, values.blurRadius);
            operands.radiusY = scalar(idBase + 74, values.blurRadiusY);
            operands.separate = values.blurSeparate;
            for (std::size_t channel = 0; channel < 4; ++channel)
                operands.scales[channel] =
                    scalar(idBase + 75 + channel, values.channelScale[channel]);
            result.recipe = detail::appendChannelBlur(emitter, result.input, operands);
            break;
        }
        case Recipe::Denoise:
            result.recipe = emitter.effect(
                result.input,
                DenoiseKernel{DenoiseMode::Median, scalar(idBase + 81, values.denoiseRadius),
                              scalar(idBase + 82, values.denoiseStrength)});
            break;
        case Recipe::EdgeExtend:
            result.recipe =
                detail::appendEdgeExtend(emitter, result.input,
                                         {scalar(idBase + 71, values.extendDistance),
                                          scalar(idBase + 72, values.extendAmount)});
            break;
        case Recipe::AlphaContour:
            // `animated` keys the width (0 at t = 0), so the distance transform reaches the widest
            // band the domains allow.
            result.recipe = detail::appendAlphaContour(
                emitter, result.input,
                {animatedScalar(idBase + 81, values.contourWidth),
                 scalar(idBase + 83, values.contourSoftness),
                 scalar(idBase + 84, values.contourOffset),
                 CompiledColorParameter{ParameterId::fromRaw(idBase + 85), values.contourColor},
                 values.contourComposite});
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

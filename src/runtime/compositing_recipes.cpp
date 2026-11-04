#include "compositing_recipes.hpp"

#include <bloom/render/image_coverage.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <variant>

namespace bloom::runtime::detail {
namespace {

using core::primitives::ScalarPrimitive;

[[nodiscard]] CompiledValueOperand valueOperand(const CompiledScalarParameter& parameter) {
    return std::visit(
        [&](const auto& source) -> CompiledValueOperand {
            using Source = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, double>)
                return CompiledValueOperand{parameter.id, CompiledValue{source}};
            else
                return CompiledValueOperand{parameter.id, source};
        },
        parameter.source);
}

[[nodiscard]] CompiledValueOperand valueOperand(const CompiledColorParameter& parameter) {
    return std::visit(
        [&](const auto& source) -> CompiledValueOperand {
            using Source = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, core::Color4d>)
                return CompiledValueOperand{parameter.id, CompiledValue{source}};
            else
                return CompiledValueOperand{parameter.id, source};
        },
        parameter.source);
}

// A Levels channel that maps every value to itself: black 0, white 1, gamma 1, output 0..1.
[[nodiscard]] LevelsChannelParameters identityChannel() {
    return {RecipeEmitter::constant(0.0), RecipeEmitter::constant(1.0),
            RecipeEmitter::constant(1.0), RecipeEmitter::constant(0.0),
            RecipeEmitter::constant(1.0)};
}

// Levels scaling the straight red, green and blue by `gains` (and alpha by `alphaGain`): each
// channel's output white, so a value x becomes gain * x with HDR and negative values extrapolated
// and nothing clamped. Levels re-associates by the graded alpha, so the result stays premultiplied.
[[nodiscard]] LevelsKernel gainLevels(const std::array<CompiledScalarParameter, 3>& gains,
                                      const std::optional<CompiledScalarParameter>& alphaGain) {
    LevelsKernel kernel;
    for (auto& channel : kernel.channels)
        channel = identityChannel();
    for (std::size_t index = 0; index < gains.size(); ++index)
        kernel.channels[index + 1].outputWhite = gains[index];
    if (alphaGain)
        kernel.channels[4].outputWhite = *alphaGain;
    kernel.clamp = false;
    return kernel;
}

} // namespace

CompiledScalarParameter RecipeEmitter::constant(const double value) {
    return CompiledScalarParameter{{}, value};
}

OperationIndex RecipeEmitter::effect(const OperationIndex input, ImageEffectKernel kernel,
                                     const std::optional<OperationIndex> secondary) {
    const auto index = OperationIndex::fromRaw(operations_.size());
    operations_.emplace_back(
        CompiledImageEffect{node_, input, std::move(kernel), false, false, secondary});
    return index;
}

OperationIndex RecipeEmitter::merge(
    const std::initializer_list<std::pair<OperationIndex, core::BlendMode>> entries) {
    CompiledMerge merge{node_, {}};
    for (const auto& [input, mode] : entries)
        merge.entries.push_back(CompiledMergeInput{{}, {}, input, mode});
    const auto index = OperationIndex::fromRaw(operations_.size());
    operations_.emplace_back(std::move(merge));
    return index;
}

CompiledScalarParameter
RecipeEmitter::scalar(const ScalarPrimitive primitive,
                      const std::initializer_list<CompiledScalarParameter> operands) {
    std::vector<double> constants;
    for (const auto& operand : operands)
        if (const auto* value = std::get_if<double>(&operand.source))
            constants.push_back(*value);
    if (constants.size() == operands.size()) {
        // The value graph's own evaluation, so a folded value is the value it would compute.
        const auto folded = core::primitives::evaluateScalar(primitive, std::span(constants));
        if (folded.hasValue())
            return constant(*folded.value());
    }
    CompiledValueScalarMath math{primitive, false, {}};
    for (const auto& operand : operands)
        math.operands.push_back(valueOperand(operand));
    return CompiledScalarParameter{{}, values_(std::move(math), 1)};
}

CompiledScalarParameter RecipeEmitter::reuse(const CompiledScalarParameter& operand) {
    if (std::holds_alternative<ScalarCurveIndex>(operand.source))
        return scalar(ScalarPrimitive::Multiply, {operand, constant(1.0)});
    return CompiledScalarParameter{{}, operand.source};
}

std::array<CompiledScalarParameter, 4>
RecipeEmitter::components(const CompiledColorParameter& color) {
    if (const auto* value = std::get_if<core::Color4d>(&color.source))
        return {constant(value->red), constant(value->green), constant(value->blue),
                constant(value->alpha)};
    const auto first = values_(CompiledValueSeparate{valueOperand(color), 4, true}, 4).value();
    return {CompiledScalarParameter{{}, ValueOutputIndex::fromRaw(first)},
            CompiledScalarParameter{{}, ValueOutputIndex::fromRaw(first + 1)},
            CompiledScalarParameter{{}, ValueOutputIndex::fromRaw(first + 2)},
            CompiledScalarParameter{{}, ValueOutputIndex::fromRaw(first + 3)}};
}

CompiledVec2Parameter RecipeEmitter::vector(const CompiledScalarParameter& x,
                                            const CompiledScalarParameter& y) {
    const auto* constantX = std::get_if<double>(&x.source);
    const auto* constantY = std::get_if<double>(&y.source);
    if (constantX && constantY)
        return CompiledVec2Parameter{{}, document::Vec2d{*constantX, *constantY}};
    return CompiledVec2Parameter{
        {}, values_(CompiledValueCombine{{valueOperand(x), valueOperand(y)}, false}, 1)};
}

// Drop Shadow: the input's alpha blurred by `softness` (the Matte Blur coverage filter, whose
// transparent halo lets the shadow grow), moved by the offset (Transform 2D), coloured and faded
// (Levels on the white coverage: straight colour 1 becomes the shadow colour, alpha becomes
// coverage * opacity * colour alpha), then the input composited over it (a Normal Merge).
OperationIndex appendDropShadow(RecipeEmitter& emitter, const OperationIndex input,
                                const DropShadowOperands& operands) {
    const auto coverage = emitter.effect(
        input, SeparableBlurKernel{SeparableBlurKind::Gaussian, operands.softness,
                                   BlurEdgeMode::Transparent, BlurChannels::Coverage});
    // Clockwise from up on a y-down canvas: x = d sin(a), y = -d cos(a).
    const auto radians =
        emitter.scalar(ScalarPrimitive::Multiply,
                       {operands.direction, RecipeEmitter::constant(std::numbers::pi / 180.0)});
    const auto x =
        emitter.scalar(ScalarPrimitive::Multiply,
                       {operands.distance, emitter.scalar(ScalarPrimitive::Sine, {radians})});
    const auto y = emitter.scalar(
        ScalarPrimitive::Multiply,
        {operands.distance, emitter.scalar(ScalarPrimitive::Negate,
                                           {emitter.scalar(ScalarPrimitive::Cosine, {radians})})});
    const auto moved = emitter.effect(
        coverage,
        Transform2DKernel{emitter.vector(x, y), CompiledVec2Parameter{{}, document::Vec2d{}},
                          CompiledVec2Parameter{{}, document::Vec2d{1.0, 1.0}},
                          RecipeEmitter::constant(0.0), Transform2DExtent::Transformed,
                          kResampleFilterBilinear});
    const auto color = emitter.components(operands.color);
    const auto alpha = emitter.scalar(ScalarPrimitive::Multiply, {operands.opacity, color[3]});
    const auto shadow = emitter.effect(moved, gainLevels({color[0], color[1], color[2]}, alpha));
    if (operands.shadowOnly)
        return shadow;
    return emitter.merge({{input, core::BlendMode::Normal}, {shadow, core::BlendMode::Normal}});
}

// Glow: Luma Key ramps the source's straight luma from threshold - knee/2 to threshold + knee/2,
// and Set Alpha with that key keeps exactly the source's straight colour at the keyed coverage,
// (premultiplied colour * ramp, alpha * ramp) -- never alpha squared. An optional Mask multiplies
// that coverage (Multiply Alpha). The result is blurred at each octave (Gaussian, transparent
// halo), the widest first, and the octaves are folded widest-first by Image Mix with the weights
// falloff^j / sum(falloff^i), so the glow is their normalized weighted sum. Levels tints it and
// scales its colour by the intensity, and an Add Merge adds it to the input: co = cs + cb, alpha
// as + ab - as * ab. Nothing is clamped, so HDR values glow by their real luma and keep it.
OperationIndex appendGlow(RecipeEmitter& emitter, const OperationIndex input,
                          const std::optional<OperationIndex> key,
                          const std::optional<OperationIndex> mask, const GlowOperands& operands) {
    const auto source = key.value_or(input);
    const auto low =
        emitter.scalar(ScalarPrimitive::MultiplyAdd,
                       {operands.knee, RecipeEmitter::constant(-0.5), operands.threshold});
    const auto high =
        emitter.scalar(ScalarPrimitive::MultiplyAdd,
                       {operands.knee, RecipeEmitter::constant(0.5), operands.threshold});
    const auto keyed = emitter.effect(source, LumaKeyKernel{low, high, false});
    auto bright = emitter.effect(
        source,
        ImageCombineKernel{ImageCombineMode::SetAlpha, {}, render::ImageCoverageChannel::Alpha},
        keyed);
    if (mask)
        bright = emitter.effect(bright,
                                ImageCombineKernel{ImageCombineMode::MultiplyAlpha,
                                                   RecipeEmitter::constant(1.0),
                                                   render::ImageCoverageChannel::Alpha},
                                *mask);
    const auto octaves = std::max<std::int64_t>(1, operands.octaves);
    const auto blurred = [&](const std::int64_t octave) {
        // Octave j has radius size * 2^(j - (octaves - 1)): the widest is `size`.
        const auto radius = emitter.scalar(
            ScalarPrimitive::Multiply,
            {operands.size,
             RecipeEmitter::constant(std::ldexp(1.0, static_cast<int>(octave - (octaves - 1))))});
        return emitter.effect(bright,
                              SeparableBlurKernel{SeparableBlurKind::Gaussian, radius,
                                                  BlurEdgeMode::Transparent, BlurChannels::Rgba});
    };
    auto glow = blurred(octaves - 1);
    // sum = 1 + f + ... + f^(octaves - 1 - j); octave j enters with factor 1 / sum.
    auto sum = RecipeEmitter::constant(1.0);
    for (auto octave = octaves - 2; octave >= 0; --octave) {
        sum = emitter.scalar(ScalarPrimitive::MultiplyAdd,
                             {operands.falloff, sum, RecipeEmitter::constant(1.0)});
        const auto factor = emitter.scalar(ScalarPrimitive::Reciprocal, {sum});
        const auto octaveImage = blurred(octave);
        glow = emitter.effect(
            glow,
            ImageCombineKernel{ImageCombineMode::Mix, factor, render::ImageCoverageChannel::Alpha},
            octaveImage);
    }
    const auto tint = emitter.components(operands.tint);
    std::array<CompiledScalarParameter, 3> gains{};
    for (std::size_t channel = 0; channel < gains.size(); ++channel)
        gains[channel] =
            emitter.scalar(ScalarPrimitive::Multiply, {tint[channel], operands.intensity});
    glow = emitter.effect(glow, gainLevels(gains, std::nullopt));
    if (operands.glowOnly)
        return glow;
    return emitter.merge({{glow, core::BlendMode::Add}, {input, core::BlendMode::Normal}});
}

// Light Wrap: the background blurred by `radius` (Clamp, so the frame edge does not darken), held
// out by a `radius` blur of the foreground's alpha (Matte Blur's transparent edge, so the edge of
// the foreground's data window is an edge; Multiply Alpha by its inverted coverage), scaled by
// `amount` (Levels on the alpha, which keeps the wrap's straight colour), then composited Atop
// the foreground: co = wrap * af + fg * (1 - wrap alpha), alpha af. So the wrap replaces the
// foreground's colour inside its edges and never changes its alpha. Wrap only composites it In the
// foreground instead (wrap * af, alpha wrap alpha * af). The amount is a separate Levels gain
// rather than Multiply Alpha's factor: Levels' alpha is one rounding on every route, where a
// factor below one rounds coverage * factor first on the device.
OperationIndex appendLightWrap(RecipeEmitter& emitter, const OperationIndex input,
                               const OperationIndex background, const LightWrapOperands& operands) {
    const auto softened =
        emitter.effect(background, SeparableBlurKernel{SeparableBlurKind::Gaussian, operands.radius,
                                                       BlurEdgeMode::Clamp, BlurChannels::Rgba});
    // The foreground spread shares the radius; the background blur owns its identity.
    const auto spread = emitter.effect(
        input, SeparableBlurKernel{SeparableBlurKind::Gaussian, emitter.reuse(operands.radius),
                                   BlurEdgeMode::Transparent, BlurChannels::Coverage});
    const auto heldOut = emitter.effect(
        softened,
        ImageCombineKernel{ImageCombineMode::MultiplyAlpha, RecipeEmitter::constant(1.0),
                           render::ImageCoverageChannel::Alpha, std::nullopt, true},
        spread);
    const auto unit = RecipeEmitter::constant(1.0);
    const auto wrap = emitter.effect(heldOut, gainLevels({unit, unit, unit}, operands.amount));
    return emitter.merge({{wrap, operands.wrapOnly ? core::BlendMode::In : core::BlendMode::Atop},
                          {input, core::BlendMode::Normal}});
}

RecipeEmitter::ValueSink definitionValueSink(CompiledCompositionPlanDefinition& definition) {
    return [&definition](CompiledValueKernel kernel, const std::uint8_t outputCount) {
        const auto first = ValueOutputIndex::fromRaw(definition.valueOutputCount);
        definition.valueOperations.push_back(
            CompiledValueOperation{{}, first, outputCount, std::move(kernel), false});
        definition.valueOutputCount += outputCount;
        return first;
    };
}

} // namespace bloom::runtime::detail

#include "compositing_recipes.hpp"
#include "gradient_source.hpp"

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

// Whether an operand is the constant `value`: such an operand needs no primitive at all.
[[nodiscard]] bool isConstant(const CompiledScalarParameter& parameter, const double value) {
    const auto* constant = std::get_if<double>(&parameter.source);
    return constant != nullptr && *constant == value;
}

} // namespace

CompiledScalarParameter RecipeEmitter::constant(const double value) {
    return CompiledScalarParameter{{}, value};
}

OperationIndex RecipeEmitter::effect(const OperationIndex input, ImageEffectKernel kernel,
                                     const std::optional<OperationIndex> secondary,
                                     const std::optional<OperationIndex> mask) {
    const auto index = OperationIndex::fromRaw(operations_.size());
    operations_.emplace_back(
        CompiledImageEffect{node_, input, std::move(kernel), false, false, secondary, mask});
    return index;
}

OperationIndex RecipeEmitter::merge(
    const std::initializer_list<std::pair<OperationIndex, core::BlendMode>> entries) {
    return merge(std::vector<std::pair<OperationIndex, core::BlendMode>>(entries));
}

OperationIndex
RecipeEmitter::merge(const std::vector<std::pair<OperationIndex, core::BlendMode>>& entries) {
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

CompiledColorParameter RecipeEmitter::gradientColor(const CompiledGradientParameter& gradient,
                                                   const double position) {
    // A constant gradient folds to its colour; a keyed one, or (#543) one with driven stops,
    // samples per frame in the value graph.
    if (const auto* constant = std::get_if<document::GradientValue>(&gradient.source);
        constant != nullptr && gradient.drivers.empty())
        return CompiledColorParameter{{}, gradientValueTintAt(*constant, position)};
    CompiledValueGradientSample sample{gradient.source, position};
    for (const auto& driver : gradient.drivers)
        sample.drivers.push_back(
            {driver.stop, driver.component, CompiledValueOperand{driver.id, driver.value}});
    return CompiledColorParameter{{}, values_(std::move(sample), 1)};
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
//
// #506 modes (docs/architecture/image-filters.md, "Glow modes"):
//  * Physical keys the light ENERGY: Luma Key over the premultiplied colour made opaque (Channel
//    Select R, G, B, 1), so the ramp k reads premultiplied luma, and Multiply Alpha takes
//    B = source * k (colour and alpha). The octaves are the Classic fold with the per-octave ratio
//    2^(2 - exponent): a Gaussian scale mixture whose weight per octave grows as r^(2 - p) has the
//    point-spread function r^-p between the tightest and widest radius. The normalized PSF moves a
//    fraction min(intensity, 1) of B out of its pixel: the input's colour is Mixed towards zero by
//    that fraction under the mask k (alpha untouched), and the glow carries intensity * B, so
//    at intensity <= 1 the frame's light is conserved; above 1 the excess is added. A key input is
//    a separate light source, so nothing is removed from the input.
//  * Streak keys like Classic and replaces the octaves by `points` rays at angle + 360 k / points,
//    each kStreakPasses chained Directional Blur passes (spacing length * 4^p / 1023, decay
//    0.01^(4^p / 1023)) that multiply into one 1024-tap exponential ray; each ray carries 1/points
//    of the light (Levels on its alpha) and an Add Merge sums them.
//  * Per-channel radius (and Streak's chromatic dispersion, which shortens green by chromatic / 3
//    and blue by 2 chromatic / 3) builds the glow once per channel at its own fraction of the size
//    or length; Channel Select keeps that channel with a third of its coverage and an Add Merge
//    sums the three, so each channel's colour is exact.
//  * Gradient (#541) tints octave j by the tint gradient's colour at j / (octaves - 1) before the
//    fold (gradientTintAt: c + (next - c) * u per channel, so a two-stop gradient is schema 1's
//    tint + (outerTint - tint) * j / (octaves - 1)); the final Levels then scales by the intensity
//    alone.
//  * Dirt adds the glow times the dirt image's luma coverage (Multiply Alpha, Luma), scaled by
//    dirtAmount; roll-off (Highlight Roll-off) then compresses the whole glow layer.
OperationIndex appendGlow(RecipeEmitter& emitter, const OperationIndex input,
                          const std::optional<OperationIndex> key,
                          const std::optional<OperationIndex> mask, const GlowOperands& operands,
                          const std::optional<OperationIndex> dirt) {
    const auto source = key.value_or(input);
    const auto low =
        emitter.scalar(ScalarPrimitive::MultiplyAdd,
                       {operands.knee, RecipeEmitter::constant(-0.5), operands.threshold});
    const auto high =
        emitter.scalar(ScalarPrimitive::MultiplyAdd,
                       {operands.knee, RecipeEmitter::constant(0.5), operands.threshold});
    const bool physical = operands.mode == GlowMode::Physical;
    const bool streak = operands.mode == GlowMode::Streak;
    const auto multiplyAlpha = [](const render::ImageCoverageChannel channel) {
        return ImageCombineKernel{ImageCombineMode::MultiplyAlpha, RecipeEmitter::constant(1.0),
                                  channel};
    };
    auto bright = source;
    std::optional<OperationIndex> energyKey;
    if (physical) {
        const auto opaque = emitter.effect(
            source, ChannelSelectKernel{{ImageChannelSource::Red, ImageChannelSource::Green,
                                         ImageChannelSource::Blue, ImageChannelSource::One},
                                        false});
        auto keyed = emitter.effect(opaque, LumaKeyKernel{low, high, false});
        if (mask)
            keyed = emitter.effect(keyed, multiplyAlpha(render::ImageCoverageChannel::Alpha), *mask);
        bright = emitter.effect(source, multiplyAlpha(render::ImageCoverageChannel::Alpha), keyed);
        energyKey = keyed;
    } else {
        const auto keyed = emitter.effect(source, LumaKeyKernel{low, high, false});
        bright = emitter.effect(
            source,
            ImageCombineKernel{ImageCombineMode::SetAlpha, {}, render::ImageCoverageChannel::Alpha},
            keyed);
        if (mask)
            bright =
                emitter.effect(bright, multiplyAlpha(render::ImageCoverageChannel::Alpha), *mask);
    }

    const auto octaves = std::max<std::int64_t>(1, operands.octaves);
    // Physical's per-octave ratio 2^(2 - exponent); Classic's is the falloff itself.
    const auto falloff =
        physical ? emitter.scalar(ScalarPrimitive::Power,
                                  {RecipeEmitter::constant(2.0),
                                   emitter.scalar(ScalarPrimitive::Subtract,
                                                  {RecipeEmitter::constant(2.0), operands.exponent})})
                 : operands.falloff;
    const auto octaveStack = [&](const std::optional<CompiledScalarParameter>& scale) {
        const auto widest =
            scale ? emitter.scalar(ScalarPrimitive::Multiply, {operands.size, *scale})
                  : operands.size;
        const auto blurred = [&](const std::int64_t octave) {
            // Octave j has radius size * 2^(j - (octaves - 1)): the widest is `size`.
            const auto radius = emitter.scalar(
                ScalarPrimitive::Multiply,
                {widest, RecipeEmitter::constant(
                             std::ldexp(1.0, static_cast<int>(octave - (octaves - 1))))});
            auto image = emitter.effect(bright, SeparableBlurKernel{SeparableBlurKind::Gaussian,
                                                                    radius, BlurEdgeMode::Transparent,
                                                                    BlurChannels::Rgba});
            if (operands.gradient) {
                const double t = octaves > 1 ? static_cast<double>(octave) /
                                                   static_cast<double>(octaves - 1)
                                             : 0.0;
                const auto tint = emitter.components(emitter.gradientColor(operands.tintGradient, t));
                image = emitter.effect(image, gainLevels({tint[0], tint[1], tint[2]}, std::nullopt));
            }
            return image;
        };
        auto glow = blurred(octaves - 1);
        // sum = 1 + f + ... + f^(octaves - 1 - j); octave j enters with factor 1 / sum.
        auto sum = RecipeEmitter::constant(1.0);
        for (auto octave = octaves - 2; octave >= 0; --octave) {
            sum = emitter.scalar(ScalarPrimitive::MultiplyAdd,
                                 {falloff, sum, RecipeEmitter::constant(1.0)});
            const auto factor = emitter.scalar(ScalarPrimitive::Reciprocal, {sum});
            const auto octaveImage = blurred(octave);
            glow = emitter.effect(glow,
                                  ImageCombineKernel{ImageCombineMode::Mix, factor,
                                                     render::ImageCoverageChannel::Alpha},
                                  octaveImage);
        }
        return glow;
    };
    const auto points = std::max<std::int64_t>(1, operands.points);
    const auto unit = RecipeEmitter::constant(1.0);
    const auto streakStack = [&](const std::optional<CompiledScalarParameter>& scale) {
        const auto length =
            scale ? emitter.scalar(ScalarPrimitive::Multiply, {operands.length, *scale})
                  : operands.length;
        std::vector<std::pair<OperationIndex, core::BlendMode>> rays;
        for (std::int64_t ray = 0; ray < points; ++ray) {
            const auto angle = emitter.scalar(
                ScalarPrimitive::Add,
                {operands.angle, RecipeEmitter::constant(360.0 * static_cast<double>(ray) /
                                                         static_cast<double>(points))});
            auto image = bright;
            for (std::int64_t pass = 0; pass < kStreakPasses; ++pass) {
                const double reach = std::ldexp(1.0, static_cast<int>(2 * pass)) /
                                     static_cast<double>(kStreakTaps - 1);
                const auto spacing = emitter.scalar(ScalarPrimitive::Multiply,
                                                    {length, RecipeEmitter::constant(reach)});
                image = emitter.effect(
                    image, DirectionalBlurKernel{angle, spacing, std::pow(kStreakEndWeight, reach)});
            }
            if (points > 1)
                image = emitter.effect(
                    image, gainLevels({unit, unit, unit},
                                      RecipeEmitter::constant(1.0 / static_cast<double>(points))));
            rays.emplace_back(image, core::BlendMode::Add);
        }
        return rays.size() == 1 ? rays.front().first : emitter.merge(rays);
    };
    const auto stack = [&](const std::optional<CompiledScalarParameter>& scale) {
        return streak ? streakStack(scale) : octaveStack(scale);
    };

    const bool chromatic = streak && !isConstant(operands.chromatic, 0.0);
    const bool split = chromatic || !isConstant(operands.radius[0], 1.0) ||
                       !isConstant(operands.radius[1], 1.0) || !isConstant(operands.radius[2], 1.0);
    auto glow = bright;
    if (!split) {
        glow = stack(std::nullopt);
    } else {
        std::vector<std::pair<OperationIndex, core::BlendMode>> channels;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            auto scale = operands.radius[channel];
            if (chromatic && channel > 0)
                scale = emitter.scalar(
                    ScalarPrimitive::Multiply,
                    {scale, emitter.scalar(ScalarPrimitive::MultiplyAdd,
                                           {operands.chromatic,
                                            RecipeEmitter::constant(
                                                -static_cast<double>(channel) / 3.0),
                                            unit})});
            auto image = stack(scale);
            // The channel alone with a third of its coverage: the colour is unchanged (Levels'
            // straight gain 3 re-associates by alpha / 3), and three thirds sum to about one.
            const auto three = RecipeEmitter::constant(3.0);
            image = emitter.effect(
                image, gainLevels({three, three, three}, RecipeEmitter::constant(1.0 / 3.0)));
            std::array<ImageChannelSource, 4> sources{ImageChannelSource::Zero,
                                                      ImageChannelSource::Zero,
                                                      ImageChannelSource::Zero,
                                                      ImageChannelSource::Alpha};
            sources[channel] = channel == 0   ? ImageChannelSource::Red
                               : channel == 1 ? ImageChannelSource::Green
                                              : ImageChannelSource::Blue;
            image = emitter.effect(image, ChannelSelectKernel{sources, false});
            channels.emplace_back(image, core::BlendMode::Add);
        }
        glow = emitter.merge(channels);
    }

    std::array<CompiledScalarParameter, 3> gains{};
    if (operands.gradient && !streak) {
        for (auto& gain : gains)
            gain = emitter.reuse(operands.intensity);
    } else {
        const auto tint = emitter.components(operands.tint);
        for (std::size_t channel = 0; channel < gains.size(); ++channel)
            gains[channel] =
                emitter.scalar(ScalarPrimitive::Multiply, {tint[channel], operands.intensity});
    }
    glow = emitter.effect(glow, gainLevels(gains, std::nullopt));
    if (dirt) {
        auto dirty =
            emitter.effect(glow, multiplyAlpha(render::ImageCoverageChannel::Luma), *dirt);
        dirty = emitter.effect(dirty, gainLevels({emitter.reuse(operands.dirtAmount),
                                                  emitter.reuse(operands.dirtAmount),
                                                  emitter.reuse(operands.dirtAmount)},
                                                 std::nullopt));
        glow = emitter.merge({{dirty, core::BlendMode::Add}, {glow, core::BlendMode::Normal}});
    }
    if (!isConstant(operands.rollOff, 0.0))
        glow = emitter.effect(glow, HighlightRollOffKernel{operands.rollOff});
    if (operands.glowOnly)
        return glow;
    auto base = input;
    if (physical && !key) {
        // The scattered fraction leaves the input's colour where it was keyed; alpha stays.
        const auto unlit = emitter.effect(
            input, ChannelSelectKernel{{ImageChannelSource::Zero, ImageChannelSource::Zero,
                                        ImageChannelSource::Zero, ImageChannelSource::Alpha},
                                       false});
        const auto scattered =
            emitter.scalar(ScalarPrimitive::Minimum, {emitter.reuse(operands.intensity), unit});
        base = emitter.effect(input,
                              ImageCombineKernel{ImageCombineMode::Mix, scattered,
                                                 render::ImageCoverageChannel::Alpha},
                              unlit, energyKey);
    }
    return emitter.merge({{glow, core::BlendMode::Add}, {base, core::BlendMode::Normal}});
}

// Light Wrap: the background blurred by `radius` (Clamp, so the frame edge does not darken), held
// out by a `radius` blur of the foreground's alpha (Matte Blur's transparent edge, so the edge of
// the foreground's data window is an edge; Multiply Alpha by its inverted coverage), scaled by
// `amount` (Levels on the alpha, which keeps the wrap's straight colour), then composited Atop
// the foreground: co = wrap * af + fg * (1 - wrap alpha), alpha af. So the wrap replaces the
// foreground's colour inside its edges and never changes its alpha. Wrap only composites it In the
// foreground instead (wrap * af, alpha wrap alpha * af). The amount is a separate Levels gain
// after the hold-out, so a changed amount re-runs only Levels.
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

#include "compositing_recipes.hpp"
#include "gradient_source.hpp"

#include <bloom/core/field_program.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
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

void RecipeEmitter::setImages(const OperationIndex effect, std::vector<OperationIndex> images) {
    std::get<CompiledImageEffect>(operations_[effect.value()]).fieldImages = std::move(images);
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

// Edge Extend. With premultiplied colour p = a c and alpha a, the input scaled by its own alpha
// (Multiply Alpha) is a c a over a a; its Gaussian blur by `distance` (transparent edge) divided by
// its alpha (Unpremultiply) is the straight colour of the nearby pixels weighted by alpha squared,
// so solid pixels count for much more than the soft ones that carry the fringe. A Channel Combine
// puts the input's alpha back and re-associates it: x = a c_ext, alpha a. Image Mix then moves each
// pixel towards x by `amount` times its transparency (1 - a), the mask being white held out by the
// input's inverted alpha, so a solid pixel is exactly the input and a pixel of alpha a keeps its
// alpha: p' = p + amount (1 - a) (x - p).
OperationIndex appendEdgeExtend(RecipeEmitter& emitter, const OperationIndex input,
                                const EdgeExtendOperands& operands) {
    const auto weighted = emitter.effect(
        input,
        ImageCombineKernel{ImageCombineMode::MultiplyAlpha, RecipeEmitter::constant(1.0),
                           render::ImageCoverageChannel::Alpha},
        input);
    const auto spread = emitter.effect(
        weighted, SeparableBlurKernel{SeparableBlurKind::Gaussian, operands.distance,
                                      BlurEdgeMode::Transparent, BlurChannels::Rgba});
    const auto straight = emitter.effect(
        spread, AlphaAssociationKernel{AlphaAssociationMode::Unpremultiply, false});
    const auto extended = emitter.effect(
        straight, ChannelCombineKernel{3, ImageChannelSource::Alpha, true}, input);
    const auto white = emitter.effect(
        input, ChannelSelectKernel{{ImageChannelSource::One, ImageChannelSource::One,
                                    ImageChannelSource::One, ImageChannelSource::One},
                                   false});
    const auto transparency = emitter.effect(
        white,
        ImageCombineKernel{ImageCombineMode::MultiplyAlpha, RecipeEmitter::constant(1.0),
                           render::ImageCoverageChannel::Alpha, std::nullopt, true},
        input);
    return emitter.effect(input,
                          ImageCombineKernel{ImageCombineMode::Mix, operands.amount,
                                             render::ImageCoverageChannel::Alpha},
                          extended, transparency);
}

namespace {

// The one-pixel antialiased edge of the band: its coverage falls from 1 to 0 over one pixel (plus
// the softness) centred on the band's edge.
constexpr double kContourEdgePixels = 1.0;
// Past the band's outer edge the distance transform still needs this margin, so the clamped
// distance (reach plus one pixel) is never inside the band.
constexpr double kContourReachMargin = 2.0;

[[nodiscard]] double contourReach(const double width, const double softness, const double offset) {
    return std::abs(offset) + std::max(0.5 * width, 0.5 * kContourEdgePixels) +
           0.5 * kContourEdgePixels + 0.5 * softness + kContourReachMargin;
}

[[nodiscard]] std::optional<double> constantOf(const CompiledScalarParameter& parameter) {
    if (const auto* constant = std::get_if<double>(&parameter.source))
        return *constant;
    return std::nullopt;
}

// The smallest white point of the inner coverage's Levels (a Choke of 1): the ramp is then 1/1024
// of the blurred coverage wide, an edge that is hard to within a thousandth.
constexpr double kInnerHardestWhitePoint = 1.0 / 1024.0;

// The blurred outside coverage m of an input, one minus it where the input is, and the choke:
//  * hole = white held out by m (Multiply Alpha, inverted): alpha 1 - m over the input's window,
//    so the data window is the input's and the pixels beyond the shifted blur count as outside;
//  * Levels on alpha only maps hole h to clamp(h / w), w = max((1 - choke) / 2, 1/1024). A pixel
//    exactly on an edge has h = 1/2, so at choke 0 the edge is full strength and fades inward over
//    the blur; each step of choke lowers w and so widens the solid band until choke 1, where
//    anything the blur reaches is solid.
// `invert` turns the result into 1 - the shaped coverage (Inner Glow's Centre source).
[[nodiscard]] OperationIndex innerCoverage(RecipeEmitter& emitter, const OperationIndex input,
                                           const OperationIndex blurredAlpha,
                                           const CompiledScalarParameter& choke) {
    const auto white = emitter.effect(
        input, ChannelSelectKernel{{ImageChannelSource::One, ImageChannelSource::One,
                                    ImageChannelSource::One, ImageChannelSource::One},
                                   false});
    const auto hole = emitter.effect(
        white,
        ImageCombineKernel{ImageCombineMode::MultiplyAlpha, RecipeEmitter::constant(1.0),
                           render::ImageCoverageChannel::Alpha, std::nullopt, true},
        blurredAlpha);
    LevelsKernel levels;
    for (auto& channel : levels.channels)
        channel = identityChannel();
    levels.channels[4].inputWhite = emitter.scalar(
        ScalarPrimitive::Maximum,
        {emitter.scalar(ScalarPrimitive::MultiplyAdd,
                        {choke, RecipeEmitter::constant(-0.5), RecipeEmitter::constant(0.5)}),
         RecipeEmitter::constant(kInnerHardestWhitePoint)});
    levels.clamp = false;
    return emitter.effect(hole, std::move(levels));
}

// Colours coverage `shaped` (alpha only; straight colour 1): the straight colour times `gain`, the
// alpha times `alpha` (or, inverted, alpha * (1 - coverage)). Nothing is clamped but alpha.
[[nodiscard]] OperationIndex innerPaint(RecipeEmitter& emitter, const OperationIndex shaped,
                                        const std::array<CompiledScalarParameter, 3>& gains,
                                        const CompiledScalarParameter& alpha, const bool invert) {
    LevelsKernel kernel = gainLevels(gains, std::nullopt);
    if (invert) {
        kernel.channels[4].outputBlack = alpha;
        kernel.channels[4].outputWhite = RecipeEmitter::constant(0.0);
    } else {
        kernel.channels[4].outputWhite = alpha;
    }
    return emitter.effect(shaped, std::move(kernel));
}

} // namespace

double alphaContourReach(const AlphaContourOperands& operands) {
    const auto width = constantOf(operands.width);
    const auto softness = constantOf(operands.softness);
    const auto offset = constantOf(operands.offset);
    if (width && softness && offset)
        return contourReach(std::clamp(*width, 0.0, document::kMaximumAlphaContourWidth),
                            std::clamp(*softness, 0.0, document::kMaximumAlphaContourSoftness),
                            std::clamp(*offset, -document::kMaximumAlphaContourOffset,
                                       document::kMaximumAlphaContourOffset));
    return contourReach(document::kMaximumAlphaContourWidth, document::kMaximumAlphaContourSoftness,
                        document::kMaximumAlphaContourOffset);
}

// Alpha Contour. MaskDistanceKernel gives every pixel of the input's data window grown by the
// reach the signed Euclidean distance d from the input's alpha edge (alpha 1/2; outside
// positive). A field raster over that window reads d through a mask Shape and gives the band's
// coverage k = clamp(1 - (|d - offset| - lo) / (hi - lo), 0, 1) with h = max(width / 2, 1/2),
// lo = h - 1/2 - softness / 2 and hi = h + 1/2 + softness / 2: 1 inside the band, falling over one
// pixel plus the softness across each of its edges. The colour is premultiplied by its alpha times
// min(width, 1) (a band thinner than a pixel is a one-pixel line that fades with its width), and
// k times it is the raster's premultiplied result, so HDR colour is kept. Width, softness and
// offset are clamped to their domains, so a driven value outside them cannot outrun the reach.
OperationIndex appendAlphaContour(RecipeEmitter& emitter, const OperationIndex input,
                                  const AlphaContourOperands& operands) {
    using core::field::FieldOp;
    const auto constant = &RecipeEmitter::constant;
    const auto width =
        emitter.scalar(ScalarPrimitive::Clamp, {emitter.reuse(operands.width), constant(0.0),
                                                constant(document::kMaximumAlphaContourWidth)});
    const auto softness =
        emitter.scalar(ScalarPrimitive::Clamp, {emitter.reuse(operands.softness), constant(0.0),
                                                constant(document::kMaximumAlphaContourSoftness)});
    const auto offset =
        emitter.scalar(ScalarPrimitive::Clamp, {emitter.reuse(operands.offset),
                                                constant(-document::kMaximumAlphaContourOffset),
                                                constant(document::kMaximumAlphaContourOffset)});
    const auto half =
        emitter.scalar(ScalarPrimitive::Maximum,
                       {emitter.scalar(ScalarPrimitive::Multiply, {width, constant(0.5)}),
                        constant(0.5 * kContourEdgePixels)});
    const auto spread =
        emitter.scalar(ScalarPrimitive::MultiplyAdd,
                       {softness, constant(0.5), constant(0.5 * kContourEdgePixels)});
    const auto low = emitter.scalar(ScalarPrimitive::Subtract, {half, spread});
    const auto high = emitter.scalar(ScalarPrimitive::Add, {half, spread});
    const auto color = emitter.components(operands.color);
    const auto alpha = emitter.scalar(
        ScalarPrimitive::Multiply,
        {color[3], emitter.scalar(ScalarPrimitive::Minimum, {width, constant(1.0)})});
    const auto premultiplied = [&](const std::size_t channel) {
        return emitter.scalar(ScalarPrimitive::Multiply, {color[channel], alpha});
    };

    const auto instruction =
        [](const FieldOp op, const std::int8_t output, const std::array<std::int8_t, 4> inputs,
           std::vector<std::int64_t> choices, std::vector<CompiledScalarParameter> scalars,
           std::vector<CompiledVec2Parameter> vectors = {}, const std::int8_t slot = -1) {
            FieldInstruction each;
            each.op = static_cast<std::uint8_t>(op);
            each.output = output;
            each.inputs = inputs;
            each.choices = std::move(choices);
            each.scalars = std::move(scalars);
            each.vectors = std::move(vectors);
            each.slot = slot;
            return each;
        };
    const auto mathOp = [](const core::field::FieldMathOperation operation) {
        return std::vector<std::int64_t>{static_cast<std::int64_t>(operation)};
    };
    FieldProgram program;
    // r0 the field position; r1 the signed distance (lane 0) of the mask in image slot 0.
    program.instructions.push_back(instruction(FieldOp::Coordinate, 0, {-1, -1, -1, -1}, {}, {}));
    program.instructions.push_back(
        instruction(FieldOp::Shape, 1, {0, -1, -1, -1}, {1, 0, 0, 0, 5},
                    {constant(0.0), constant(0.0), constant(0.5), constant(0.0)},
                    {CompiledVec2Parameter{{}, document::Vec2d{}},
                     CompiledVec2Parameter{{}, document::Vec2d{1.0, 1.0}}},
                    0));
    // r2 the offset; r0 |d - offset|; r1 the band's coverage.
    program.instructions.push_back(
        instruction(FieldOp::Constant, 2, {-1, -1, -1, -1}, {},
                    {offset, emitter.reuse(offset), emitter.reuse(offset), emitter.reuse(offset)}));
    program.instructions.push_back(instruction(FieldOp::Math, 0, {1, 2, -1, -1},
                                               mathOp(core::field::FieldMathOperation::Difference),
                                               {constant(0.1), constant(0.0)}));
    program.instructions.push_back(
        instruction(FieldOp::Remap, 1, {0, -1, -1, -1}, {1, 1, 0, 0},
                    {low, high, constant(0.0), constant(1.0), constant(0.5), constant(0.5)}));
    // r2 the coverage in every lane; r0 the premultiplied colour; r1 their product.
    program.instructions.push_back(
        instruction(FieldOp::Swizzle, 2, {1, -1, -1, -1}, {0, 0, 0, 0},
                    {constant(1.0), constant(1.0), constant(1.0), constant(1.0)}));
    program.instructions.push_back(
        instruction(FieldOp::Constant, 0, {-1, -1, -1, -1}, {},
                    {premultiplied(0), premultiplied(1), premultiplied(2), emitter.reuse(alpha)}));
    program.instructions.push_back(instruction(FieldOp::Math, 1, {2, 0, -1, -1},
                                               mathOp(core::field::FieldMathOperation::Multiply),
                                               {constant(0.1), constant(0.0)}));
    program.result = 1;
    program.kind = static_cast<std::uint8_t>(core::field::FieldResultKind::Premultiplied);
    program.mode = static_cast<std::uint8_t>(core::field::FieldOutputMode::Raw);

    const auto distance = emitter.effect(input, MaskDistanceKernel{alphaContourReach(operands)});
    const auto contour = emitter.effect(distance, FieldRasterKernel{std::move(program), 1});
    emitter.setImages(contour, {distance});
    switch (operands.composite) {
    case AlphaContourComposite::ContourOnly:
        return contour;
    case AlphaContourComposite::Under:
        return emitter.merge(
            {{input, core::BlendMode::Normal}, {contour, core::BlendMode::Normal}});
    case AlphaContourComposite::Over:
        break;
    }
    return emitter.merge({{contour, core::BlendMode::Normal}, {input, core::BlendMode::Normal}});
}

// Inner Shadow. The input's alpha is blurred by `softness` (Matte Blur coverage, transparent halo),
// moved by the offset exactly as Drop Shadow's, and the shadow is what that coverage does NOT
// cover, inside the input: innerCoverage, then coloured and faded by opacity * colour alpha (Levels).
// An Atop Merge puts it on the input (co = shadow * a + input * (1 - shadow alpha), alpha a), which
// keeps the input's alpha and its premultiplication; Shadow Only composites it In the input instead
// (shadow * a, alpha shadow alpha * a).
OperationIndex appendInnerShadow(RecipeEmitter& emitter, const OperationIndex input,
                                 const InnerShadowOperands& operands) {
    const auto coverage = emitter.effect(
        input, SeparableBlurKernel{SeparableBlurKind::Gaussian, operands.softness,
                                   BlurEdgeMode::Transparent, BlurChannels::Coverage});
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
    const auto shaped = innerCoverage(emitter, input, moved, operands.choke);
    const auto colour = emitter.components(operands.color);
    const auto alpha = emitter.scalar(ScalarPrimitive::Multiply, {operands.opacity, colour[3]});
    const auto shadow =
        innerPaint(emitter, shaped, {colour[0], colour[1], colour[2]}, alpha, false);
    return emitter.merge({{shadow, operands.shadowOnly ? core::BlendMode::In : core::BlendMode::Atop},
                          {input, core::BlendMode::Normal}});
}

// Inner Glow. The shaped inner coverage of a `size` blur with no offset (Edge: full at the alpha
// edge, fading inward), or its complement (Centre: nothing at the edge, full deep inside), coloured
// by colour * intensity (unclamped, so HDR glows keep their values) and faded by opacity * colour
// alpha, then composited Atop the input (In for Glow Only) like Inner Shadow.
OperationIndex appendInnerGlow(RecipeEmitter& emitter, const OperationIndex input,
                               const InnerGlowOperands& operands) {
    const auto coverage = emitter.effect(
        input, SeparableBlurKernel{SeparableBlurKind::Gaussian, operands.size,
                                   BlurEdgeMode::Transparent, BlurChannels::Coverage});
    const auto shaped = innerCoverage(emitter, input, coverage, operands.choke);
    const auto colour = emitter.components(operands.color);
    const auto alpha = emitter.scalar(ScalarPrimitive::Multiply, {operands.opacity, colour[3]});
    const auto glow = innerPaint(
        emitter, shaped,
        {emitter.scalar(ScalarPrimitive::Multiply, {colour[0], operands.intensity}),
         emitter.scalar(ScalarPrimitive::Multiply, {colour[1], operands.intensity}),
         emitter.scalar(ScalarPrimitive::Multiply, {colour[2], operands.intensity})},
        alpha, operands.centre);
    return emitter.merge({{glow, operands.glowOnly ? core::BlendMode::In : core::BlendMode::Atop},
                          {input, core::BlendMode::Normal}});
}

OperationIndex appendSharpen(RecipeEmitter& emitter, const OperationIndex input,
                             const SharpenOperands& operands) {
    const auto blurred =
        emitter.effect(input, SeparableBlurKernel{SeparableBlurKind::Gaussian, operands.radius,
                                                  BlurEdgeMode::Clamp, BlurChannels::Rgba});
    const auto sharpened = emitter.effect(
        input, SharpenKernel{operands.amount, operands.threshold, emitter.reuse(operands.radius)});
    emitter.setImages(sharpened, {blurred});
    return sharpened;
}

OperationIndex appendChannelBlur(RecipeEmitter& emitter, const OperationIndex input,
                                 const ChannelBlurOperands& operands) {
    const auto blurAt = [&](const CompiledScalarParameter& scale) {
        SeparableBlurKernel kernel{SeparableBlurKind::Gaussian,
                                   emitter.scalar(core::primitives::ScalarPrimitive::Multiply,
                                                  {operands.radius, scale}),
                                   operands.edge, BlurChannels::Rgba};
        if (operands.separate) {
            kernel.separate = true;
            kernel.radiusY = emitter.scalar(core::primitives::ScalarPrimitive::Multiply,
                                            {operands.radiusY, scale});
        }
        return kernel;
    };
    auto widest = operands.scales[0];
    for (std::size_t channel = 1; channel < operands.scales.size(); ++channel)
        widest = emitter.scalar(core::primitives::ScalarPrimitive::Maximum,
                                {widest, operands.scales[channel]});
    auto result = emitter.effect(input, blurAt(widest));
    constexpr std::array sources{ImageChannelSource::Red, ImageChannelSource::Green,
                                 ImageChannelSource::Blue, ImageChannelSource::Alpha};
    for (std::uint8_t channel = 0; channel < 4; ++channel) {
        const auto blurred = emitter.effect(input, blurAt(operands.scales[channel]));
        result = emitter.effect(result, ChannelCombineKernel{channel, sources[channel], false},
                                blurred);
    }
    return result;
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

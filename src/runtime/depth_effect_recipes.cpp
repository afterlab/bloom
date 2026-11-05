#include "depth_effect_recipes.hpp"

#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

OperationIndex appendZDefocus(RecipeEmitter& emitter, const OperationIndex input,
                              const std::optional<OperationIndex> depth,
                              const std::optional<OperationIndex> bokeh,
                              ZDefocusOperands operands) {
    using Primitive = core::primitives::ScalarPrimitive;
    auto& gather = operands.gather;
    auto& map = operands.map;
    const auto mode = gather.mode;
    map.output = DepthMapOutput::CircleOfConfusion;
    map.mode = mode;
    // The map's size and maximum are the gather's (a second use of each operand).
    map.size = emitter.reuse(gather.size);
    map.maxSize = emitter.reuse(gather.maxSize);
    const bool sizeIsWidest =
        mode == document::kDefocusModeSizeMap || mode == document::kDefocusModeUniform;
    // The widest reach R (1 + chromatic / 4) and the prefilter, a box of the ring spacing at it,
    // R (1 + chromatic / 4) / K, of radius (spacing - 1) / 2.
    const auto widest = emitter.reuse(sizeIsWidest ? gather.size : gather.maxSize);
    const auto red = emitter.scalar(
        Primitive::Add,
        {RecipeEmitter::constant(1.0),
         emitter.scalar(Primitive::Multiply,
                        {emitter.reuse(gather.chromatic), RecipeEmitter::constant(0.25)})});
    const auto reach = emitter.scalar(Primitive::Multiply, {widest, red});
    const auto quality = std::clamp<std::int64_t>(gather.quality, document::kDefocusQualityDraft,
                                                  document::kDefocusQualityMaximum);
    const auto rings =
        static_cast<double>(document::kDefocusQualityRings[static_cast<std::size_t>(quality)]);
    const auto boxRadius = emitter.scalar(
        Primitive::Maximum,
        {RecipeEmitter::constant(0.0),
         emitter.scalar(
             Primitive::Multiply,
             {emitter.scalar(Primitive::Subtract,
                             {emitter.scalar(Primitive::Divide, {reach, RecipeEmitter::constant(rings)}),
                              RecipeEmitter::constant(1.0)}),
              RecipeEmitter::constant(0.5)})});
    std::vector<OperationIndex> images;
    const auto slot = [&](const std::size_t index, const OperationIndex image) {
        gather.slotImages[index] = static_cast<std::int8_t>(images.size());
        images.push_back(image);
    };
    // The tile maximum: the map dilated in alpha with each pixel's own signed circle of confusion
    // carried in red, so the gather reads both from one image.
    if (mode != document::kDefocusModeUniform && depth) {
        const auto coc = emitter.effect(*depth, std::move(map));
        slot(kDefocusTileSlot,
             emitter.effect(coc, SeparableBlurKernel{
                                     SeparableBlurKind::Dilate,
                                     emitter.scalar(Primitive::Minimum,
                                                    {reach, RecipeEmitter::constant(
                                                                document::kMaximumBlurRadiusPixels)}),
                                     BlurEdgeMode::Transparent, BlurChannels::Rgba}));
    }
    slot(kDefocusPrefilterSlot,
         emitter.effect(input, SeparableBlurKernel{SeparableBlurKind::Box, boxRadius,
                                                   BlurEdgeMode::Transparent, BlurChannels::Rgba}));
    if (gather.shape == document::kBokehImage && bokeh)
        slot(kDefocusBokehSlot, *bokeh);
    const auto result = emitter.effect(input, std::move(gather));
    emitter.setImages(result, std::move(images));
    return result;
}

OperationIndex appendDepthFog(RecipeEmitter& emitter, const OperationIndex input,
                              const OperationIndex depth, const std::optional<OperationIndex> fog,
                              DepthFogOperands operands) {
    using Source = ImageChannelSource;
    operands.map.output = DepthMapOutput::Fog;
    const auto factor = emitter.effect(depth, std::move(operands.map));
    // The fog layer: the fog image or colour over the input's coverage, or (filling empty areas)
    // over the whole depth window.
    auto layer = input;
    if (fog) {
        layer = operands.fillEmpty
                    ? *fog
                    : emitter.effect(*fog,
                                     ImageCombineKernel{ImageCombineMode::SetAlpha, std::nullopt,
                                                        render::ImageCoverageChannel::Alpha},
                                     input);
    } else {
        ColorizeKernel colorize;
        colorize.mode = ColorizeMode::Fill;
        colorize.amount = RecipeEmitter::constant(1.0);
        colorize.color = operands.color;
        const auto base =
            operands.fillEmpty
                ? emitter.effect(factor, ChannelSelectKernel{{Source::One, Source::One,
                                                              Source::One, Source::One}})
                : input;
        layer = emitter.effect(base, std::move(colorize));
    }
    return emitter.effect(input,
                          ImageCombineKernel{ImageCombineMode::Mix, RecipeEmitter::constant(1.0),
                                             render::ImageCoverageChannel::Alpha},
                          layer, factor);
}

} // namespace bloom::runtime::detail

#include <algorithm>
#include <bloom/runtime/compiled_plan.hpp>
#include <optional>
#include <type_traits>

namespace bloom::runtime {
void CompiledCompositionPlan::analyzeTimeDependence() {
    std::vector<std::uint8_t> outputs(valueOutputCount_, 1);
    valueTimeDependent_.reserve(valueOperations_.size());
    for (const auto& operation : valueOperations_) {
        // A Time node, and the one readout whose value is the FRAME rather than the composition.
        // The other three readouts were baked into constants by the compiler and never reach here.
        const auto* utility = std::get_if<CompiledValueUtility>(&operation.kernel);
        const auto* sample = std::get_if<CompiledValueGradientSample>(&operation.kernel);
        // #525: a Field Sample whose field reads the clock or a keyed curve.
        const auto* field = std::get_if<CompiledValueFieldSample>(&operation.kernel);
        bool fieldAnimated = field != nullptr && field->program != nullptr &&
                             field->program->usesTime;
        if (field != nullptr && field->program != nullptr)
            forEachFieldOperand(*field->program, [&](const auto& operand) {
                using Operand = std::decay_t<decltype(operand)>;
                if constexpr (std::is_same_v<Operand, CompiledPathParameter>)
                    fieldAnimated = fieldAnimated ||
                                    std::holds_alternative<CompiledPathCurve>(operand.source);
            });
        bool dependent = fieldAnimated ||
            std::holds_alternative<CompiledValueTime>(operation.kernel) ||
            std::holds_alternative<CompiledValueShake>(operation.kernel) ||
            (sample != nullptr &&
             std::holds_alternative<CompiledGradientCurve>(sample->gradient)) ||
            std::holds_alternative<CompiledBoundsReadout>(operation.kernel) ||
            (utility != nullptr && utility->operation == document::ValueUtilityKernel::FrameNumber);
        forEachValueOperand(operation.kernel, [&](const CompiledValueOperand& operand) {
            if (const auto* input = std::get_if<ValueOutputIndex>(&operand.source))
                dependent =
                    dependent || input->value() >= outputs.size() || outputs[input->value()] != 0;
            else
                dependent = dependent || !std::holds_alternative<CompiledValue>(operand.source);
        });
        valueTimeDependent_.push_back(dependent ? 1 : 0);
        for (std::size_t slot = 0; slot < operation.outputCount; ++slot) {
            const auto index = operation.firstOutput.value();
            if (index < outputs.size() && slot < outputs.size() - index)
                outputs[index + slot] = dependent ? 1 : 0;
        }
    }
    const auto parameter = [&](const auto& operand) {
        if (const auto* input = std::get_if<ValueOutputIndex>(&operand.source))
            return input->value() >= outputs.size() || outputs[input->value()] != 0;
        return operand.source.index() != 0;
    };
    // Task DRIVE-1. A parameter whose only source is a driver: a String, an Integer or a Boolean
    // has no curve alternative, so "does this vary with time" is entirely the question of whether
    // the value output behind it does.
    const auto driven = [&](const std::optional<ValueOutputIndex>& output) {
        return output.has_value() &&
               (output->value() >= outputs.size() || outputs[output->value()] != 0);
    };
    // #541: a keyed gradient varies; #543: so does one whose driven stops do.
    const auto gradient = [&](const CompiledGradientParameter& operand) {
        return std::holds_alternative<CompiledGradientCurve>(operand.source) ||
               std::ranges::any_of(operand.drivers, [&](const CompiledGradientStopDriver& driver) {
                   return driven(driver.value);
               });
    };
    const auto input = [&](OperationIndex index) {
        return index.value() >= operationTimeDependent_.size() ||
               operationTimeDependent_[index.value()] != 0;
    };
    operationTimeDependent_.reserve(operations_.size());
    for (const auto& operation : operations_) {
        const bool dependent = std::visit(
            [&](const auto& step) {
                using Step = std::decay_t<decltype(step)>;
                if constexpr (std::is_same_v<Step, CompiledSolid>)
                    return parameter(step.color) || parameter(step.width) ||
                           parameter(step.height) ||
                           (step.gradient &&
                            (gradient(step.gradient->stops) || parameter(step.gradient->start) ||
                             parameter(step.gradient->end)));
                else if constexpr (std::is_same_v<Step, CompiledChecker>)
                    return parameter(step.colorA) || parameter(step.colorB);
                else if constexpr (std::is_same_v<Step, CompiledShape>)
                    return parameter(step.size) || parameter(step.fillColor) ||
                           parameter(step.strokeColor) || parameter(step.strokeWidth) ||
                           driven(step.drivenFillEnabled) || driven(step.drivenStrokeEnabled);
                else if constexpr (std::is_same_v<Step, CompiledCompositionSource> ||
                                   std::is_same_v<Step, CompiledVideoSource>)
                    return true;
                else if constexpr (std::is_same_v<Step, CompiledImageSource>)
                    return step.asset && step.asset->kind == document::AssetKind::Sequence;
                else if constexpr (std::is_same_v<Step, CompiledChannelShuffle> ||
                                   std::is_same_v<Step, CompiledCryptomatte>)
                    return std::ranges::any_of(step.channels.sources, [](const auto& source) {
                        return source.asset && source.asset->kind == document::AssetKind::Sequence;
                    });
                else if constexpr (std::is_same_v<Step, CompiledImageEffect>) {
                    if (input(step.input) ||
                        (step.secondaryInput && input(*step.secondaryInput)) ||
                        (step.maskInput && input(*step.maskInput)) ||
                        std::ranges::any_of(step.fieldImages, input))
                        return true;
                    if (step.bypass)
                        return false;
                    if (const auto* pointwise = std::get_if<PointwiseImageKernel>(&step.kernel))
                        return pointwise->amount && parameter(*pointwise->amount);
                    if (const auto* canvas = std::get_if<CanvasImageKernel>(&step.kernel))
                        return std::ranges::any_of(canvas->margins, parameter);
                    if (const auto* combine = std::get_if<ImageCombineKernel>(&step.kernel))
                        return (combine->factor && parameter(*combine->factor)) ||
                               driven(combine->drivenChannel);
                    if (const auto* blur = std::get_if<SeparableBlurKernel>(&step.kernel))
                        return parameter(blur->radius);
                    if (const auto* key = std::get_if<LumaKeyKernel>(&step.kernel))
                        return parameter(key->low) || parameter(key->high);
                    if (const auto* directional = std::get_if<DirectionalBlurKernel>(&step.kernel))
                        return parameter(directional->angle) || parameter(directional->spacing);
                    // #484: jittered samples are hashed with the frame index.
                    if (const auto* path = std::get_if<MotionPathBlurKernel>(&step.kernel);
                        path != nullptr && path->shutter.jitter)
                        return true;
                    if (const auto* vector = std::get_if<VectorBlurKernel>(&step.kernel);
                        vector != nullptr && vector->shutter.jitter)
                        return true;
                    if (isColorGradeKernel(step.kernel)) {
                        // #525: a field that reads the clock (a looping noise, a moving pattern).
                        const auto* field = std::get_if<FieldRasterKernel>(&step.kernel);
                        bool animated = field != nullptr && field->program.usesTime;
                        // #486: grain drawn anew for every frame.
                        if (const auto* grain = std::get_if<FilmGrainKernel>(&step.kernel))
                            animated = animated || grain->animation != document::kFilmGrainHeld;
                        forEachColorGradeOperand(step.kernel, [&](const auto& operand) {
                            using Operand = std::decay_t<decltype(operand)>;
                            if constexpr (std::is_same_v<Operand, CompiledPathParameter>)
                                animated = animated ||
                                           std::holds_alternative<CompiledPathCurve>(operand.source);
                            else if constexpr (std::is_same_v<Operand, CompiledGradientParameter>)
                                animated = animated || gradient(operand);
                            else
                                animated = animated || parameter(operand);
                        });
                        return animated;
                    }
                    if (const auto* transform = std::get_if<Transform2DKernel>(&step.kernel))
                        return parameter(transform->translate) || parameter(transform->pivot) ||
                               parameter(transform->scale) || parameter(transform->rotation);
                    if (const auto* masks = std::get_if<LayerMaskKernel>(&step.kernel))
                        return std::ranges::any_of(masks->masks, [&](const auto& mask) {
                            return std::holds_alternative<CompiledPathCurve>(mask.path.source) ||
                                   parameter(mask.feather) || parameter(mask.expansion) ||
                                   parameter(mask.opacity);
                        });
                    return false;
                } else if constexpr (std::is_same_v<Step, CompiledText>)
                    return parameter(step.color) || parameter(step.size) ||
                           driven(step.drivenContent) ||
                           (parameter(step.layout.lineHeight) ||
                            parameter(step.layout.letterSpacing) ||
                            driven(step.layout.drivenAlignment));
                // A driven blend mode is consumed by the Layer Stack rather than by this step, but
                // it is a property of the LAYER, so the layer's operation is what varies with time
                // -- and the stack's own dependence already follows its inputs.
                else if constexpr (std::is_same_v<Step, CompiledLayerOutput>)
                    return input(step.input) || (step.parent && input(*step.parent)) ||
                           parameter(step.position) || parameter(step.anchor) ||
                           parameter(step.scale) || parameter(step.rotation) ||
                           parameter(step.opacity) || driven(step.drivenBlendMode);
                else if constexpr (std::is_same_v<Step, CompiledMerge>)
                    return std::ranges::any_of(
                        step.entries, [&](const auto& entry) { return input(entry.input); });
                else
                    return input(step.input);
            },
            operation);
        operationTimeDependent_.push_back(dependent ? 1 : 0);
    }
}
} // namespace bloom::runtime

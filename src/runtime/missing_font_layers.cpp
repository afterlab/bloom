#include <bloom/runtime/missing_font_layers.hpp>

#include "plan_time_scopes.hpp"

namespace bloom::runtime {

std::vector<bool> layersUsingMissingFont(const std::span<const CompiledOperation> operations) {
    // `reaches[i]`: operation i's picture contains a Text with a missing font. Operations are in
    // topological order (an input precedes its consumer), so one sweep settles every entry.
    std::vector<bool> reaches(operations.size(), false);
    std::vector<bool> layers(operations.size(), false);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        if (const auto* text = std::get_if<CompiledText>(&operation)) {
            reaches[index] = text->fontMissing;
            continue;
        }
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
            const auto input = layer->input.value();
            layers[index] = input < index && reaches[input];
            // Other layers (an adjustment reading the stack below, a matte) do not use the font.
            continue;
        }
        detail::forEachOperationInput(operation, [&](const OperationIndex input) {
            if (input.value() < index && reaches[input.value()])
                reaches[index] = true;
        });
    }
    return layers;
}

} // namespace bloom::runtime

#pragma once

// Private to src/runtime. #483 Z Defocus and Depth Fog as chains of operations, shared by the
// snapshot compiler (the production caller), the GPU coverage fixtures and the tests, so the
// evidence covers the production chains. Z Defocus is its depth map (DepthMapKernel), the map's
// tile maximum (a Dilate of its coverage), the input box-prefiltered at the widest ring spacing
// and the gather (DefocusKernel); Depth Fog is its fog factor (DepthMapKernel) as the Mask of an
// Image Mix of the input with the fog layer. Values derived from the authored ones (the tile and
// prefilter radii) are value-graph operations. docs/architecture/image-filters.md, "Depth
// effects", is the specification.

#include "compositing_recipes.hpp"

#include <optional>

namespace bloom::runtime::detail {

struct ZDefocusOperands final {
    // The circle of confusion map's reading; its size and maximum are the gather's.
    DepthMapKernel map;
    DefocusKernel gather;
};
// Z Defocus of `input`; `depth` is absent only in Uniform mode, `bokeh` is the bokeh image (read
// with the Image shape).
[[nodiscard]] OperationIndex appendZDefocus(RecipeEmitter& emitter, OperationIndex input,
                                            std::optional<OperationIndex> depth,
                                            std::optional<OperationIndex> bokeh,
                                            ZDefocusOperands operands);

struct DepthFogOperands final {
    DepthMapKernel map;
    CompiledColorParameter color{{}, core::Color4d{0.75, 0.8, 0.85, 1.0}};
    // Fog the input's transparent areas too (the fog layer covers the whole depth window).
    bool fillEmpty = false;
};
// Depth Fog of `input` by `depth`; `fog` replaces the fog colour with an image.
[[nodiscard]] OperationIndex appendDepthFog(RecipeEmitter& emitter, OperationIndex input,
                                            OperationIndex depth, std::optional<OperationIndex> fog,
                                            DepthFogOperands operands);

} // namespace bloom::runtime::detail

#pragma once

#include <bloom/runtime/compiled_plan.hpp>

#include <span>
#include <vector>

namespace bloom::runtime {

// For each compiled operation, whether it is a Layer Output whose picture is built, through any
// chain of effects, merges or other image operations, from a Text drawn in a fallback face because
// its font is missing (docs/architecture/fonts.md). Entries for other operations are false. A Layer
// Output only reads its content input here: a transform parent or another layer's picture does not
// make it a layer using the missing font. The Viewer's missing-font overlay uses this and draws it
// over the layer's bounds; it is never part of a rendered frame.
[[nodiscard]] std::vector<bool> layersUsingMissingFont(std::span<const CompiledOperation> operations);

} // namespace bloom::runtime

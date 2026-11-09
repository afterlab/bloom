#pragma once

// Private to src/runtime. #349 Histogram as the chain of kernels the snapshot compiler lowers it to,
// shared by the compiler (the production caller), the GPU coverage fixtures, the native parity
// harness and the tests, so the evidence covers the production chain. docs/architecture/
// image-filters.md, "Histogram", is the specification.

#include "compositing_recipes.hpp"

#include <array>
#include <cstdint>

namespace bloom::runtime::detail {

// The tiles of the counting pass: a frame is cut into at most this many tiles across and down, each
// counted on its own (the same tiling as Auto Exposure's first reduction).
inline constexpr std::int64_t kHistogramTileGrid = 32;

struct HistogramOperands final {
    std::int64_t channels = document::kHistogramRgb;
    bool straight = true;
    bool logarithmic = false;
    std::int64_t bins = 256;
    std::int64_t height = 128;
    // The range's minimum and maximum.
    std::array<CompiledScalarParameter, 2> values{};
};

// Bin, Sum and Plot over `input`: the plot is the chain's result. A frame reads nothing but its own
// pixels, so there is no state to carry from one frame to the next.
[[nodiscard]] OperationIndex appendHistogram(RecipeEmitter& emitter, OperationIndex input,
                                             const HistogramOperands& operands);

} // namespace bloom::runtime::detail

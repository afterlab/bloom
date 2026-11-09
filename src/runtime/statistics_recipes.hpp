#pragma once

// Private to src/runtime. #349 Image Statistics as the chain of kernels the snapshot compiler lowers
// the node's tap plan to, shared by the compiler (the production caller), the GPU coverage fixtures,
// the native parity harness and the tests, so the evidence covers the production chain.
// docs/architecture/image-statistics.md is the specification.

#include "compositing_recipes.hpp"

#include <cstdint>

namespace bloom::runtime::detail {

// The tiles of the reducing pass: a frame is cut into at most this many tiles across and down (the
// same tiling as Auto Exposure's and Histogram's first reductions).
inline constexpr std::int64_t kStatisticsTileGrid = 32;

struct StatisticsOperands final {
    std::int64_t channel = 0;
    bool straight = true;
    bool opaqueOnly = false;
};

// Reduce and Sum over `input`: the sum is the chain's result, a two-pixel image.
[[nodiscard]] OperationIndex appendStatistics(RecipeEmitter& emitter, OperationIndex input,
                                              const StatisticsOperands& operands);

} // namespace bloom::runtime::detail

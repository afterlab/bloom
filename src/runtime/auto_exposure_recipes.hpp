#pragma once

// Private to src/runtime. #566 Auto Exposure as the chain of kernels the snapshot compiler lowers it
// to, shared by the compiler (the production caller), the GPU coverage fixtures, the native parity
// harness and the tests, so the evidence covers the production chain. docs/architecture/
// image-filters.md, "Auto Exposure", is the specification.

#include "compositing_recipes.hpp"

#include <array>
#include <cstdint>

namespace bloom::runtime::detail {

// The tiles of the first reduction pass: a frame is cut into at most this many tiles across and
// down, each summed on its own.
inline constexpr std::int64_t kAutoExposureTileGrid = 64;

struct AutoExposureOperands final {
    std::int64_t lookBackFrames = 48;
    // The node's scalars in document::autoExposureScalarIndex order.
    std::array<CompiledScalarParameter, document::kAutoExposureScalarCount> values{};
};

// The look-back window of `lookBackFrames` frames up to the current: each frame's mean colour by two
// reduction passes, the recursive filter over those means, and the gain applied to the current
// frame. A frame never reads state a previous render left, and the reductions of a source time are
// shared by every frame whose window reaches it. Empty when the window is out of bounds.
[[nodiscard]] std::optional<OperationIndex> appendAutoExposure(RecipeEmitter& emitter,
                                                               OperationIndex input,
                                                               const AutoExposureOperands& operands);

} // namespace bloom::runtime::detail

#pragma once

// Private to src/runtime. #563 Codec Degrade as the chain of kernels the snapshot compiler lowers it
// to, shared by the compiler (the production caller), the GPU coverage fixtures, the native parity
// harness and the tests, so the evidence covers the production chain. docs/architecture/
// image-filters.md, "Codec Degrade", is the specification.

#include "compositing_recipes.hpp"

#include <cstdint>

namespace bloom::runtime::detail {

struct CodecDegradeOperands final {
    std::int64_t blockPixels = 8;
    std::int64_t gopFrames = 12;
    bool chromaBleed = true;
    CompiledScalarParameter quality;
    CompiledScalarParameter persistence;
};

// The look-back chain: for every frame, the group of pictures from the last keyframe up to the frame
// (at most `gopFrames` pictures, so never more than that many frames back), each picture coded
// against the one before it with the block motion of the optical flow between them, then the last
// picture back to scene-linear colour. A frame never reads state a previous render left: the chain
// is rebuilt from the keyframe's source frame by the same operations whatever way the frame is
// reached.
[[nodiscard]] OperationIndex appendCodecDegrade(RecipeEmitter& emitter, OperationIndex input,
                                                const CodecDegradeOperands& operands);

} // namespace bloom::runtime::detail

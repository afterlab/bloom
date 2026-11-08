#pragma once

// Private to src/runtime. #566 the look-back window: the one fixed, bounded set of earlier frames a
// node reads in place of any state carried from playback, so a frame renders on its own and the
// same in preview, scrubbing, backward play and export. Auto Exposure is its first user; Temporal
// Denoise (#514) and Flow Retime (#522) read the frames before and after their own the same way,
// through time samples of their input on the plan's frame grid.

#include "compositing_recipes.hpp"

#include <cstdint>
#include <vector>

namespace bloom::runtime::detail {

// The most frames a look-back window may span, including the current one.
inline constexpr std::int64_t kMaximumLookBackFrames = 64;

// The images of `input` on the `frames` frames up to and including the current one, oldest first.
// Sample i is the frame `max(frame - (frames - 1 - i), 0)` of the plan's frame rate, evaluated by a
// time sample of `input`, so a frame before the composition's first repeats its first; the last is
// `input` itself, which needs no sample. Sharing is by cache identity: the same source time reached
// from two frames is one result. Empty when `frames` is outside [1, kMaximumLookBackFrames].
[[nodiscard]] std::vector<OperationIndex>
appendLookBackSamples(RecipeEmitter& emitter, OperationIndex input, std::int64_t frames);

} // namespace bloom::runtime::detail

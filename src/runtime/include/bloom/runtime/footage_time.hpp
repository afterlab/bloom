#pragma once

// Interpret Footage (#381): which frame of timed footage a source shows at a time. One rule for
// image sequences, video, their read-ahead and the CPU and GPU routes.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <optional>

namespace bloom::runtime::detail {

// `time` minus `start` composition frames at `rate`, exactly; nullopt when unrepresentable.
[[nodiscard]] std::optional<core::RationalTime>
shiftedByFrames(core::RationalTime time, std::int64_t start, document::FrameRate rate);

// The frame (0-based, within one pass of the footage) a source shows at `time`, its operand time:
// the node's start frame (composition frames) and loop mode (0 hold, 1 loop, 2 ping-pong) over the
// footage's whole interpreted length, the asset's Loop count, and its assumed or conformed rate. A
// sequence that matches the composition advances one member per composition frame. Before the
// start the first frame holds. Nullopt for footage with no frames or an unrepresentable time.
[[nodiscard]] std::optional<std::uint64_t> footageFrameIndex(const document::AssetRecord& asset,
                                                             core::RationalTime time,
                                                             std::int64_t startFrame,
                                                             std::int64_t loopMode,
                                                             document::FrameRate compositionRate);

// `time` (an audio clip's source-clock time) under `footage`, relative to `start`; nullopt past
// the last pass (silence) or when unrepresentable.
[[nodiscard]] std::optional<core::RationalTime> footageAudioTime(const FootageAudioTiming& footage,
                                                                 core::RationalTime start,
                                                                 core::RationalTime time);

} // namespace bloom::runtime::detail

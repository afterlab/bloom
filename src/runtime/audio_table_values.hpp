#pragma once

// #632 to #635: the outputs of a table node (Audio Bands, Audio Sample, Audio Features, Audio
// Beat, Audio Pitch) at one time, from the readings audio_table_analysis.hpp prepares. On a
// worker inside a frame's preparation the readings are prepared here; anywhere else only what is
// already prepared is read.

#include <bloom/core/rational_time.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>

#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct AudioTableValues final {
    // One value per output of the node, in the definition's order: a double or a number list.
    // Always filled; silence when the reading is not there.
    std::vector<CompiledValue> outputs;
    // Why the outputs are silence, when they are and it is worth saying; empty otherwise.
    std::string summary;
    std::string detail;
};

// `operands` are the node's Scalar parameters as resolved for this evaluation, in definition
// order.
[[nodiscard]] AudioTableValues audioTableValues(const CompiledValueAudioTable& kernel,
                                                std::span<const double> operands,
                                                core::RationalTime time);

// The outputs of a node whose audio input carries nothing.
[[nodiscard]] std::vector<CompiledValue> silentAudioTableOutputs(AudioTableNode node);

} // namespace bloom::runtime::detail

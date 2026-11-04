#pragma once

// #480: the time each source of a channel expression is read at. A file source reached through
// Composition Source `channels` outputs (CompiledChannelSource::nesting) is read at the mapped
// nested time -- each precomp's offset, scale and loop, with its animated operands sampled on the
// clock of the plan that owns them -- and the innermost nested plan's frame rate. Every other source
// is read at the reading operation's time and its plan's rate. Shared by the CPU reference, GPU
// scene preparation, the layered output and the channel listings, so every route reads the same
// frames (and so builds the same source cache identities).

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/channel_expression.hpp>

#include <string>
#include <vector>

namespace bloom::runtime {
class CompiledCompositionPlan;
} // namespace bloom::runtime

namespace bloom::runtime::detail {

struct ResolvedEvaluation;

struct ChannelSourceTime final {
    core::RationalTime time{};
    document::FrameRate rate = document::FrameRate::framesPerSecond24();
};

// The plan holding the expression and, when the caller has already resolved the reading
// operation's clock, that resolution (otherwise the plan is sampled at the reading time). Without a
// plan a nested source cannot be read.
struct ChannelSourceClock final {
    const CompiledCompositionPlan* plan = nullptr;
    const ResolvedEvaluation* resolved = nullptr;
};

struct ChannelSourceTimes final {
    // One entry per source of the expression.
    std::vector<ChannelSourceTime> times;
    std::string error;
    bool cancelled = false;
    [[nodiscard]] bool ok() const noexcept { return error.empty() && !cancelled; }
};

[[nodiscard]] ChannelSourceTimes
resolveChannelSourceTimes(const CompiledChannelExpression& expression, core::RationalTime time,
                          document::FrameRate rate, const ChannelSourceClock& clock,
                          const CancellationToken& cancel);

} // namespace bloom::runtime::detail

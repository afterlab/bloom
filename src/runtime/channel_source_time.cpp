#include "channel_source_time.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"

#include <bloom/runtime/composition_time.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace bloom::runtime::detail {

ChannelSourceTimes resolveChannelSourceTimes(const CompiledChannelExpression& expression,
                                             const core::RationalTime time,
                                             const document::FrameRate rate,
                                             const ChannelSourceClock& clock,
                                             const CancellationToken& cancel) {
    ChannelSourceTimes result;
    result.times.assign(expression.sources.size(), ChannelSourceTime{time, rate});
    if (std::ranges::none_of(expression.sources,
                             [](const auto& source) { return !source.nesting.empty(); }))
        return result;
    if (clock.plan == nullptr) {
        result.error = "A precomp's channels cannot be read without their composition";
        return result;
    }
    // The reading operation's own resolution, sampled once when the caller has none.
    std::optional<ResolvedEvaluation> sampledTop;
    // Sources reached through the same precomps share one mapping.
    std::vector<std::pair<const std::vector<CompiledCompositionSource>*, ChannelSourceTime>> memo;
    for (std::size_t index = 0; index < expression.sources.size(); ++index) {
        const auto& nesting = expression.sources[index].nesting;
        if (nesting.empty())
            continue;
        if (const auto found = std::ranges::find_if(
                memo, [&](const auto& entry) { return *entry.first == nesting; });
            found != memo.end()) {
            result.times[index] = found->second;
            continue;
        }
        const CompiledCompositionPlan* plan = clock.plan;
        auto current = time;
        const ResolvedEvaluation* resolved = clock.resolved;
        std::optional<ResolvedEvaluation> sampled;
        for (std::size_t level = 0; level < nesting.size(); ++level) {
            if (cancel.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            if (level > 0 || resolved == nullptr) {
                bool cancelled = false;
                auto& target = level == 0 ? sampledTop : sampled;
                if (level > 0 || !target)
                    target = sampleParametersAt(*plan, current, cancel, cancelled);
                if (cancelled) {
                    result.cancelled = true;
                    return result;
                }
                if (!target) {
                    result.error = "A precomp's channel time cannot be resolved";
                    return result;
                }
                resolved = &*target;
            }
            const auto& mapping = nesting[level];
            if (mapping.nestedPlanIndex >= plan->nestedPlans().size() ||
                !plan->nestedPlans()[mapping.nestedPlanIndex]) {
                result.error = "A precomp's channel source references no nested composition";
                return result;
            }
            const auto& nested = plan->nestedPlans()[mapping.nestedPlanIndex];
            const auto offset = resolveParameter(mapping.timeMapping.offset, *plan, *resolved);
            const auto scale = resolveParameter(mapping.timeMapping.scale, *plan, *resolved);
            const auto mapped =
                offset && scale && mapping.timeMapping.loopMode >= 0 &&
                        mapping.timeMapping.loopMode <= 2
                    ? mapCompositionTime(current, offset->value, scale->value, nested->duration(),
                                         nested->format().frameRate(), mapping.timeMapping.loopMode)
                    : std::nullopt;
            if (!mapped) {
                result.error = "Composition source time cannot be represented";
                return result;
            }
            current = *mapped;
            plan = nested.get();
        }
        result.times[index] = ChannelSourceTime{current, plan->format().frameRate()};
        memo.emplace_back(&nesting, result.times[index]);
    }
    return result;
}

} // namespace bloom::runtime::detail

#pragma once

// The Profiler panel's words for runtime enumerations (#423).

#include <bloom/runtime/profiler_snapshot.hpp>
#include <bloom/ui/profiler_history.hpp>

#include <QString>

namespace bloom::ui::profiler {

[[nodiscard]] QString text(const runtime::ProfilerName& name);
[[nodiscard]] QString poolKindText(runtime::ProfilerPoolKind kind);
[[nodiscard]] QString taskStateText(runtime::TaskState state);
[[nodiscard]] QString ownerKindText(runtime::TaskOwnerKind kind);
[[nodiscard]] QString executorText(runtime::TaskExecutor executor);
[[nodiscard]] QString stageText(runtime::ProfilerStage stage);
[[nodiscard]] QString evictionCauseText(runtime::ProfilerEvictionCause cause);
// "Evicted (pressure)": the short readout label for one cause.
[[nodiscard]] QString evictionRowText(runtime::ProfilerEvictionCause cause);
[[nodiscard]] QString eventText(const ProfilerHistory::Event& event);
// "12.5 s ago" for an event `ageNs` before the current sample.
[[nodiscard]] QString agoText(std::int64_t ageNs);

} // namespace bloom::ui::profiler

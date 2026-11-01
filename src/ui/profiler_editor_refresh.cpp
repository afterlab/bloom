// The Profiler panel's per-sample work (#423): copy the snapshot, push the history, and refresh
// the sections that are open. Readouts store numbers and format them when they paint; tables ask
// for their cells when they paint; charts read the rings when they paint.
#include <bloom/ui/profiler_editor.hpp>

#include "profiler_rows.hpp"
#include "profiler_table_model.hpp"
#include "profiler_text.hpp"

#include <bloom/ui/kit/chart.hpp>
#include <bloom/ui/kit/readout.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/profiler_history.hpp>

#include <QBrush>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

using namespace profiler;
using kit::MetricUnit;
using Series = ProfilerHistory::Series;

template <typename T> double number(const runtime::ProfilerMetric<T>& metric) {
    return metric.present ? static_cast<double>(metric.value) : std::nan("");
}

void setBytes(kit::KReadoutGrid& grid, const int row, const runtime::ProfilerBytes& value) {
    grid.setValue(row, number(value), MetricUnit::Bytes);
}

// A value from a slower source shows its age; absent values show none.
void setAged(kit::KReadoutGrid& grid, const int row, const runtime::ProfilerBytes& value,
             const runtime::ProfilerNanoseconds& age) {
    setBytes(grid, row, value);
    if (value.present && age.present)
        grid.setAge(row, age.value, age.value > kStaleAfterNs);
    else if (value.present)
        grid.setNote(row, {});
}

void setFlag(kit::KReadoutGrid& grid, const int row, const runtime::ProfilerMetric<bool>& flag) {
    if (!flag.present)
        grid.setAbsent(row);
    else
        grid.setText(row, flag.value ? ProfilerEditor::tr("Yes") : ProfilerEditor::tr("No"),
                     flag.value ? kit::Color::Warn : kit::Color::Foreground);
}

QVariant absentCell(const int role) {
    if (role == Qt::DisplayRole)
        return kit::absentMetricText();
    if (role == Qt::ForegroundRole)
        return QBrush(kit::color(kit::Color::Faint));
    return {};
}

template <typename T>
QVariant metricCell(const runtime::ProfilerMetric<T>& metric, const MetricUnit unit,
                    const int role) {
    if (!metric.present)
        return absentCell(role);
    return role == Qt::DisplayRole
               ? QVariant(kit::formatMetric(static_cast<double>(metric.value), unit))
               : QVariant{};
}

QVariant numberCell(const double value, const MetricUnit unit, const int role) {
    return role == Qt::DisplayRole ? QVariant(kit::formatMetric(value, unit)) : QVariant{};
}

double latest(const kit::SampleRing& ring) { return static_cast<double>(ring.latest()); }

bool expanded(const kit::KSection* section) {
    return section != nullptr && !section->isCollapsed();
}

} // namespace

void ProfilerEditor::profilerSnapshotArrived(const runtime::ProfilerSnapshot& snapshot) {
    // A sampler restart (the serial went back) starts the history again.
    if (snapshot.serial < current_->serial)
        history_->clear();
    *current_ = snapshot;
    history_->push(snapshot);
    auto& base = *stageBase_;
    for (std::size_t stage = 0; stage < base.size(); ++stage) {
        const auto& now = snapshot.workload.stages[stage];
        auto& start = base[stage];
        if (!start.present || !now.present || now.value.counts[0] < start.value.counts[0] ||
            history_->size() == 1)
            start = now;
    }
    refreshAll();
}

void ProfilerEditor::refreshAll() {
    refreshMemory();
    refreshGpu();
    refreshCpu();
    refreshWorkload();
    refreshPressure();
    refreshProcesses();
}

void ProfilerEditor::refreshMemory() {
    if (!expanded(memory_.section))
        return;
    const auto& snapshot = *current_;
    const auto& memory = snapshot.memory;
    const auto pools = std::min<std::size_t>(memory.poolCount, runtime::kProfilerMaximumPools);
    for (int slot = 0; slot < ProfilerHistory::kPoolSeries; ++slot)
        memory_.chart->setBandVisible(memory_.firstPoolBand + slot,
                                      static_cast<std::size_t>(slot) < pools);
    memory_.chart->setMarkerValue(memory_.baselineMarker, number(memory.idleBaselineBytes));
    memory_.chart->samplesChanged();

    auto& grid = *memory_.readouts;
    const auto& ledgerAge = snapshot.ledgerSampleAgeNs;
    setBytes(grid, RssRow, memory.rssBytes);
    setAged(grid, AnonRow, memory.anonBytes, ledgerAge);
    setAged(grid, FileRow, memory.fileBytes, ledgerAge);
    setAged(grid, ShmemRow, memory.shmemBytes, ledgerAge);

    // Growth across the visible history: a steady climb that does not come back is the leak
    // signal; the note says over how long.
    const auto& rss = history_->series(Series::Rss);
    double oldest = std::nan("");
    int oldestIndex = 0;
    for (; oldestIndex < rss.size() && std::isnan(oldest); ++oldestIndex)
        oldest = static_cast<double>(rss.at(oldestIndex));
    const double newest = latest(rss);
    const int spanSamples = rss.size() - oldestIndex;
    if (std::isnan(oldest) || std::isnan(newest) || spanSamples <= 0 || snapshot.intervalNs <= 0) {
        grid.setAbsent(GrowthRow);
    } else {
        grid.setValue(GrowthRow, newest - oldest, MetricUnit::Bytes);
        grid.setNote(GrowthRow, tr("over %1").arg(
                                    kit::formatMetric(static_cast<double>(spanSamples) *
                                                          static_cast<double>(snapshot.intervalNs),
                                                      MetricUnit::Duration)));
    }

    const auto unattributed = runtime::profilerUnattributedBytes(memory);
    grid.setValue(UnattributedRow, number(unattributed), MetricUnit::Bytes);
    const auto overBaseline = runtime::profilerUnattributedOverBaseline(memory);
    if (overBaseline.present)
        grid.setNote(UnattributedRow,
                     tr("%1 over idle baseline")
                         .arg(kit::formatMetric(static_cast<double>(overBaseline.value),
                                                MetricUnit::Bytes)));
    else if (unattributed.present)
        grid.setNote(UnattributedRow, {});
    setBytes(grid, IdleBaselineRow, memory.idleBaselineBytes);
    setBytes(grid, StagingRow, memory.stagingLiveBytes);
    if (memory.stagingLiveBytes.present && memory.stagingPeakBytes.present)
        grid.setNote(StagingRow,
                     tr("peak %1").arg(kit::formatMetric(
                         static_cast<double>(memory.stagingPeakBytes.value), MetricUnit::Bytes)));
    setBytes(grid, ClaimsRow, memory.admissionClaimBytes);
    // Counts and sums of arrays are measurements once a sample exists; before one, they are absent.
    if (snapshot.serial == 0) {
        grid.setAbsent(PoolsRow);
    } else {
        grid.setValue(PoolsRow, static_cast<double>(runtime::profilerPoolHostBytes(memory)),
                      MetricUnit::Bytes);
        grid.setNote(PoolsRow, pools == 1 ? tr("1 pool") : tr("%1 pools").arg(pools));
    }
    setBytes(grid, LedgerConfiguredRow, memory.ledgerConfiguredBytes);
    setBytes(grid, LedgerEffectiveRow, memory.ledgerEffectiveBytes);
    setBytes(grid, LedgerRetainedRow, memory.ledgerRetainedBytes);
    setBytes(grid, HeadroomRow, memory.headroomBytes);
    setBytes(grid, UsableRow, memory.usableBytes);
    setBytes(grid, PhysicalRow, memory.physicalBytes);
    setBytes(grid, AvailableRow, memory.availableBytes);
    setBytes(grid, SwapRow, memory.swapUsedBytes);
    if (memory.swapUsedBytes.present && memory.swapTotalBytes.present)
        grid.setNote(SwapRow,
                     tr("of %1").arg(kit::formatMetric(
                         static_cast<double>(memory.swapTotalBytes.value), MetricUnit::Bytes)));
    setFlag(grid, MemoryPressureRow, memory.memoryPressure);
    setFlag(grid, SwapPressureRow, memory.swapPressure);
    memory_.pools->refresh();
}

void ProfilerEditor::refreshGpu() {
    if (!expanded(gpu_.section))
        return;
    const auto& snapshot = *current_;
    const auto& gpu = snapshot.gpu;
    gpu_.memory->setMarkerValue(gpu_.budgetMarker, number(gpu.heapBudgetBytes));
    gpu_.memory->samplesChanged();
    gpu_.transfer->samplesChanged();
    auto& grid = *gpu_.readouts;
    setBytes(grid, GpuLiveRow, gpu.liveBytes);
    setBytes(grid, GpuRetiringRow, gpu.retiringBytes);
    setAged(grid, GpuHeapUsageRow, gpu.heapUsageBytes, snapshot.gpuBudgetQueryAgeNs);
    setAged(grid, GpuHeapBudgetRow, gpu.heapBudgetBytes, snapshot.gpuBudgetQueryAgeNs);
    // Claims against use: how much of what the driver reports in use is Bloom's own.
    if (gpu.liveBytes.present && gpu.heapUsageBytes.present && gpu.heapUsageBytes.value > 0)
        grid.setValue(GpuShareRow,
                      static_cast<double>(gpu.liveBytes.value + gpu.retiringBytes.valueOr(0)) /
                          static_cast<double>(gpu.heapUsageBytes.value),
                      MetricUnit::Ratio);
    else
        grid.setAbsent(GpuShareRow);
    grid.setValue(GpuUtilizationRow, number(gpu.utilization), MetricUnit::Ratio);
    const auto& history = *history_;
    grid.setValue(GpuUploadRow, latest(history.series(Series::GpuUploadRate)),
                  MetricUnit::BytesPerSecond);
    grid.setValue(GpuReadbackRow, latest(history.series(Series::GpuReadbackRate)),
                  MetricUnit::BytesPerSecond);
    grid.setValue(GpuSubmitsRow, latest(history.series(Series::GpuSubmitRate)),
                  MetricUnit::CountPerSecond);
    grid.setValue(GpuRefusalsRow, number(gpu.refusals), MetricUnit::Count);
    grid.setValue(GpuReclaimsRow, number(gpu.reclaims), MetricUnit::Count);
}

void ProfilerEditor::refreshCpu() {
    if (!expanded(cpu_.section))
        return;
    const auto& cpu = current_->cpu;
    const auto& history = *history_;
    const auto executors = std::min<std::size_t>(cpu.executorPoolCount, cpu.executorPools.size());
    for (int slot = 0; slot < ProfilerHistory::kExecutorSeries; ++slot)
        cpu_.executors->setLineVisible(slot, static_cast<std::size_t>(slot) < executors);
    cpu_.process->samplesChanged();
    cpu_.executors->samplesChanged();
    auto& grid = *cpu_.readouts;
    grid.setValue(ProcessCpuRow, latest(history.series(Series::ProcessCores)), MetricUnit::Cores);
    for (int slot = 0; slot < ProfilerHistory::kExecutorSeries; ++slot) {
        const int row = FirstExecutorRow + slot;
        const auto index = static_cast<std::size_t>(slot);
        grid.setRowVisible(row, index < executors);
        if (index >= executors)
            continue;
        const auto& pool = cpu.executorPools[index];
        grid.setLabel(row, text(pool.name));
        grid.setValue(row, latest(history.executor(slot)), MetricUnit::Ratio);
        grid.setNote(
            row, (pool.workers == 1 ? tr("%1 worker · %2 queued") : tr("%1 workers · %2 queued"))
                     .arg(pool.workers)
                     .arg(pool.queueDepth));
    }
}

void ProfilerEditor::refreshWorkload() {
    if (!expanded(workload_.section))
        return;
    const auto& workload = current_->workload;
    auto& tasks = *workload_.tasks;
    if (current_->serial == 0) {
        for (int row = 0; row < tasks.rowCount(); ++row)
            tasks.setAbsent(row);
    }
    for (std::size_t state = 0; state < runtime::kProfilerTaskStateCount && current_->serial != 0;
         ++state)
        tasks.setValue(static_cast<int>(state), workload.tasksByState[state], MetricUnit::Count);
    QStringList owners;
    for (std::size_t owner = 0; owner < runtime::kProfilerTaskOwnerKindCount; ++owner)
        if (workload.tasksByOwner[owner] > 0)
            owners.push_back(tr("%1 %2")
                                 .arg(ownerKindText(static_cast<runtime::TaskOwnerKind>(owner)))
                                 .arg(workload.tasksByOwner[owner]));
    if (current_->serial != 0)
        tasks.setText(OwnersRow,
                      owners.isEmpty() ? tr("None") : owners.join(QStringLiteral(" · ")));
    QStringList executors;
    for (std::size_t executor = 0; executor < runtime::kProfilerTaskExecutorCount; ++executor)
        if (workload.tasksByExecutor[executor] > 0)
            executors.push_back(tr("%1 %2")
                                    .arg(executorText(static_cast<runtime::TaskExecutor>(executor)))
                                    .arg(workload.tasksByExecutor[executor]));
    if (current_->serial != 0)
        tasks.setText(ExecutorsRow,
                      executors.isEmpty() ? tr("None") : executors.join(QStringLiteral(" · ")));
    workload_.taskModel->refresh();

    const auto& base = *stageBase_;
    for (std::size_t stage = 0; stage < runtime::kProfilerStageCount; ++stage) {
        auto* chart = workload_.stages[stage];
        const auto& now = workload.stages[stage];
        if (!now.present) {
            chart->setAbsent();
            continue;
        }
        std::array<std::uint64_t, runtime::kProfilerHistogramBucketCount> since{};
        for (std::size_t bucket = 0; bucket < since.size(); ++bucket) {
            const auto start = base[stage].present ? base[stage].value.counts[bucket] : 0;
            since[bucket] =
                now.value.counts[bucket] >= start ? now.value.counts[bucket] - start : 0;
        }
        chart->setCounts(since);
    }

    workload_.rates->samplesChanged();
    auto& grid = *workload_.readouts;
    grid.setValue(PlaybackRow, number(workload.playbackFps), MetricUnit::FramesPerSecond);
    grid.setValue(DroppedRow, number(workload.droppedFrames), MetricUnit::Count);
    grid.setValue(RamCachedRow, number(workload.ramPreviewCachedFrames), MetricUnit::Count);
    if (workload.ramPreviewCachedFrames.present)
        grid.setNote(RamCachedRow, tr("frames"));
    grid.setValue(RamFillRow, number(workload.ramPreviewFillFps), MetricUnit::FramesPerSecond);
    grid.setValue(ExportRow, number(workload.exportFps), MetricUnit::FramesPerSecond);
}

void ProfilerEditor::refreshPressure() {
    if (!expanded(pressure_.section))
        return;
    const auto& snapshot = *current_;
    const auto& pressure = snapshot.pressure;
    const auto& history = *history_;
    pressure_.chart->samplesChanged();
    auto& grid = *pressure_.readouts;
    setFlag(grid, PressureMemoryRow, snapshot.memory.memoryPressure);
    setFlag(grid, PressureSwapRow, snapshot.memory.swapPressure);
    grid.setValue(WaitsRow, number(pressure.admissionWaits), MetricUnit::Count);
    grid.setNoteValue(WaitsRow, latest(history.series(Series::AdmissionWaitRate)),
                      MetricUnit::CountPerSecond);
    grid.setValue(RefusalsRow, number(pressure.admissionRefusals), MetricUnit::Count);
    grid.setNoteValue(RefusalsRow, latest(history.series(Series::AdmissionRefusalRate)),
                      MetricUnit::CountPerSecond);
    for (std::size_t cause = 0; cause < runtime::kProfilerEvictionCauseCount; ++cause)
        setBytes(grid, FirstEvictionRow + static_cast<int>(cause), pressure.evictionBytes[cause]);
    grid.setValue(FirstEvictionRow + static_cast<int>(runtime::kProfilerEvictionCauseCount),
                  latest(history.series(Series::EvictionRate)), MetricUnit::BytesPerSecond);
    pressure_.events->refresh();
}

void ProfilerEditor::refreshProcesses() {
    if (!expanded(processes_.section))
        return;
    processes_.helpers->refresh();
}

QVariant ProfilerEditor::poolCell(const int row, const int column, const int role) const {
    const auto& pool = current_->memory.pools[static_cast<std::size_t>(row)];
    if (role == Qt::DecorationRole && column == PoolNameColumn)
        return kit::color(kit::chartSeriesColor(row));
    if (role == Qt::ToolTipRole && column == PoolNameColumn)
        return text(pool.name);
    switch (column) {
    case PoolNameColumn:
        return role == Qt::DisplayRole ? QVariant(text(pool.name)) : QVariant{};
    case PoolKindColumn:
        return role == Qt::DisplayRole ? QVariant(poolKindText(pool.kind)) : QVariant{};
    case PoolHostColumn:
        return numberCell(static_cast<double>(pool.hostBytes), MetricUnit::Bytes, role);
    case PoolPendingColumn:
        return numberCell(static_cast<double>(pool.pendingBytes), MetricUnit::Bytes, role);
    case PoolDeviceColumn:
        return numberCell(static_cast<double>(pool.deviceBytes), MetricUnit::Bytes, role);
    case PoolEntriesColumn:
        return numberCell(static_cast<double>(pool.entries), MetricUnit::Count, role);
    case PoolCeilingColumn:
        if (!pool.ceilingBytes.present)
            return role == Qt::DisplayRole ? QVariant(tr("None")) : QVariant{};
        return metricCell(pool.ceilingBytes, MetricUnit::Bytes, role);
    case PoolHitsColumn:
        return numberCell(static_cast<double>(pool.hits), MetricUnit::Count, role);
    case PoolMissesColumn:
        return numberCell(static_cast<double>(pool.misses), MetricUnit::Count, role);
    case PoolStoresColumn:
        return numberCell(static_cast<double>(pool.stores), MetricUnit::Count, role);
    case PoolEvictionsColumn:
        return numberCell(static_cast<double>(pool.evictions), MetricUnit::Count, role);
    case PoolRefusalsColumn:
        return numberCell(static_cast<double>(pool.refusals), MetricUnit::Count, role);
    default:
        return {};
    }
}

QVariant ProfilerEditor::taskCell(const int row, const int column, const int role) const {
    const auto& task = current_->workload.tasks[static_cast<std::size_t>(row)];
    switch (column) {
    case TaskNameColumn:
        return role == Qt::DisplayRole || role == Qt::ToolTipRole ? QVariant(text(task.name))
                                                                  : QVariant{};
    case TaskOwnerColumn:
        return role == Qt::DisplayRole ? QVariant(ownerKindText(task.ownerKind)) : QVariant{};
    case TaskExecutorColumn:
        return role == Qt::DisplayRole ? QVariant(executorText(task.executor)) : QVariant{};
    case TaskStateColumn:
        return role == Qt::DisplayRole ? QVariant(taskStateText(task.state)) : QVariant{};
    case TaskProgressColumn:
        if (!task.progress.present)
            return role == Qt::DisplayRole ? QVariant(tr("Indeterminate")) : QVariant{};
        return numberCell(static_cast<double>(task.progress.value), MetricUnit::Ratio, role);
    case TaskAgeColumn:
        return numberCell(static_cast<double>(task.ageNs), MetricUnit::Duration, role);
    default:
        return {};
    }
}

QVariant ProfilerEditor::helperCell(const int row, const int column, const int role) const {
    const auto& helper = current_->processes.helpers[static_cast<std::size_t>(row)];
    switch (column) {
    case HelperNameColumn:
        return role == Qt::DisplayRole ? QVariant(text(helper.name)) : QVariant{};
    case HelperPidColumn:
        return role == Qt::DisplayRole ? QVariant(QString::number(helper.pid)) : QVariant{};
    case HelperRssColumn:
        return metricCell(helper.rssBytes, MetricUnit::Bytes, role);
    case HelperCpuColumn:
        return metricCell(helper.cpuNs, MetricUnit::Duration, role);
    case HelperStateColumn:
        if (role == Qt::ForegroundRole && !helper.alive)
            return QBrush(kit::color(kit::Color::Muted));
        return role == Qt::DisplayRole ? QVariant(helper.alive ? tr("Running") : tr("Exited"))
                                       : QVariant{};
    default:
        return {};
    }
}

QVariant ProfilerEditor::eventCell(const int row, const int column, const int role) const {
    if (role != Qt::DisplayRole)
        return {};
    const auto& event = history_->event(row);
    if (column == EventWhenColumn)
        return agoText(std::max<std::int64_t>(0, current_->steadyTimestampNs - event.timestampNs));
    return eventText(event);
}

} // namespace bloom::ui

#pragma once

// The Profiler panel's readout rows (#423), in the order each grid adds them, so a row's index is
// its enumerator. Shared by the panel's construction and its refresh.

namespace bloom::ui::profiler {

enum MemoryRow : int {
    RssRow,
    AnonRow,
    FileRow,
    ShmemRow,
    GrowthRow,
    UnattributedRow,
    IdleBaselineRow,
    StagingRow,
    ClaimsRow,
    PoolsRow,
    LedgerConfiguredRow,
    LedgerEffectiveRow,
    LedgerRetainedRow,
    HeadroomRow,
    UsableRow,
    PhysicalRow,
    AvailableRow,
    SwapRow,
    ProcessSwapRow,
    MemoryPressureRow,
    SwapPressureRow,
};

enum GpuRow : int {
    GpuLiveRow,
    GpuRetiringRow,
    GpuHeapUsageRow,
    GpuHeapBudgetRow,
    GpuShareRow,
    GpuUtilizationRow,
    GpuUploadRow,
    GpuReadbackRow,
    GpuSubmitsRow,
    GpuRefusalsRow,
    GpuReclaimsRow,
};

// Then one row per executor pool slot.
enum CpuRow : int {
    ProcessCpuRow,
    FirstExecutorRow,
};

// Then the owner and executor summaries.
enum TaskRow : int {
    QueuedRow,
    RunningRow,
    SucceededRow,
    CancelledRow,
    FailedRow,
    OwnersRow,
    ExecutorsRow,
};

enum WorkloadRow : int {
    PlaybackRow,
    DroppedRow,
    RamCachedRow,
    RamFillRow,
    ExportRow,
};

enum PressureRow : int {
    PressureMemoryRow,
    PressureSwapRow,
    PressureSwapUsedRow,
    PressureProcessSwapRow,
    WaitsRow,
    RefusalsRow,
    // Then one row per runtime::ProfilerEvictionCause, then the eviction rate.
    FirstEvictionRow,
};

// Section columns of the tables.
enum PoolColumn : int {
    PoolNameColumn,
    PoolKindColumn,
    PoolHostColumn,
    PoolPendingColumn,
    PoolDeviceColumn,
    PoolEntriesColumn,
    PoolCeilingColumn,
    PoolHitsColumn,
    PoolMissesColumn,
    PoolStoresColumn,
    PoolEvictionsColumn,
    PoolEvictedColumn,
    PoolRefusalsColumn,
};

enum TaskColumn : int {
    TaskNameColumn,
    TaskOwnerColumn,
    TaskExecutorColumn,
    TaskStateColumn,
    TaskProgressColumn,
    TaskAgeColumn,
};

enum HelperColumn : int {
    HelperNameColumn,
    HelperPidColumn,
    HelperRssColumn,
    HelperCpuColumn,
    HelperStateColumn,
};

enum EventColumn : int {
    EventWhenColumn,
    EventTextColumn,
};

// A value from a slower source older than this is stale: twice the ledger's five-second sample.
inline constexpr long long kStaleAfterNs = 10'000'000'000LL;

} // namespace bloom::ui::profiler

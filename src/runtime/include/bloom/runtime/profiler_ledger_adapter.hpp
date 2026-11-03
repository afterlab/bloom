#pragma once

// The profiler's one read of the memory budget ledger (#423). Every figure the Profiler shows
// from the ledger goes through readProfilerLedger(), so a change to the ledger's accessors touches
// this one function. It only calls existing const accessors: it never samples the process, never
// mutates budgets and never triggers ledger sampling.

#include <bloom/runtime/profiler_snapshot.hpp>

namespace bloom::runtime {

class MemoryBudgetLedger;

// What the ledger exposes, as snapshot-shaped values. Trivially copyable, so the UI-side hook can
// publish it to the sampler through a seqlock.
struct ProfilerLedgerReading final {
    // The machine and the ledger's configuration and pressure state, from MemoryBudgetState.
    ProfilerBytes physicalBytes{};
    ProfilerBytes availableBytes{};
    ProfilerBytes swapTotalBytes{};
    ProfilerBytes swapUsedBytes{};
    ProfilerBytes headroomBytes{};
    ProfilerBytes usableBytes{};
    ProfilerBytes configuredBytes{};
    ProfilerBytes effectiveBytes{};
    ProfilerBytes retainedBytes{};
    ProfilerMetric<bool> memoryPressure{};
    ProfilerMetric<bool> swapPressure{};
    // When the ledger applied the machine sample these figures come from (steady-clock
    // nanoseconds); the sampler reports its age as ProfilerSnapshot::ledgerSampleAgeNs.
    ProfilerNanoseconds sampleSteadyNs{};
    // One row per registered pool (MemoryBudgetLedger::copyProfilerPools), and their sums.
    bool holdingPresent = false;
    std::uint64_t holdingHostBytes = 0;
    std::uint64_t holdingPendingBytes = 0;
    std::uint64_t holdingDeviceBytes = 0;
    std::uint32_t poolCount = 0;
    std::uint32_t droppedPools = 0;
    std::array<ProfilerPool, kProfilerMaximumPools> pools{};
    // Bytes admission has granted to running work, outside any pool (UsableCapacity's host claims
    // minus the retained preview frames they include); absent without an admission policy.
    ProfilerBytes admissionClaimBytes{};
};
static_assert(std::is_trivially_copyable_v<ProfilerLedgerReading>);

// Reads the ledger into `reading`. MemoryBudgetLedger::state() copies the whole process sample
// (strings and vectors), so it is read only when `refreshState` is set (after the ledger applied a
// sample); otherwise the state-derived fields keep their previous values. The pool rows ask every
// registered pool what it holds, so this runs on the thread that polls the ledger (the UI thread).
// It never allocates.
void readProfilerLedger(const MemoryBudgetLedger& ledger, ProfilerLedgerReading& reading,
                        bool refreshState);

// Copies a reading into the snapshot's memory section: machine and ledger figures, the pool rows
// and the admission claims.
void applyProfilerLedgerReading(const ProfilerLedgerReading& reading,
                                ProfilerMemory& memory) noexcept;

} // namespace bloom::runtime

#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/profiler_ledger_adapter.hpp>

#include <algorithm>

namespace bloom::runtime {
namespace {

ProfilerBytes bytes(const std::size_t value) { return ProfilerBytes::of(value); }
ProfilerBytes nonZero(const std::size_t value) {
    return value == 0 ? ProfilerBytes{} : bytes(value);
}

} // namespace

void readProfilerLedger(const MemoryBudgetLedger& ledger, ProfilerLedgerReading& reading,
                        const bool refreshState) {
    if (refreshState) {
        const auto state = ledger.state();
        reading.availableBytes =
            state.machine.availableBytes ? bytes(*state.machine.availableBytes) : ProfilerBytes{};
        // A zero swap total is a machine without swap or a platform that cannot report it.
        reading.swapTotalBytes = nonZero(state.machine.swapTotalBytes);
        reading.swapUsedBytes =
            reading.swapTotalBytes.present ? bytes(state.machine.swapUsedBytes) : ProfilerBytes{};
        reading.configuredBytes = bytes(state.configuredBytes);
        reading.effectiveBytes = bytes(state.effectiveBytes);
        reading.retainedBytes = bytes(state.retainedBytes);
        reading.memoryPressure = ProfilerMetric<bool>::of(state.memoryPressure);
        reading.swapPressure = ProfilerMetric<bool>::of(state.swapPressure);
    }
    reading.physicalBytes = nonZero(ledger.physicalByteBudget());
    const auto headroom = ledger.ramHeadroom();
    reading.headroomBytes = headroom ? bytes(*headroom) : ProfilerBytes{};
    // Usable = measured capacity minus the headroom; without a headroom there is no usable model.
    const auto capacity = ledger.usableByteBudget();
    reading.usableBytes =
        headroom ? bytes(capacity > *headroom ? capacity - *headroom : 0) : ProfilerBytes{};
    reading.poolCount =
        static_cast<std::uint32_t>(ledger.copyProfilerPools(reading.pools, reading.droppedPools));
    reading.holdingPresent = true;
    reading.holdingHostBytes = 0;
    reading.holdingPendingBytes = 0;
    reading.holdingDeviceBytes = 0;
    for (std::size_t index = 0; index < reading.poolCount; ++index) {
        reading.holdingHostBytes += reading.pools[index].hostBytes;
        reading.holdingPendingBytes += reading.pools[index].pendingBytes;
        reading.holdingDeviceBytes += reading.pools[index].deviceBytes;
    }
}

void applyProfilerLedgerReading(const ProfilerLedgerReading& reading,
                                ProfilerMemory& memory) noexcept {
    memory.physicalBytes = reading.physicalBytes;
    memory.availableBytes = reading.availableBytes;
    memory.swapTotalBytes = reading.swapTotalBytes;
    memory.swapUsedBytes = reading.swapUsedBytes;
    memory.headroomBytes = reading.headroomBytes;
    memory.usableBytes = reading.usableBytes;
    memory.ledgerConfiguredBytes = reading.configuredBytes;
    memory.ledgerEffectiveBytes = reading.effectiveBytes;
    memory.ledgerRetainedBytes = reading.retainedBytes;
    memory.memoryPressure = reading.memoryPressure;
    memory.swapPressure = reading.swapPressure;
    memory.admissionClaimBytes = reading.admissionClaimBytes;
    memory.poolCount = 0;
    memory.droppedPools = 0;
    if (!reading.holdingPresent)
        return;
    const auto count = std::min<std::size_t>(reading.poolCount, memory.pools.size());
    std::copy_n(reading.pools.begin(), count, memory.pools.begin());
    memory.poolCount = static_cast<std::uint32_t>(count);
    memory.droppedPools = reading.droppedPools;
}

} // namespace bloom::runtime

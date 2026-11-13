#pragma once

// A constant host memory reading for tests. A test that admits work against the real machine's
// free memory passes or fails with whatever else is running, so tests give admission this fixed
// reading instead: 16 GiB of physical memory with 8 GiB available, no process limit, no swap, no
// stall. The timestamp is the time of the call, so the reading always counts as fresh.
//
// It has the signature of GpuProcessingPhasePolicy::HostMemorySampler and converts to
// ui::MemoryBudgetController::Sampler:
//
//   policy.sampleHostMemory({}, bloom::test_support::fixedMemoryBudgetSample);
//   ui::MemoryBudgetController controller(scheduler, bridge, ledger,
//                                         bloom::test_support::fixedMemoryBudgetSample, nullptr,
//                                         std::nullopt); // no pressure monitor either
//   const auto state = bloom::test_support::applyFixedMemoryBudget(ledger);
//
// The device has the same kind of seam: GpuPreviewDisplayServiceOptions::deviceMemoryLimitBytes
// (render::GpuDeviceCreationOptions::device_memory_limit_bytes) plans for a card of a stated size.

#include <bloom/runtime/memory_budget_ledger.hpp>

#include <chrono>
#include <cstddef>

namespace bloom::test_support {

inline constexpr std::size_t kFixedPhysicalMemoryBytes = std::size_t{16} << 30U;
inline constexpr std::size_t kFixedAvailableMemoryBytes = std::size_t{8} << 30U;
// The card a small GPU test plans for, whatever the machine has and whatever else holds it.
inline constexpr std::uint64_t kFixedDeviceMemoryBytes = std::uint64_t{2} << 30U;

[[nodiscard]] inline runtime::MemoryBudgetSample
fixedMemoryBudgetSample(const runtime::CancellationToken& = {}) noexcept {
    runtime::MemoryBudgetSample reading;
    reading.physicalBytes = kFixedPhysicalMemoryBytes;
    reading.machine.availableBytes = kFixedAvailableMemoryBytes;
    reading.process.source = runtime::ProcessMemorySource::OtherHostOnly;
    reading.process.status = runtime::ProcessMemoryStatus::Unsupported;
    reading.process.sampledAt = std::chrono::steady_clock::now();
    return reading;
}

// Feeds `ledger` the fixed reading, as a completed process sample. Call on the ledger's thread.
inline runtime::MemoryBudgetState applyFixedMemoryBudget(runtime::MemoryBudgetLedger& ledger) {
    ledger.beginProcessSampling();
    return ledger.applySample(fixedMemoryBudgetSample());
}

} // namespace bloom::test_support

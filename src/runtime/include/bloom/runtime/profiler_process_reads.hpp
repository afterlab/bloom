#pragma once

// The profiler sampler's cheap, allocation-free reads of this process (#423). Each is O(1) and
// safe at the sampler rate: Linux reads /proc/self/statm into a fixed buffer (never smaps or
// smaps_rollup), macOS asks task_info, Windows GetProcessMemoryInfo and GetProcessTimes.

#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::runtime {

struct ProfilerResidentMemory final {
    std::uint64_t residentBytes = 0;
    // Resident file-backed plus shared memory (Linux statm "shared"); absent elsewhere.
    std::optional<std::uint64_t> fileAndSharedBytes;
};

// Parses /proc/self/statm text ("size resident shared text lib data dt", in pages). Returns
// nothing when the first three fields are not all present and numeric.
[[nodiscard]] std::optional<ProfilerResidentMemory>
parseProfilerStatm(std::string_view text, std::uint64_t pageBytes) noexcept;

[[nodiscard]] std::optional<ProfilerResidentMemory> readProfilerResidentMemory() noexcept;

// Cumulative user plus system CPU time of this process.
[[nodiscard]] std::optional<std::uint64_t> readProfilerProcessCpuNs() noexcept;

// Lowers the calling thread's scheduling priority (best effort).
void lowerProfilerThreadPriority() noexcept;

} // namespace bloom::runtime

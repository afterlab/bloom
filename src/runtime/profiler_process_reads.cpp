#include <bloom/runtime/profiler_process_reads.hpp>

#include <array>
#include <charconv>
#include <cstddef>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: psapi.h needs its types.
#include <psapi.h>
#endif

namespace bloom::runtime {
namespace {

// Reads the next unsigned decimal field of `text` from `position`, skipping leading spaces.
std::optional<std::uint64_t> nextField(const std::string_view text, std::size_t& position) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
        ++position;
    std::uint64_t value = 0;
    const auto* begin = text.data() + position;
    const auto* end = text.data() + text.size();
    const auto [last, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || last == begin)
        return std::nullopt;
    position += static_cast<std::size_t>(last - begin);
    return value;
}

[[maybe_unused]] std::uint64_t timevalNs(const long seconds, const long micros) {
    return static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(micros) * 1'000ULL;
}

} // namespace

std::optional<ProfilerResidentMemory> parseProfilerStatm(const std::string_view text,
                                                         const std::uint64_t pageBytes) noexcept {
    std::size_t position = 0;
    const auto size = nextField(text, position);
    const auto resident = nextField(text, position);
    const auto shared = nextField(text, position);
    if (!size || !resident || !shared || pageBytes == 0)
        return std::nullopt;
    return ProfilerResidentMemory{.residentBytes = *resident * pageBytes,
                                  .fileAndSharedBytes = *shared * pageBytes};
}

std::optional<ProfilerResidentMemory> readProfilerResidentMemory() noexcept {
#if defined(__linux__)
    static const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return std::nullopt;
    const int descriptor = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return std::nullopt;
    std::array<char, 160> buffer{};
    const auto length = ::read(descriptor, buffer.data(), buffer.size() - 1);
    ::close(descriptor);
    if (length <= 0)
        return std::nullopt;
    return parseProfilerStatm(std::string_view(buffer.data(), static_cast<std::size_t>(length)),
                              static_cast<std::uint64_t>(page));
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), // NOLINT(*-reinterpret-cast)
                  &count) != KERN_SUCCESS)
        return std::nullopt;
    return ProfilerResidentMemory{.residentBytes = info.resident_size, .fileAndSharedBytes = {}};
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0)
        return std::nullopt;
    return ProfilerResidentMemory{.residentBytes = counters.WorkingSetSize,
                                  .fileAndSharedBytes = {}};
#else
    return std::nullopt;
#endif
}

std::optional<std::uint64_t> readProfilerProcessCpuNs() noexcept {
#if defined(__linux__) || defined(__APPLE__)
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return std::nullopt;
    return timevalNs(usage.ru_utime.tv_sec, usage.ru_utime.tv_usec) +
           timevalNs(usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
#elif defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0)
        return std::nullopt;
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    // FILETIME counts 100 ns ticks.
    return (ticks(kernel) + ticks(user)) * 100ULL;
#else
    return std::nullopt;
#endif
}

void lowerProfilerThreadPriority() noexcept {
#if defined(__linux__)
    // Per-thread nice value on Linux.
    static_cast<void>(::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10));
#elif defined(__APPLE__)
    // The lowest priority of the thread's current policy.
    int policy = 0;
    sched_param parameters{};
    if (pthread_getschedparam(pthread_self(), &policy, &parameters) == 0) {
        parameters.sched_priority = sched_get_priority_min(policy);
        static_cast<void>(pthread_setschedparam(pthread_self(), policy, &parameters));
    }
#elif defined(_WIN32)
    static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST));
#endif
}

} // namespace bloom::runtime

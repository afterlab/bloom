#include <bloom/runtime/profiler_process_reads.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <span>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
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

#if defined(__linux__)
// Reads /proc/<pid>/<leaf> into `buffer` without allocating; returns the text read.
std::string_view readProcFile(const std::int64_t pid, const std::string_view leaf,
                              std::span<char> buffer) noexcept {
    std::array<char, 64> path{};
    constexpr std::string_view prefix = "/proc/";
    auto* cursor = std::copy(prefix.begin(), prefix.end(), path.data());
    const auto converted = std::to_chars(cursor, path.data() + path.size() - leaf.size() - 2, pid);
    if (converted.ec != std::errc{})
        return {};
    cursor = converted.ptr;
    *cursor++ = '/';
    cursor = std::copy(leaf.begin(), leaf.end(), cursor);
    *cursor = '\0';
    const int descriptor = ::open(path.data(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return {};
    const auto length = ::read(descriptor, buffer.data(), buffer.size() - 1);
    ::close(descriptor);
    return length > 0 ? std::string_view(buffer.data(), static_cast<std::size_t>(length))
                      : std::string_view{};
}
#endif

} // namespace

std::optional<ProfilerProcStat> parseProfilerProcStat(const std::string_view text,
                                                      const std::uint64_t ticksPerSecond) noexcept {
    // The command (field 2) may hold spaces and parentheses; everything after its last ')' is
    // space-separated: state (3), ppid (4) ... utime (14), stime (15).
    const auto close = text.rfind(')');
    if (close == std::string_view::npos || ticksPerSecond == 0)
        return std::nullopt;
    std::size_t position = close + 1;
    while (position < text.size() && text[position] == ' ')
        ++position;
    if (position >= text.size())
        return std::nullopt;
    ProfilerProcStat result;
    result.state = text[position++];
    // Skip fields 4..13 (ten fields; tty_nr and tpgid can be negative), then read utime and stime.
    for (int field = 4; field <= 13; ++field) {
        while (position < text.size() && text[position] == ' ')
            ++position;
        const auto start = position;
        while (position < text.size() && text[position] != ' ')
            ++position;
        if (position == start)
            return std::nullopt;
    }
    const auto user = nextField(text, position);
    const auto system = nextField(text, position);
    if (!user || !system)
        return std::nullopt;
    result.cpuNs = (*user + *system) * (1'000'000'000ULL / ticksPerSecond);
    return result;
}

ProfilerHelperReading readProfilerHelperProcess(const std::int64_t pid) noexcept {
    ProfilerHelperReading reading;
    if (pid <= 0)
        return reading;
#if defined(__linux__)
    static const long page = ::sysconf(_SC_PAGESIZE);
    static const long ticks = ::sysconf(_SC_CLK_TCK);
    std::array<char, 512> buffer{};
    const auto stat = parseProfilerProcStat(readProcFile(pid, "stat", buffer),
                                            ticks > 0 ? static_cast<std::uint64_t>(ticks) : 0);
    if (!stat)
        return reading;
    reading.alive = stat->state != 'Z' && stat->state != 'X' && stat->state != 'x';
    reading.cpuNs = stat->cpuNs;
    if (page > 0)
        if (const auto statm = parseProfilerStatm(readProcFile(pid, "statm", buffer),
                                                  static_cast<std::uint64_t>(page)))
            reading.residentBytes = statm->residentBytes;
#elif defined(__APPLE__)
    proc_taskinfo info{};
    if (proc_pidinfo(static_cast<int>(pid), PROC_PIDTASKINFO, 0, &info, sizeof(info)) !=
        static_cast<int>(sizeof(info)))
        return reading;
    reading.alive = true;
    reading.residentBytes = info.pti_resident_size;
    // Task times are Mach absolute time units.
    static const auto timebase = [] {
        mach_timebase_info_data_t value{};
        if (mach_timebase_info(&value) != KERN_SUCCESS || value.denom == 0)
            value = {1, 1};
        return value;
    }();
    reading.cpuNs = (info.pti_total_user + info.pti_total_system) * timebase.numer / timebase.denom;
#elif defined(_WIN32)
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return reading;
    DWORD exitCode = 0;
    reading.alive = GetExitCodeProcess(process, &exitCode) != 0 && exitCode == STILL_ACTIVE;
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(process, &counters, sizeof(counters)) != 0)
        reading.residentBytes = counters.WorkingSetSize;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user) != 0) {
        const auto ticks = [](const FILETIME& time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
        };
        reading.cpuNs = (ticks(kernel) + ticks(user)) * 100ULL;
    }
    CloseHandle(process);
#endif
    return reading;
}

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

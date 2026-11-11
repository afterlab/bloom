#include "gpu_shader_compiler_process.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::color::gpu_shader_detail {

namespace {

#if defined(__linux__)
// The second field of a schedstat file: nanoseconds spent runnable on a run queue, not running.
std::optional<std::chrono::nanoseconds> runQueueWait(const std::string& path) {
    std::ifstream file(path);
    std::uint64_t running = 0;
    std::uint64_t waiting = 0;
    if (!(file >> running >> waiting))
        return std::nullopt;
    return std::chrono::nanoseconds(
        static_cast<std::chrono::nanoseconds::rep>(std::min<std::uint64_t>(
            waiting, static_cast<std::uint64_t>(std::chrono::nanoseconds::max().count()))));
}
#endif

ScheduledDeadline::Sources systemSources() {
#if defined(__linux__)
    return {[] { return runQueueWait("/proc/thread-self/schedstat"); },
            [](const std::int64_t processId) {
                return runQueueWait("/proc/" + std::to_string(processId) + "/schedstat");
            }};
#else
    return {};
#endif
}

} // namespace

ScheduledDeadline::ScheduledDeadline(const std::chrono::milliseconds limit,
                                     const std::chrono::milliseconds ceiling)
    : ScheduledDeadline(limit, ceiling, systemSources()) {}

ScheduledDeadline::ScheduledDeadline(const std::chrono::milliseconds limit,
                                     const std::chrono::milliseconds ceiling, Sources sources)
    : sources_(std::move(sources)), start_(Clock::now()), limit_(start_ + limit),
      ceiling_(start_ + std::max(limit, ceiling)), current_(limit_) {
    if (sources_.thisThread)
        threadAtStart_ = sources_.thisThread();
}

void ScheduledDeadline::refresh() {
    if (threadAtStart_ && sources_.thisThread) {
        if (const auto now = sources_.thisThread(); now && *now > *threadAtStart_)
            threadWait_ = std::max(threadWait_, *now - *threadAtStart_);
    }
    if (liveProcess_ > 0 && sources_.process) {
        if (const auto now = sources_.process(liveProcess_))
            liveProcessWait_ = std::max(liveProcessWait_, *now);
    }
    const auto room = ceiling_ - limit_;
    current_ = limit_ + std::min<Clock::duration>(
                            room, std::chrono::duration_cast<Clock::duration>(waited()));
}

bool ScheduledDeadline::expired() {
    if (Clock::now() < current_)
        return false;
    refresh();
    return Clock::now() >= current_;
}

void ScheduledDeadline::processStarted(const std::int64_t processId) {
    processFinished();
    liveProcess_ = processId;
}

void ScheduledDeadline::processFinished() {
    finishedProcessWait_ += liveProcessWait_;
    liveProcessWait_ = std::chrono::nanoseconds{0};
    liveProcess_ = 0;
}

std::chrono::nanoseconds ScheduledDeadline::waited() const noexcept {
    return threadWait_ + finishedProcessWait_ + liveProcessWait_;
}

namespace {

BoundedHash hashFileUntil(const std::string& path, std::size_t ceiling,
                          const std::function<bool()>& expired,
                          const platform::ProcessCancellation& cancel) {
    BoundedHash result;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > ceiling) {
        result.error = GpuShaderCompileError::InvalidTool;
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = GpuShaderCompileError::InvalidTool;
        return result;
    }
    core::Sha256Hasher hasher;
    // On the heap: 1 MiB does not fit the stack of a worker thread on every platform.
    std::vector<std::byte> chunk(std::size_t{1} << 20);
    std::uintmax_t remaining = size;
    while (remaining > 0) {
        if (cancel && cancel()) {
            result.error = GpuShaderCompileError::Cancelled;
            return result;
        }
        if (expired()) {
            result.error = GpuShaderCompileError::Timeout;
            return result;
        }
        const auto want = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, static_cast<std::uintmax_t>(chunk.size())));
        input.read(reinterpret_cast<char*>(chunk.data()), want);
        const auto got = input.gcount();
        if (got <= 0) {
            result.error = GpuShaderCompileError::InvalidTool;
            return result;
        }
        if (!hasher.update(
                std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(got)))) {
            result.error = GpuShaderCompileError::IoFailure;
            return result;
        }
        remaining -= static_cast<std::uintmax_t>(got);
    }
    result.digest = hasher.finalize();
    return result;
}

} // namespace

BoundedHash hashFileBounded(const std::string& path, const std::size_t ceiling,
                            ScheduledDeadline& deadline,
                            const platform::ProcessCancellation& cancel) {
    return hashFileUntil(path, ceiling, [&deadline] { return deadline.expired(); }, cancel);
}

BoundedHash hashFileBounded(const std::string& path, const std::size_t ceiling,
                            const GpuShaderDeadline deadline,
                            const platform::ProcessCancellation& cancel) {
    return hashFileUntil(
        path, ceiling, [deadline] { return std::chrono::steady_clock::now() >= deadline; }, cancel);
}

ToolRun runProcess(const std::string& executable, const std::vector<std::string>& arguments,
                   std::uint64_t addressSpaceBytes, std::uint32_t openFiles,
                   ScheduledDeadline& deadline, const platform::ProcessCancellation& cancel,
                   std::size_t maxOutput) {
    using platform::ProcessError;
    using platform::ProcessFailure;
    using platform::ProcessOptions;
    using platform::ProcessSupervisor;

    ToolRun run;
    ProcessOptions options;
    options.executable = executable;
    options.arguments = arguments;
    options.addressSpaceBytes = addressSpaceBytes;
    options.openFiles = openFiles;
    auto launched = ProcessSupervisor::launch(options);
    if (auto* failure = std::get_if<ProcessFailure>(&launched)) {
        run.failure = *failure;
        return run;
    }
    auto child = std::move(std::get<std::unique_ptr<ProcessSupervisor>>(launched));
    // The child's waits for a processor count toward the limit while it runs.
    deadline.processStarted(child->processId());
    struct Finished final {
        ScheduledDeadline& deadline;
        ~Finished() { deadline.processFinished(); }
    } finishedGuard{deadline};
    // ProcessSupervisor::read is a fixed-length protocol read: it fills the whole span or fails,
    // and an early end-of-stream discards any partial bytes. A byte at a time is the only correct
    // way to stream unknown-length tool output without losing it. Diagnostics are small and SPIR-V
    // goes to a file, so the extra syscalls are bounded.
    std::array<std::byte, 1> buffer{};
    while (true) {
        auto read = child->read(buffer, deadline.current(), cancel);
        if (auto* failure = std::get_if<ProcessFailure>(&read)) {
            if (failure->code == ProcessError::Crashed && failure->nativeCode == 0)
                break; // End of stream: the child closed stdout.
            // The limit was reached on the wall clock. If the child spent part of that time
            // waiting for a processor, the limit has moved: keep reading.
            if (failure->code == ProcessError::Timeout && !deadline.expired())
                continue;
            run.failure = *failure;
            return run;
        }
        if (run.output.size() < maxOutput)
            run.output.push_back(static_cast<std::uint8_t>(buffer[0]));
        else
            run.outputOverflow = true;
    }
    // Wait for the exit under the same limit, reading the child's waits while it still exists.
    // finish() then reports the exit, or stops a child that outlived the limit.
    deadline.refresh();
    while (!child->waitExit(std::chrono::milliseconds(2)).has_value()) {
        if (deadline.expired())
            break;
    }
    auto finished = child->finish(deadline.current());
    if (auto* failure = std::get_if<ProcessFailure>(&finished)) {
        if (failure->code == ProcessError::Crashed) {
            run.exitStatus = failure->nativeCode;
            return run;
        }
        run.failure = *failure;
        return run;
    }
    return run;
}

} // namespace bloom::color::gpu_shader_detail

#pragma once

// Private to bloom_color_gpu_shader. Not installed and not part of the public header set.

#include <bloom/color/gpu_shader_compiler.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/platform/process_supervisor.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bloom::color::gpu_shader_detail {

struct ToolRun {
    std::optional<platform::ProcessFailure> failure;
    int exitStatus = 0;
    std::vector<std::uint8_t> output;
    bool outputOverflow = false;
};

// Incremental, cancellable content verification of a bounded executable. The hash is computed in
// chunked reads with cancellation and deadline checks between chunks so a large binary cannot block
// a cancel request behind one monolithic read+hash.
struct BoundedHash {
    std::optional<core::Sha256Digest> digest;
    std::optional<GpuShaderCompileError> error;
};

// A compile's time limit (#623). The limit is spent only while the work can run: time this thread
// or the compile's tool process is runnable but waiting for a processor is added back, so a
// machine that is busy compiling something else does not turn a compile that needs a fraction
// of a second of processor time into a timeout. A tool that is blocked or asleep earns nothing,
// so a hung tool still ends at the limit. Whatever is added back, the limit never passes
// `ceiling` of wall-clock time from the start. The waits come from the operating system's
// scheduler statistics (Linux); where there are none the limit is plain wall-clock time.
// Used on the one thread that runs the compile.
class ScheduledDeadline final {
  public:
    using Clock = std::chrono::steady_clock;
    // Total time spent runnable but not running, or nothing when the system does not report it.
    struct Sources {
        std::function<std::optional<std::chrono::nanoseconds>()> thisThread;
        std::function<std::optional<std::chrono::nanoseconds>(std::int64_t processId)> process;
    };
    ScheduledDeadline(std::chrono::milliseconds limit, std::chrono::milliseconds ceiling);
    ScheduledDeadline(std::chrono::milliseconds limit, std::chrono::milliseconds ceiling,
                      Sources sources);

    [[nodiscard]] Clock::time_point start() const noexcept { return start_; }
    // The wall-clock instant the limit ends at, as of the last refresh.
    [[nodiscard]] Clock::time_point current() const noexcept { return current_; }
    // Reads the waits again and moves current() accordingly.
    void refresh();
    // False before current(); at or past it, refreshes once and answers against the result.
    [[nodiscard]] bool expired();
    // The tool process whose waits count from now, and the end of its run. Its waits are read
    // while it exists, so finish only after the last refresh that should see it.
    void processStarted(std::int64_t processId);
    void processFinished();
    // What has been added back so far.
    [[nodiscard]] std::chrono::nanoseconds waited() const noexcept;

  private:
    Sources sources_;
    Clock::time_point start_;
    Clock::time_point limit_;
    Clock::time_point ceiling_;
    Clock::time_point current_;
    std::optional<std::chrono::nanoseconds> threadAtStart_;
    std::chrono::nanoseconds threadWait_{0};
    std::chrono::nanoseconds finishedProcessWait_{0};
    std::chrono::nanoseconds liveProcessWait_{0};
    std::int64_t liveProcess_ = 0;
};

BoundedHash hashFileBounded(const std::string& path, std::size_t ceiling,
                            ScheduledDeadline& deadline,
                            const platform::ProcessCancellation& cancel);
// The same against a plain wall-clock instant.
BoundedHash hashFileBounded(const std::string& path, std::size_t ceiling,
                            GpuShaderDeadline deadline,
                            const platform::ProcessCancellation& cancel);

// Bounded, cancellable run of one absolute-path child. No shell is ever involved. stdout is drained
// to EOF while the child runs so a chatty tool cannot block on a full pipe, and the caller's
// cancellation and deadline are honored during the read. stderr is discarded by the platform
// supervisor; glslangValidator reports diagnostics on stdout and those are captured.
ToolRun runProcess(const std::string& executable, const std::vector<std::string>& arguments,
                   std::uint64_t addressSpaceBytes, std::uint32_t openFiles,
                   ScheduledDeadline& deadline, const platform::ProcessCancellation& cancel,
                   std::size_t maxOutput);

} // namespace bloom::color::gpu_shader_detail

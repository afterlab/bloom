#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::platform {
class ReadOnlyFileSnapshot;
class SharedFrameArena;
class StagedArtifactInheritableWriter;
enum class ProcessError { Unavailable, Spawn, ResourceLimit, Io, Crashed, Timeout, Cancelled };
struct ProcessFailure {
    ProcessError code = ProcessError::Io;
    int nativeCode = 0;
};
// How a child ended, once it has been reaped. `signaled` is a POSIX signal death (code is the
// signal number); otherwise code is the exit code (the NT exit status on Windows).
struct ProcessExit {
    bool signaled = false;
    int code = 0;
};
// A plain, platform-neutral cause for error messages ("killed by signal 9", "exited with code 3").
[[nodiscard]] inline std::string describeProcessExit(const ProcessExit& exit) {
    if (exit.signaled) {
        const char* name = exit.code == 9    ? " (killed)"
                           : exit.code == 11 ? " (segmentation fault)"
                           : exit.code == 6  ? " (aborted)"
                           : exit.code == 15 ? " (terminated)"
                                             : "";
        return "killed by signal " + std::to_string(exit.code) + name;
    }
    if (exit.code == 0)
        return "exited normally";
    if (static_cast<unsigned>(exit.code) >= 0xC0000000U) {
        char text[16];
        std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(exit.code));
        return std::string("crashed with status 0x") + text;
    }
    return "exited with code " + std::to_string(exit.code);
}
template <typename T> using ProcessResult = std::variant<T, ProcessFailure>;
using ProcessDeadline = std::chrono::steady_clock::time_point;
using ProcessCancellation = std::function<bool()>;
struct ProcessOptions {
    std::string executable;
    std::vector<std::string> arguments;
    // Optional child environment entries in KEY=VALUE form. The supervisor still supplies the
    // deterministic LC_ALL/TZ baseline; entries here override those two keys when requested.
    std::vector<std::string> environment;
    std::uint64_t addressSpaceBytes = 2ULL * 1024 * 1024 * 1024;
    std::uint32_t openFiles = 64;
    std::chrono::milliseconds messageGrace{50}, termGrace{100};
    // Owning, captured source capability. Only this handle can install child fd 4; ordinary
    // external paths cannot opt into the immutable-input provider contract.
    std::shared_ptr<const ReadOnlyFileSnapshot> inputSnapshot{};
    // ADR 0033 section 16: the staged artifact the child writes directly, installed as child fd 5
    // (Linux). Other platforms refuse a launch that asks for it.
    std::shared_ptr<const StagedArtifactInheritableWriter> stagedOutput{};
    // Issue #474: the shared frame arena the child writes decoded planes into, installed as
    // child fd 6 (Linux). Other platforms refuse a launch that asks for it.
    std::shared_ptr<const SharedFrameArena> frameArena{};
};
// Blocking API. Call only on a BlockingIo executor. No detached threads or host signal handlers.
// The trusted worker must call processWorkerBootstrap() before loading provider code.
class ProcessSupervisor final {
  public:
    [[nodiscard]] static ProcessResult<std::unique_ptr<ProcessSupervisor>>
    launch(const ProcessOptions& options);
    ~ProcessSupervisor();
    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;
    [[nodiscard]] ProcessResult<std::size_t> write(std::span<const std::byte> bytes,
                                                   ProcessDeadline deadline,
                                                   const ProcessCancellation& cancel);
    [[nodiscard]] ProcessResult<std::size_t>
    read(std::span<std::byte> bytes, ProcessDeadline deadline, const ProcessCancellation& cancel);
    // Waits for orderly exit after the caller's shutdown message/ack. Zero is success.
    [[nodiscard]] ProcessResult<int> finish(ProcessDeadline deadline);
    // Cancellation message -> grace -> SIGTERM -> grace -> SIGKILL; always reaps the child.
    void stop(std::span<const std::byte> cancellationMessage = {});
    [[nodiscard]] std::int64_t processId() const;
    // Reaps the child if it has ended, waiting up to `grace` for it to do so (zero: a single
    // non-blocking check). Returns how it ended, or nothing while it is still running. Safe to
    // call at any time, including while idle, so an exited worker is never left defunct.
    [[nodiscard]] std::optional<ProcessExit> waitExit(std::chrono::milliseconds grace);

  private:
    struct State;
    explicit ProcessSupervisor(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
[[nodiscard]] bool processWorkerBootstrap();
} // namespace bloom::platform

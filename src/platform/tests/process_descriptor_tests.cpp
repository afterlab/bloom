#include <bloom/platform/process_supervisor.hpp>
#if defined(__linux__)
#include <bloom/platform/private_temporary_directory.hpp>
#include <bloom/platform/staged_artifact.hpp>
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {
// This executable also acts as a codec-free trusted worker. Bootstrap proves the real fd3 gate;
// the exchange proves stdin/stdout are intact and the absent source capability leaves fd4 closed.
int worker() {
    if (!bloom::platform::processWorkerBootstrap())
        return 1;
    errno = 0;
    if (::fcntl(4, F_GETFD) != -1 || errno != EBADF)
        return 2;
    char received = 0;
    if (::read(STDIN_FILENO, &received, 1) != 1 || received != 'Q')
        return 3;
    constexpr char reply = 'R';
    return ::write(STDOUT_FILENO, &reply, 1) == 1 ? 0 : 4;
}

#if defined(__linux__)
constexpr std::string_view kStagedPayload = "streamed by the worker";
// A worker given a staged artifact (ADR 0033 section 16): fd 5 must be that regular file, which it
// resets and writes as the media worker does, and fd 3 must have been the gate.
int stagedWorker() {
    if (!bloom::platform::processWorkerBootstrap())
        return 1;
    struct stat status{};
    if (::fstat(5, &status) != 0 || !S_ISREG(status.st_mode))
        return 2;
    if (::ftruncate(5, 0) != 0 ||
        ::pwrite(5, kStagedPayload.data(), kStagedPayload.size(), 0) !=
            static_cast<ssize_t>(kStagedPayload.size()))
        return 3;
    char received = 0;
    if (::read(STDIN_FILENO, &received, 1) != 1 || received != 'Q')
        return 4;
    constexpr char reply = 'R';
    return ::write(STDOUT_FILENO, &reply, 1) == 1 ? 0 : 5;
}
#endif

// A worker that holds about 48 MiB of touched memory before it answers, so the parent can read the
// memory the operating system reports for a supervised child.
constexpr std::size_t kHeldBytes = std::size_t{48} << 20U;
int memoryWorker() {
    if (!bloom::platform::processWorkerBootstrap())
        return 1;
    std::vector<unsigned char> held(kHeldBytes);
    for (std::size_t at = 0; at < held.size(); at += 4096)
        held[at] = static_cast<unsigned char>(at >> 12U) | 1U;
    char received = 0;
    if (::read(STDIN_FILENO, &received, 1) != 1 || received != 'Q')
        return 3;
    // Every touched page is read back, so the compiler cannot drop the stores.
    unsigned long sum = 0;
    for (std::size_t at = 0; at < held.size(); at += 4096)
        sum += held[at];
    const char reply = sum != 0 ? 'R' : 'X';
    if (::write(STDOUT_FILENO, &reply, 1) != 1)
        return 4;
    // Hold the memory until the parent has read it: an exiting process releases it first.
    return ::read(STDIN_FILENO, &received, 1) == 1 && received == 'Z' ? 0 : 5;
}

// A tool that finishes at once and never reads the gate, as a fast external compiler does.
int quickTool() { return 0; }

// #623: launching a tool that exits at once must never fail the launch. The child closes the gate
// as it exits, which can happen before the parent writes the ready byte and before the child can
// be reaped; under load that window is wide, and the write used to report it as a failed spawn.
int quickToolsLaunch(const std::string& executable) {
    constexpr int kThreads = 16;
    constexpr int kLaunches = 150;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread)
        threads.emplace_back([&] {
            for (int launch = 0; launch < kLaunches; ++launch) {
                bloom::platform::ProcessOptions options;
                options.executable = executable;
                options.arguments = {"--quick-tool"};
                auto launched = bloom::platform::ProcessSupervisor::launch(options);
                auto* process =
                    std::get_if<std::unique_ptr<bloom::platform::ProcessSupervisor>>(&launched);
                if (!process) {
                    ++failures;
                    continue;
                }
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                std::array<std::byte, 1> byte{};
                const auto ended = (*process)->read(byte, deadline, {});
                const auto finished = (*process)->finish(deadline);
                if (!std::holds_alternative<bloom::platform::ProcessFailure>(ended) ||
                    !std::holds_alternative<int>(finished) || std::get<int>(finished) != 0)
                    ++failures;
            }
        });
    for (auto& thread : threads)
        thread.join();
    return failures.load();
}

int memoryReport(const std::string& executable) {
    bloom::platform::ProcessOptions options;
    options.executable = executable;
    options.arguments = {"--memory-worker"};
    auto launched = bloom::platform::ProcessSupervisor::launch(options);
    auto* process = std::get_if<std::unique_ptr<bloom::platform::ProcessSupervisor>>(&launched);
    if (!process)
        return 20;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const std::array query{std::byte{'Q'}};
    std::array<std::byte, 1> reply{};
    if (!std::holds_alternative<std::size_t>((*process)->write(query, deadline, {})) ||
        !std::holds_alternative<std::size_t>((*process)->read(reply, deadline, {})) ||
        reply.front() != std::byte{'R'})
        return 21;
    const auto held = (*process)->memory();
    if (!held)
        return 22;
    // The child holds 48 MiB; a generous floor keeps the check about the report, not the runtime.
    if (held->residentBytes < (std::size_t{40} << 20U) ||
        held->peakResidentBytes < held->residentBytes)
        return 23;
    const std::array release{std::byte{'Z'}};
    if (!std::holds_alternative<std::size_t>((*process)->write(release, deadline, {})))
        return 26;
    const auto finished = (*process)->finish(deadline);
    if (!std::holds_alternative<int>(finished) || std::get<int>(finished) != 0)
        return 24;
    (void)(*process)->waitExit(std::chrono::milliseconds(0));
    // A reaped child has no memory to report.
    return (*process)->memory().has_value() ? 25 : 0;
}

int exchange(const std::string& executable, const bool occupied) {
    // This runs in a dedicated forked harness, before any worker threads. Its descriptor setup
    // cannot close or replace a descriptor in CTest or in the parent test process.
    ::close(4);
    if (occupied) {
        const int source = ::open("/dev/null", O_RDONLY);
        if (source < 0)
            return 10;
        const int copied = ::dup2(source, 4);
        if (source != 4)
            ::close(source);
        if (copied != 4)
            return 11;
    }
    bloom::platform::ProcessOptions options;
    options.executable = executable;
    options.arguments = {"--descriptor-worker"};
    auto launched = bloom::platform::ProcessSupervisor::launch(options);
    auto* process = std::get_if<std::unique_ptr<bloom::platform::ProcessSupervisor>>(&launched);
    if (!process) {
        const auto failure = std::get<bloom::platform::ProcessFailure>(launched);
        std::fprintf(stderr, "descriptor launch failed: code=%u native=%d\n",
                     static_cast<unsigned>(failure.code), failure.nativeCode);
        return 12;
    }
    errno = 0;
    const int flags = ::fcntl(4, F_GETFD);
    if ((occupied && flags < 0) || (!occupied && (flags != -1 || errno != EBADF)))
        return 13;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    const std::array query{std::byte{'Q'}};
    std::array<std::byte, 1> reply{};
    const auto written = (*process)->write(query, deadline, {});
    const auto read = (*process)->read(reply, deadline, {});
    const auto finished = (*process)->finish(deadline);
    return std::holds_alternative<std::size_t>(written) && std::get<std::size_t>(written) == 1 &&
                   std::holds_alternative<std::size_t>(read) && std::get<std::size_t>(read) == 1 &&
                   reply.front() == std::byte{'R'} && std::holds_alternative<int>(finished) &&
                   std::get<int>(finished) == 0
               ? 0
               : 14;
}

bool isolatedExchange(const std::string& executable, const bool occupied) {
    const pid_t harness = ::fork();
    if (harness < 0)
        return false;
    if (harness == 0)
        ::_exit(exchange(executable, occupied));
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(harness, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const bool passed = waited == harness && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!passed)
        std::fprintf(stderr, "fd4 %s harness failed: status=%d\n", occupied ? "occupied" : "closed",
                     status);
    return passed;
}
#if defined(__linux__)
// The staged writer is an ordinary open in the host, so it takes the lowest free descriptor number,
// which in a busy host can be 3 or 4: the numbers the launch installs the gate at and closes for an
// absent source. The launch must still hand the child that file at 5. Before the fix, a writer at 3
// reached the child as the gate pipe (ftruncate failed: "Cannot reset the staged artifact"), and a
// writer at 4 was closed before it was copied (the launch failed with EBADF: "Media worker I/O
// failed").
int stagedExchange(const std::string& executable, const int writerDescriptor) {
    // A dedicated forked harness, as above: replacing 3 and 4 touches nothing outside it. Both
    // hold placeholders while the coordinator and the lease open their own files, then only the
    // number under test is freed, so the writer's open takes exactly that number.
    for (const int placeholder : {3, 4}) {
        const int opened = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (opened < 0 || (opened != placeholder && ::dup3(opened, placeholder, O_CLOEXEC) < 0))
            return 29;
        if (opened != placeholder)
            ::close(opened);
    }
    const auto folder = bloom::platform::createPrivateTemporaryDirectory("bloom-descriptor-staged-");
    if (!folder)
        return 30;
    const auto removeFolder = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(*folder, ignored);
    };
    auto created = bloom::platform::StagedArtifactCoordinator::create({});
    if (!created) {
        removeFolder();
        return 31;
    }
    auto coordinator = std::move(created).takeCoordinator();
    auto preflight = coordinator.preflight(
        {.targetPath = *folder / "movie.mov",
         .overwritePolicy = bloom::platform::ArtifactOverwritePolicy::CreateOrReplace,
         .expectedTarget = std::nullopt});
    auto lease = std::move(coordinator.stage(std::move(preflight).takeTarget())).takeLease();
    ::close(writerDescriptor);
    auto writer = lease.inheritableWriter();
    if (!writer || writer.writer->descriptor() != writerDescriptor) {
        std::fprintf(stderr, "the staged writer is at %d, not %d\n",
                     writer ? writer.writer->descriptor() : -1, writerDescriptor);
        removeFolder();
        return 33;
    }
    bloom::platform::ProcessOptions options;
    options.executable = executable;
    options.arguments = {"--staged-worker"};
    options.stagedOutput = writer.writer;
    auto launched = bloom::platform::ProcessSupervisor::launch(options);
    auto* process = std::get_if<std::unique_ptr<bloom::platform::ProcessSupervisor>>(&launched);
    if (!process) {
        const auto failure = std::get<bloom::platform::ProcessFailure>(launched);
        std::fprintf(stderr, "staged launch with the writer at %d failed: code=%u native=%d\n",
                     writerDescriptor, static_cast<unsigned>(failure.code), failure.nativeCode);
        removeFolder();
        return 34;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const std::array query{std::byte{'Q'}};
    std::array<std::byte, 1> reply{};
    const auto written = (*process)->write(query, deadline, {});
    const auto read = (*process)->read(reply, deadline, {});
    const auto finished = (*process)->finish(deadline);
    const auto ended = (*process)->waitExit(std::chrono::milliseconds(0));
    std::array<char, kStagedPayload.size()> staged{};
    const bool landed =
        ::pread(writer.writer->descriptor(), staged.data(), staged.size(), 0) ==
            static_cast<ssize_t>(staged.size()) &&
        std::string_view(staged.data(), staged.size()) == kStagedPayload;
    removeFolder();
    if (!std::holds_alternative<std::size_t>(written) || !std::holds_alternative<std::size_t>(read) ||
        reply.front() != std::byte{'R'} || !std::holds_alternative<int>(finished)) {
        std::fprintf(stderr, "the staged worker with the writer at %d ended: %s\n",
                     writerDescriptor,
                     ended ? bloom::platform::describeProcessExit(*ended).c_str() : "unknown");
        return 35;
    }
    return landed ? 0 : 36;
}

bool isolatedStagedExchange(const std::string& executable, const int writerDescriptor) {
    const pid_t harness = ::fork();
    if (harness < 0)
        return false;
    if (harness == 0)
        ::_exit(stagedExchange(executable, writerDescriptor));
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(harness, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const bool passed = waited == harness && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!passed)
        std::fprintf(stderr, "staged writer at fd %d: harness failed: exit %d\n", writerDescriptor,
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return passed;
}
#endif
} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--descriptor-worker")
            return worker();
        if (argc == 2 && std::string_view(argv[1]) == "--memory-worker")
            return memoryWorker();
        if (argc == 2 && std::string_view(argv[1]) == "--quick-tool")
            return quickTool();
#if defined(__linux__)
        if (argc == 2 && std::string_view(argv[1]) == "--staged-worker")
            return stagedWorker();
#endif
        const auto executable = std::filesystem::absolute(argv[0]).string();
        if (const int code = memoryReport(executable); code != 0) {
            std::fprintf(stderr, "worker memory report failed: %d\n", code);
            return 1;
        }
        if (const int failed = quickToolsLaunch(executable); failed != 0) {
            std::fprintf(stderr, "%d quick-tool launches failed\n", failed);
            return 1;
        }
        bool passed = isolatedExchange(executable, false) && isolatedExchange(executable, true);
#if defined(__linux__)
        // Staged output is a Linux launch (ADR 0033 section 16); macOS refuses a launch that asks
        // for it, so these cases have no macOS form.
        passed = isolatedStagedExchange(executable, 3) && passed;
        passed = isolatedStagedExchange(executable, 4) && passed;
#endif
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "descriptor fixture failed: %s\n", error.what());
        return 1;
    }
}

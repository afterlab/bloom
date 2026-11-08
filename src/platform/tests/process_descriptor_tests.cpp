#include <bloom/platform/process_supervisor.hpp>

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
} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--descriptor-worker")
            return worker();
        if (argc == 2 && std::string_view(argv[1]) == "--memory-worker")
            return memoryWorker();
        if (argc == 2 && std::string_view(argv[1]) == "--quick-tool")
            return quickTool();
        const auto executable = std::filesystem::absolute(argv[0]).string();
        if (const int code = memoryReport(executable); code != 0) {
            std::fprintf(stderr, "worker memory report failed: %d\n", code);
            return 1;
        }
        if (const int failed = quickToolsLaunch(executable); failed != 0) {
            std::fprintf(stderr, "%d quick-tool launches failed\n", failed);
            return 1;
        }
        return isolatedExchange(executable, false) && isolatedExchange(executable, true) ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "descriptor fixture failed: %s\n", error.what());
        return 1;
    }
}

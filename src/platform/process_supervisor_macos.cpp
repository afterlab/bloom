#include <bloom/platform/file_snapshot.hpp>
#include <bloom/platform/process_supervisor.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <libproc.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#if !defined(__APPLE__)
#error "process_supervisor_macos.cpp requires macOS"
#endif

// macOS port of the Linux supervisor. Differences from the Linux provider, all forced by Darwin:
//   * no pipe2(O_CLOEXEC): pipe() plus F_DUPFD_CLOEXEC over fd 4 (the same high-fd trick).
//   * no posix_spawnattr_setrlimit_np, no prlimit and no addclosefrom_np: the parent cannot set a
//     child's kernel limits, and the child relies on close-on-exec for every descriptor the host
//     already marked, plus the explicit file actions below. The limits are enforced like this:
//       - open files and core dumps: the parent sends the open-file limit with the ready byte, and
//         a trusted worker's processWorkerBootstrap installs it and a zero core limit on itself
//         before any provider code is loaded. A tool that never calls the bootstrap gets neither.
//       - memory: an address-space limit cannot be set here (a new process already maps hundreds
//         of gigabytes of shared system libraries, and the kernel refuses a limit below that). The
//         parent instead reads the child's physical footprint while it waits on the child's pipes
//         and kills a child that exceeds ProcessOptions::addressSpaceBytes, reporting
//         ResourceLimit. A footprint is never larger than an address space, so this is no
//         stricter than the Linux limit of the same number.
//   * a pipe's SIGPIPE goes to the process, not the writing thread: the parent's write ends are
//     marked F_SETNOSIGPIPE, so a write to a closed pipe fails with EPIPE and raises nothing.
namespace bloom::platform {
namespace {
using Clock = std::chrono::steady_clock;
// What the parent writes to the gate: 'R' and the open-file limit, a 32-bit count.
constexpr std::size_t kGateBytes = 1 + sizeof(std::uint32_t);
class Fd final {
  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const { return fd_; }

  private:
    int fd_;
};
bool makePipe(std::array<Fd, 2>& ends) {
    std::array<int, 2> raw{};
    if (::pipe(raw.data()) != 0)
        return false;
    // Keep launch file actions independent of whether stdin/stdout/stderr were open in the host.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const int high = ::fcntl(raw[i], F_DUPFD_CLOEXEC, 5);
        ::close(raw[i]);
        if (high < 0) {
            for (std::size_t j = i + 1; j < raw.size(); ++j)
                ::close(raw[j]);
            return false;
        }
        ends[i] = Fd(high);
    }
    return true;
}
class SpawnActions final {
  public:
    posix_spawn_file_actions_t value{};
    int status = posix_spawn_file_actions_init(&value);
    ~SpawnActions() {
        if (status == 0)
            posix_spawn_file_actions_destroy(&value);
    }
};
class SpawnAttributes final {
  public:
    posix_spawnattr_t value{};
    int status = posix_spawnattr_init(&value);
    ~SpawnAttributes() {
        if (status == 0)
            posix_spawnattr_destroy(&value);
    }
};
// A write to a pipe the parent owns. launch() marks those pipes so that a write after the child
// closed its end fails with EPIPE and raises no SIGPIPE. Blocking the signal in the writing thread,
// as the Linux supervisor does, is not enough here: this kernel sends a pipe's SIGPIPE to the
// process, so another thread with the signal unblocked would take it.
ssize_t pipeWrite(int fd, const void* data, std::size_t size) { return ::write(fd, data, size); }
// Whether the child's physical footprint is over `limit`. An unreadable footprint (the child is
// gone) is not over: the pipe then reports how the child ended.
bool overMemoryLimit(const pid_t pid, const std::uint64_t limit) {
    rusage_info_v4 usage{};
    return ::proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) == 0 &&
           usage.ri_phys_footprint > limit;
}
ProcessResult<std::size_t> transfer(int fd, std::span<std::byte> bytes,
                                    std::span<const std::byte> output, ProcessDeadline deadline,
                                    const ProcessCancellation& cancel, const pid_t pid,
                                    const std::uint64_t memoryLimit) {
    const bool writing = !output.empty();
    const auto size = writing ? output.size() : bytes.size();
    std::size_t offset = 0;
    while (offset < size) {
        if (cancel && cancel())
            return ProcessFailure{ProcessError::Cancelled, 0};
        // Checked on every turn of the wait, at most ten milliseconds apart.
        if (overMemoryLimit(pid, memoryLimit)) {
            ::kill(-pid, SIGKILL);
            return ProcessFailure{ProcessError::ResourceLimit, ENOMEM};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0)
            return ProcessFailure{ProcessError::Timeout, 0};
        pollfd p{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int ready =
            ::poll(&p, 1, static_cast<int>(std::min<std::int64_t>(10, remaining.count())));
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return ProcessFailure{ProcessError::Io, errno};
        }
        if (ready == 0)
            continue;
        if ((p.revents & POLLNVAL) != 0)
            return ProcessFailure{ProcessError::Io, EBADF};
        const auto n = writing ? pipeWrite(fd, output.data() + offset, size - offset)
                               : ::read(fd, bytes.data() + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return ProcessFailure{errno == EPIPE ? ProcessError::Crashed : ProcessError::Io, errno};
        }
        if (n == 0)
            return ProcessFailure{ProcessError::Crashed, 0};
        offset += static_cast<std::size_t>(n);
    }
    return offset;
}
} // namespace
struct ProcessSupervisor::State {
    pid_t pid = -1;
    Fd input, output;
    ProcessOptions options;
    int status = 0;
    bool reaped = false;
    bool waitUntil(ProcessDeadline deadline) {
        while (!reaped) {
            const auto result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) {
                reaped = true;
                return true;
            }
            if (result < 0 && errno != EINTR)
                return false;
            if (Clock::now() >= deadline)
                return false;
            ::poll(nullptr, 0, 2);
        }
        return true;
    }
};
ProcessSupervisor::ProcessSupervisor(std::unique_ptr<State> state) : state_(std::move(state)) {}
ProcessSupervisor::~ProcessSupervisor() { stop(); }
ProcessResult<std::unique_ptr<ProcessSupervisor>>
ProcessSupervisor::launch(const ProcessOptions& options) {
    // Staged streaming publication is Linux-only in this release (ADR 0033 sections 15 and 16),
    // and so is the shared frame arena (#474).
    if (options.stagedOutput || options.frameArena)
        return ProcessFailure{ProcessError::Spawn, ENOTSUP};
    if (options.executable.empty() || options.executable.front() != '/' ||
        options.executable.find('\0') != std::string::npos ||
        options.addressSpaceBytes < 64ULL * 1024U * 1024U || options.openFiles < 8 ||
        options.openFiles > 4096 || options.messageGrace.count() < 0 ||
        options.messageGrace > std::chrono::seconds(5) || options.termGrace.count() < 0 ||
        options.termGrace > std::chrono::seconds(5))
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    std::array<Fd, 2> input, output, gate;
    if (!makePipe(input) || !makePipe(output) || !makePipe(gate))
        return ProcessFailure{ProcessError::Spawn, errno};
    if (::fcntl(input[1].get(), F_SETNOSIGPIPE, 1) != 0 ||
        ::fcntl(gate[1].get(), F_SETNOSIGPIPE, 1) != 0)
        return ProcessFailure{ProcessError::Spawn, errno};
    SpawnActions actions;
    SpawnAttributes attributes;
    if (actions.status != 0 || attributes.status != 0)
        return ProcessFailure{ProcessError::Spawn,
                              actions.status != 0 ? actions.status : attributes.status};
    sigset_t empty{}, defaults{};
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE);
    int code = posix_spawnattr_setflags(
        &attributes.value, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (code == 0)
        code = posix_spawnattr_setpgroup(&attributes.value, 0);
    if (code == 0)
        code = posix_spawnattr_setsigmask(&attributes.value, &empty);
    if (code == 0)
        code = posix_spawnattr_setsigdefault(&attributes.value, &defaults);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions.value, input[0].get(), STDIN_FILENO);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions.value, output[1].get(), STDOUT_FILENO);
    if (code == 0)
        code = posix_spawn_file_actions_addopen(&actions.value, STDERR_FILENO, "/dev/null",
                                                O_WRONLY, 0);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions.value, gate[0].get(), 3);
    if (code == 0 && options.inputSnapshot)
        code = posix_spawn_file_actions_adddup2(&actions.value, options.inputSnapshot->descriptor(),
                                                4);
    // Darwin treats closing an already-closed fd in spawn actions as fatal EBADF. Replace any
    // unrelated inherited fd4 with our own child-only descriptor, then close that known-open fd.
    if (code == 0 && !options.inputSnapshot)
        code = posix_spawn_file_actions_addopen(&actions.value, 4, "/dev/null", O_RDONLY, 0);
    if (code == 0 && !options.inputSnapshot)
        code = posix_spawn_file_actions_addclose(&actions.value, 4);
    if (code != 0)
        return ProcessFailure{ProcessError::Spawn, code};
    std::vector<std::string> arguments{options.executable};
    for (const auto& arg : options.arguments) {
        if (arg.find('\0') != std::string::npos)
            return ProcessFailure{ProcessError::Spawn, EINVAL};
        arguments.push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& arg : arguments)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    auto state = std::make_unique<State>();
    state->options = options;
    std::vector<std::string> environment{"LC_ALL=C", "TZ=UTC"};
    for (const auto& entry : options.environment) {
        if (entry.find('\0') != std::string::npos || entry.find('=') == std::string::npos)
            return ProcessFailure{ProcessError::Spawn, EINVAL};
        const auto separator = entry.find('=');
        const auto name = entry.substr(0, separator);
        auto found = std::find_if(environment.begin(), environment.end(),
                                  [&](const auto& value) { return value.starts_with(name + '='); });
        if (found == environment.end())
            environment.push_back(entry);
        else
            *found = entry;
    }
    std::vector<char*> environmentPointers;
    environmentPointers.reserve(environment.size() + 1);
    for (auto& entry : environment)
        environmentPointers.push_back(entry.data());
    environmentPointers.push_back(nullptr);
    code = ::posix_spawn(&state->pid, options.executable.c_str(), &actions.value, &attributes.value,
                         argv.data(), environmentPointers.data());
    if (code != 0)
        return ProcessFailure{ProcessError::Spawn, code};
    state->input = std::move(input[1]);
    state->output = std::move(output[0]);
    input[0] = Fd{};
    output[1] = Fd{};
    gate[0] = Fd{};
    auto child = std::unique_ptr<ProcessSupervisor>(new ProcessSupervisor(std::move(state)));
    // A fast external tool never calls processWorkerBootstrap, so unlike a trusted worker it does
    // not wait on the gate: it may exit before the parent writes the ready byte, and the write
    // then reports EPIPE on closed pipes. A WNOHANG probe recognizes an already-finished child so
    // the write is skipped instead of failing the launch; its output and exit status still gate
    // the caller.
    auto childExited = [&]() -> bool {
        while (true) {
            const auto result = ::waitpid(child->state_->pid, &child->state_->status, WNOHANG);
            if (result == child->state_->pid || (result < 0 && errno == ECHILD)) {
                child->state_->reaped = true;
                return true;
            }
            if (result == 0)
                return false;
            if (errno != EINTR)
                return false;
        }
    };
    const bool released = childExited();
    for (const int fd : {child->state_->input.get(), child->state_->output.get()}) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
            return ProcessFailure{ProcessError::Io, errno};
    }
    // The gate keeps the worker protocol identical to Linux: the child's bootstrap waits for 'R'
    // before loading provider code. The parent cannot install the child's limits here, so the
    // open-file limit follows the ready byte and the bootstrap installs it on itself.
    if (!released) {
        std::array<char, kGateBytes> ready{};
        ready[0] = 'R';
        const std::uint32_t openFiles = options.openFiles;
        std::memcpy(ready.data() + 1, &openFiles, sizeof(openFiles));
        // EPIPE means every holder of the gate's read end has closed it: the child has exited or
        // is exiting (its files close before it can be reaped, so a WNOHANG probe may still say it
        // runs, and under load that window is long), or it is a tool that never reads the gate.
        // Either way there is nothing to release; its output and exit status still gate the
        // caller. Any other failure is a failed launch.
        if (pipeWrite(gate[1].get(), ready.data(), ready.size()) !=
                static_cast<ssize_t>(ready.size()) &&
            errno != EPIPE && !childExited())
            return ProcessFailure{ProcessError::Spawn, errno};
    }
    return child;
}
ProcessResult<std::size_t> ProcessSupervisor::write(std::span<const std::byte> bytes,
                                                    ProcessDeadline deadline,
                                                    const ProcessCancellation& cancel) {
    return transfer(state_->input.get(), {}, bytes, deadline, cancel, state_->pid,
                    state_->options.addressSpaceBytes);
}
ProcessResult<std::size_t> ProcessSupervisor::read(std::span<std::byte> bytes,
                                                   ProcessDeadline deadline,
                                                   const ProcessCancellation& cancel) {
    return transfer(state_->output.get(), bytes, {}, deadline, cancel, state_->pid,
                    state_->options.addressSpaceBytes);
}
ProcessResult<int> ProcessSupervisor::finish(ProcessDeadline deadline) {
    state_->input = Fd{};
    if (!state_->waitUntil(deadline)) {
        stop();
        return ProcessFailure{ProcessError::Timeout, 0};
    }
    if (!WIFEXITED(state_->status) || WEXITSTATUS(state_->status) != 0)
        return ProcessFailure{ProcessError::Crashed, state_->status};
    return 0;
}
void ProcessSupervisor::stop(std::span<const std::byte> message) {
    if (state_->reaped)
        return;
    if (!message.empty()) {
        const auto ignored = write(message, Clock::now() + state_->options.messageGrace, {});
        (void)ignored;
    }
    state_->input = Fd{};
    if (state_->waitUntil(Clock::now() + state_->options.messageGrace))
        return;
    ::kill(-state_->pid, SIGTERM);
    if (state_->waitUntil(Clock::now() + state_->options.termGrace))
        return;
    ::kill(-state_->pid, SIGKILL);
    while (::waitpid(state_->pid, &state_->status, 0) < 0 && errno == EINTR) {
    }
    state_->reaped = true;
}
std::int64_t ProcessSupervisor::processId() const { return state_->pid; }
std::optional<ProcessMemory> ProcessSupervisor::memory() const {
    if (state_->reaped || state_->pid <= 0)
        return std::nullopt;
    rusage_info_v4 usage{};
    if (::proc_pid_rusage(state_->pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) !=
        0)
        return std::nullopt;
    return ProcessMemory{usage.ri_resident_size,
                         std::max<std::uint64_t>(usage.ri_resident_size,
                                                 usage.ri_lifetime_max_phys_footprint)};
}
std::optional<ProcessExit> ProcessSupervisor::waitExit(const std::chrono::milliseconds grace) {
    if (!state_->waitUntil(Clock::now() + grace))
        return std::nullopt;
    if (WIFSIGNALED(state_->status))
        return ProcessExit{true, WTERMSIG(state_->status)};
    return ProcessExit{false, WIFEXITED(state_->status) ? WEXITSTATUS(state_->status) : 0};
}
bool processWorkerBootstrap() {
    // The ready byte and the open-file limit the parent chose (see the notes at the top).
    std::array<char, kGateBytes> ready{};
    std::size_t received = 0;
    while (received < ready.size()) {
        const auto n = ::read(3, ready.data() + received, ready.size() - received);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        received += static_cast<std::size_t>(n);
    }
    ::close(3);
    if (received != ready.size() || ready[0] != 'R')
        return false;
    std::uint32_t openFiles = 0;
    std::memcpy(&openFiles, ready.data() + 1, sizeof(openFiles));
    const rlimit files{openFiles, openFiles}, core{0, 0};
    if (openFiles < 8 || ::setrlimit(RLIMIT_NOFILE, &files) != 0 ||
        ::setrlimit(RLIMIT_CORE, &core) != 0)
        return false;
    const int flags = ::fcntl(4, F_GETFD);
    return (flags < 0 && errno == EBADF) ||
           (flags >= 0 && ::fcntl(4, F_SETFD, flags | FD_CLOEXEC) == 0);
}
} // namespace bloom::platform

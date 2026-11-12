// rand_s, used once to prime the system random source before a confined worker locks down.
#define _CRT_RAND_S
#include <cstdlib>

#include <bloom/platform/file_snapshot.hpp>
#include <bloom/platform/process_supervisor.hpp>

#if !defined(_WIN32)
#error "process_supervisor_windows.cpp requires Windows"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Windows port of the media worker supervisor (ADR 0035). Same contract as the POSIX providers:
//   * stdin/stdout are byte-mode named pipes whose parent ends use overlapped I/O, so every read
//     and write shares one monotonic deadline and honours cancellation (CancelIoEx), exactly like
//     the poll() loop on Linux and macOS.
//   * the startup gate is a third pipe. Windows has no descriptor numbers, so the child learns the
//     inherited gate handle from BLOOM_PROCESS_GATE and processWorkerBootstrap() waits for 'R'.
//   * the child inherits exactly its pipe ends, the gate and NUL for stderr
//     (PROC_THREAD_ATTRIBUTE_HANDLE_LIST); nothing else leaks into it.
//   * a job object carries the resource limits: the per-process committed-memory limit stands in
//     for RLIMIT_AS, one active process forbids grandchildren, and closing the job kills the child.
//     Windows has no per-process open-handle limit; ProcessOptions::openFiles is validated only.
//   * stop(): cancellation message -> grace -> closed stdin -> grace -> TerminateJobObject. There
//   is
//     no SIGTERM for a console-less child; end of input is the cooperative request.
//   * the source snapshot (fd 4 on POSIX) is inherited as a read-only handle whose value the
//     child reads from BLOOM_PROCESS_SNAPSHOT. The staged output (fd 5) and the shared frame arena
//     are POSIX descriptors; a launch that asks for either is refused and the host takes its
//     path/copy route.
//   * a confined launch (ProcessOptions::confined) gives the child a restricted primary token:
//     every account and group in it is deny-only, the only restricting identifier is the null
//     one, and no privilege is left, so no access check that names a file, a device, a registry
//     key or a process can pass. The process cannot start under that token (the loader has to
//     open its libraries), so its first thread starts impersonating a restricted token with the
//     launching user's own access; processWorkerBootstrap() lowers the process to the untrusted
//     integrity level and drops the impersonation before the worker reads any input. The job also
//     denies the desktop, the clipboard, global atoms and other processes' window handles.
// The environment holds only LC_ALL=C, TZ=UTC, SystemRoot (required by system DLL loading), the
// gate, and the caller's explicit entries.
namespace bloom::platform {
namespace {
using Clock = std::chrono::steady_clock;
constexpr wchar_t kGateVariable[] = L"BLOOM_PROCESS_GATE";
constexpr wchar_t kConfinedVariable[] = L"BLOOM_PROCESS_CONFINED";

class Handle final {
  public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }
    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] bool valid() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_;
};

class AttributeList final {
  public:
    explicit AttributeList(DWORD count) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        storage_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, count, 0, &size))
            list_ = nullptr;
    }
    ~AttributeList() {
        if (list_ != nullptr)
            DeleteProcThreadAttributeList(list_);
    }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

  private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

bool widen(const std::string& text, std::wstring& wide) {
    wide.clear();
    if (text.empty())
        return true;
    if (text.find('\0') != std::string::npos ||
        text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
        return false;
    wide.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), wide.data(), count) == count;
}

// The inverse of CommandLineToArgvW / the C runtime's argument parser.
void appendQuoted(std::wstring& line, const std::wstring& argument) {
    if (!line.empty())
        line.push_back(L' ');
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line.push_back(L'"');
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
            line.push_back(L'"');
        } else {
            line.append(backslashes, L'\\');
            line.push_back(*it);
        }
    }
    line.push_back(L'"');
}

std::wstring uniquePipeName() {
    static std::atomic<std::uint64_t> counter{0};
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return L"\\\\.\\pipe\\bloom-media-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(counter.fetch_add(1)) + L"-" +
           std::to_wstring(static_cast<std::uint64_t>(ticks.QuadPart));
}

// One byte-mode pipe: the parent end is overlapped and private, the child end is inheritable.
// `parentWrites` selects the direction (child stdin) or the reverse (child stdout).
bool makePipe(const bool parentWrites, Handle& parent, Handle& child) {
    const auto name = uniquePipeName();
    parent.reset(CreateNamedPipeW(name.c_str(),
                                  (parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
                                      FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                      PIPE_REJECT_REMOTE_CLIENTS,
                                  1, 1U << 16, 1U << 16, 0, nullptr));
    if (!parent.valid())
        return false;
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    child.reset(CreateFileW(name.c_str(), parentWrites ? GENERIC_READ : GENERIC_WRITE, 0,
                            &inheritable, OPEN_EXISTING, 0, nullptr));
    if (!child.valid())
        return false;
    // The client is already connected; confirm it on the overlapped server end.
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid())
        return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(parent.get(), &overlapped))
        return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED)
        return true;
    if (error != ERROR_IO_PENDING)
        return false;
    DWORD ignored = 0;
    return GetOverlappedResult(parent.get(), &overlapped, &ignored, TRUE) != 0;
}

// A pipe whose other end is gone. For a read that is the end of the stream, which the contract
// reports as Crashed with native code 0 exactly like a POSIX read that returns 0: callers that
// stream a tool's output to its end test for that pair. A write keeps the system code.
ProcessFailure transferFailure(const DWORD error, const bool writing) {
    const bool peerGone =
        error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED;
    if (!peerGone)
        return ProcessFailure{ProcessError::Io, static_cast<int>(error)};
    return ProcessFailure{ProcessError::Crashed, writing ? static_cast<int>(error) : 0};
}

ProcessResult<std::size_t> transfer(HANDLE pipe, std::span<std::byte> bytes,
                                    std::span<const std::byte> output, ProcessDeadline deadline,
                                    const ProcessCancellation& cancel) {
    const bool writing = !output.empty();
    const auto size = writing ? output.size() : bytes.size();
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid())
        return ProcessFailure{ProcessError::Io, static_cast<int>(GetLastError())};
    std::size_t offset = 0;
    while (offset < size) {
        if (cancel && cancel())
            return ProcessFailure{ProcessError::Cancelled, 0};
        if (Clock::now() >= deadline)
            return ProcessFailure{ProcessError::Timeout, 0};
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        ResetEvent(event.get());
        const auto chunk =
            static_cast<DWORD>(std::min<std::size_t>(size - offset, std::size_t{1} << 20));
        DWORD count = 0;
        const BOOL started =
            writing ? WriteFile(pipe, output.data() + offset, chunk, nullptr, &overlapped)
                    : ReadFile(pipe, bytes.data() + offset, chunk, nullptr, &overlapped);
        if (const DWORD error = started ? ERROR_SUCCESS : GetLastError();
            !started && error != ERROR_IO_PENDING)
            return transferFailure(error, writing);
        // Wait in short steps so cancellation and the shared deadline stay responsive.
        std::optional<ProcessFailure> abandoned;
        while (WaitForSingleObject(event.get(), 10) == WAIT_TIMEOUT) {
            if (cancel && cancel())
                abandoned = ProcessFailure{ProcessError::Cancelled, 0};
            else if (Clock::now() >= deadline)
                abandoned = ProcessFailure{ProcessError::Timeout, 0};
            if (abandoned) {
                CancelIoEx(pipe, &overlapped);
                break;
            }
        }
        // The kernel owns `overlapped` until the operation completes or is cancelled.
        const BOOL done = GetOverlappedResult(pipe, &overlapped, &count, TRUE);
        if (abandoned)
            return *abandoned;
        if (!done)
            return transferFailure(GetLastError(), writing);
        if (count == 0 && !writing)
            return ProcessFailure{ProcessError::Crashed, 0};
        offset += count;
    }
    return offset;
}

// One token information class, read whole.
std::vector<std::byte> tokenInformation(HANDLE token, const TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<std::byte> storage(size);
    if (size == 0 || !GetTokenInformation(token, kind, storage.data(), size, &size))
        storage.clear();
    return storage;
}

// The two tokens of a confined launch, made from this process's own.
struct ConfinedTokens {
    Handle primary;       // what the child process is: no usable identity, no privilege
    Handle impersonation; // what its first thread is until the worker bootstrap drops it
};
std::optional<ConfinedTokens> confinedTokens() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &raw))
        return std::nullopt;
    const Handle own(raw);
    const auto userStorage = tokenInformation(own.get(), TokenUser);
    const auto groupStorage = tokenInformation(own.get(), TokenGroups);
    if (userStorage.empty() || groupStorage.empty())
        return std::nullopt;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(userStorage.data());
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groupStorage.data());
    // Every identity the token carries, except its integrity label, which is not an identity.
    std::vector<SID_AND_ATTRIBUTES> identities{{user->User.Sid, 0}};
    for (DWORD index = 0; index < groups->GroupCount; ++index)
        if ((groups->Groups[index].Attributes & SE_GROUP_INTEGRITY) == 0)
            identities.push_back({groups->Groups[index].Sid, 0});
    std::array<std::byte, SECURITY_MAX_SID_SIZE> nullStorage{};
    DWORD nullSize = static_cast<DWORD>(nullStorage.size());
    if (!CreateWellKnownSid(WinNullSid, nullptr, nullStorage.data(), &nullSize))
        return std::nullopt;
    SID_AND_ATTRIBUTES nobody{nullStorage.data(), 0};

    ConfinedTokens tokens;
    HANDLE primary = nullptr;
    if (!CreateRestrictedToken(own.get(), DISABLE_MAX_PRIVILEGE,
                               static_cast<DWORD>(identities.size()), identities.data(), 0, nullptr,
                               1, &nobody, &primary))
        return std::nullopt;
    tokens.primary.reset(primary);
    // The same access as this process, but as a restricted token: a restricted process may only
    // impersonate one.
    HANDLE same = nullptr;
    if (!CreateRestrictedToken(own.get(), DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr,
                               static_cast<DWORD>(identities.size()), identities.data(), &same))
        return std::nullopt;
    const Handle sameAccess(same);
    HANDLE impersonation = nullptr;
    if (!DuplicateTokenEx(sameAccess.get(), TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT,
                          nullptr, SecurityImpersonation, TokenImpersonation, &impersonation))
        return std::nullopt;
    tokens.impersonation.reset(impersonation);
    return tokens;
}
} // namespace

struct ProcessSupervisor::State {
    Handle process, thread, job, input, output;
    DWORD pid = 0;
    ProcessOptions options;
    bool reaped = false;
    bool waitUntil(ProcessDeadline deadline) {
        while (!reaped) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            const auto wait =
                static_cast<DWORD>(std::clamp<std::int64_t>(remaining.count(), 0, 50));
            const DWORD status = WaitForSingleObject(process.get(), wait);
            if (status == WAIT_OBJECT_0) {
                reaped = true;
                return true;
            }
            if (status == WAIT_FAILED)
                return false;
            if (Clock::now() >= deadline)
                return false;
        }
        return true;
    }
};

ProcessSupervisor::ProcessSupervisor(std::unique_ptr<State> state) : state_(std::move(state)) {}
ProcessSupervisor::~ProcessSupervisor() { stop(); }

ProcessResult<std::unique_ptr<ProcessSupervisor>>
ProcessSupervisor::launch(const ProcessOptions& options) {
    if (options.stagedOutput || options.frameArena)
        return ProcessFailure{ProcessError::Spawn, ERROR_NOT_SUPPORTED};
    std::wstring executable;
    if (!widen(options.executable, executable) || executable.empty() ||
        !std::filesystem::path(executable).is_absolute() ||
        options.addressSpaceBytes < 64ULL * 1024U * 1024U || options.openFiles < 8 ||
        options.openFiles > 4096 || options.messageGrace.count() < 0 ||
        options.messageGrace > std::chrono::seconds(5) || options.termGrace.count() < 0 ||
        options.termGrace > std::chrono::seconds(5))
        return ProcessFailure{ProcessError::ResourceLimit, 0};

    Handle input, childInput, output, childOutput;
    if (!makePipe(true, input, childInput) || !makePipe(false, output, childOutput))
        return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE gateRead = nullptr;
    HANDLE gateWrite = nullptr;
    if (!CreatePipe(&gateRead, &gateWrite, &inheritable, 0))
        return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    Handle childGate(gateRead), gate(gateWrite);
    if (!SetHandleInformation(gate.get(), HANDLE_FLAG_INHERIT, 0))
        return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    Handle nul(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                           OPEN_EXISTING, 0, nullptr));
    if (!nul.valid())
        return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};

    // Resource containment: the job exists, with its limits, before the child runs any code.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid())
        return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(options.addressSpaceBytes);
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits)))
        return ProcessFailure{ProcessError::ResourceLimit, static_cast<int>(GetLastError())};
    std::optional<ConfinedTokens> tokens;
    if (options.confined) {
        JOBOBJECT_BASIC_UI_RESTRICTIONS desktop{};
        desktop.UIRestrictionsClass =
            JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
            JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
            JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
            JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
        if (!SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &desktop,
                                     sizeof(desktop)))
            return ProcessFailure{ProcessError::ResourceLimit, static_cast<int>(GetLastError())};
        tokens = confinedTokens();
        if (!tokens)
            return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    }

    // Deterministic environment: the baseline, then explicit entries replace same-named ones.
    std::vector<std::wstring> environment{L"LC_ALL=C", L"TZ=UTC"};
    {
        wchar_t root[MAX_PATH + 1] = {};
        const DWORD length = GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH + 1);
        if (length > 0 && length <= MAX_PATH)
            environment.push_back(std::wstring(L"SystemRoot=") + root);
    }
    environment.push_back(std::wstring(kGateVariable) + L"=" +
                          std::to_wstring(reinterpret_cast<std::uintptr_t>(childGate.get())));
    if (options.confined)
        environment.push_back(std::wstring(kConfinedVariable) + L"=1");
    // The snapshot handle is private to this process; the child gets an inheritable duplicate.
    Handle childSnapshot;
    if (options.inputSnapshot) {
        HANDLE duplicate = nullptr;
        if (options.inputSnapshot->nativeHandle() == nullptr ||
            !DuplicateHandle(GetCurrentProcess(),
                             static_cast<HANDLE>(options.inputSnapshot->nativeHandle()),
                             GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
            return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
        childSnapshot.reset(duplicate);
        environment.push_back(
            std::wstring(L"BLOOM_PROCESS_SNAPSHOT=") +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(childSnapshot.get())));
    }
    for (const auto& entry : options.environment) {
        std::wstring wide;
        const auto separator = entry.find('=');
        if (!widen(entry, wide) || separator == std::string::npos || separator == 0)
            return ProcessFailure{ProcessError::Spawn, ERROR_INVALID_PARAMETER};
        const auto name = wide.substr(0, wide.find(L'=') + 1);
        auto found = std::ranges::find_if(environment, [&](const std::wstring& value) {
            return CompareStringOrdinal(value.c_str(), static_cast<int>(name.size()), name.c_str(),
                                        static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
        });
        if (found == environment.end())
            environment.push_back(std::move(wide));
        else
            *found = std::move(wide);
    }
    std::ranges::sort(environment, [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& entry : environment) {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    std::wstring commandLine;
    appendQuoted(commandLine, executable);
    for (const auto& argument : options.arguments) {
        std::wstring wide;
        if (!widen(argument, wide))
            return ProcessFailure{ProcessError::Spawn, ERROR_INVALID_PARAMETER};
        appendQuoted(commandLine, wide);
    }

    std::vector<HANDLE> inherited{childInput.get(), childOutput.get(), nul.get(), childGate.get()};
    if (childSnapshot.valid())
        inherited.push_back(childSnapshot.get());
    AttributeList attributes(1);
    if (attributes.get() == nullptr ||
        !UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(HANDLE) * inherited.size(), nullptr,
                                   nullptr))
        return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput.get();
    startup.StartupInfo.hStdOutput = childOutput.get();
    startup.StartupInfo.hStdError = nul.get();
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION created{};
    // A confined child gets no console at all: a hidden console is a second program started with
    // the child's own token, which that token cannot do.
    const DWORD creation = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                           EXTENDED_STARTUPINFO_PRESENT |
                           (tokens ? DETACHED_PROCESS : CREATE_NO_WINDOW);
    const BOOL started =
        tokens ? CreateProcessAsUserW(tokens->primary.get(), executable.c_str(), commandLine.data(),
                                      nullptr, nullptr, TRUE, creation, block.data(), nullptr,
                                      &startup.StartupInfo, &created)
               : CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                creation, block.data(), nullptr, &startup.StartupInfo, &created);
    if (!started)
        return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    auto state = std::make_unique<State>();
    state->process.reset(created.hProcess);
    state->thread.reset(created.hThread);
    state->pid = created.dwProcessId;
    state->options = options;
    // The suspended first thread starts as the launching user so the loader can open the
    // program's libraries; the worker bootstrap drops this.
    if (tokens && !SetThreadToken(&created.hThread, tokens->impersonation.get())) {
        const auto error = static_cast<int>(GetLastError());
        TerminateProcess(state->process.get(), 1);
        WaitForSingleObject(state->process.get(), INFINITE);
        return ProcessFailure{ProcessError::Spawn, error};
    }
    if (!AssignProcessToJobObject(job.get(), state->process.get())) {
        const auto error = static_cast<int>(GetLastError());
        TerminateProcess(state->process.get(), 1);
        WaitForSingleObject(state->process.get(), INFINITE);
        return ProcessFailure{ProcessError::ResourceLimit, error};
    }
    state->job = std::move(job);
    state->input = std::move(input);
    state->output = std::move(output);
    // The child holds its own copies now; the parent keeps only its private ends.
    childInput.reset();
    childOutput.reset();
    childGate.reset();
    childSnapshot.reset();
    nul.reset();
    if (ResumeThread(state->thread.get()) == static_cast<DWORD>(-1)) {
        const auto error = static_cast<int>(GetLastError());
        TerminateJobObject(state->job.get(), 1);
        WaitForSingleObject(state->process.get(), INFINITE);
        return ProcessFailure{ProcessError::Spawn, error};
    }
    state->thread.reset();
    auto child = std::unique_ptr<ProcessSupervisor>(new ProcessSupervisor(std::move(state)));
    // Release the gate. A fast external tool that never bootstraps may already have exited; its
    // output and exit status still gate the caller, exactly as on POSIX.
    constexpr char ready = 'R';
    DWORD written = 0;
    if (!WriteFile(gate.get(), &ready, 1, &written, nullptr) || written != 1) {
        if (WaitForSingleObject(child->state_->process.get(), 0) != WAIT_OBJECT_0)
            return ProcessFailure{ProcessError::Spawn, static_cast<int>(GetLastError())};
    }
    return child;
}

ProcessResult<std::size_t> ProcessSupervisor::write(std::span<const std::byte> bytes,
                                                    ProcessDeadline deadline,
                                                    const ProcessCancellation& cancel) {
    if (!state_->input.valid())
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_HANDLE};
    return transfer(state_->input.get(), {}, bytes, deadline, cancel);
}

ProcessResult<std::size_t> ProcessSupervisor::read(std::span<std::byte> bytes,
                                                   ProcessDeadline deadline,
                                                   const ProcessCancellation& cancel) {
    if (!state_->output.valid())
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_HANDLE};
    return transfer(state_->output.get(), bytes, {}, deadline, cancel);
}

ProcessResult<int> ProcessSupervisor::finish(ProcessDeadline deadline) {
    state_->input.reset();
    if (!state_->waitUntil(deadline)) {
        stop();
        return ProcessFailure{ProcessError::Timeout, 0};
    }
    DWORD code = 1;
    if (!GetExitCodeProcess(state_->process.get(), &code) || code != 0)
        return ProcessFailure{ProcessError::Crashed, static_cast<int>(code)};
    return 0;
}

void ProcessSupervisor::stop(std::span<const std::byte> message) {
    if (state_->reaped || !state_->process.valid())
        return;
    if (!message.empty() && state_->input.valid()) {
        const auto ignored = write(message, Clock::now() + state_->options.messageGrace, {});
        (void)ignored;
    }
    state_->input.reset();
    if (state_->waitUntil(Clock::now() + state_->options.messageGrace))
        return;
    // End of input is the cooperative stop request; give it the termination grace too.
    if (state_->waitUntil(Clock::now() + state_->options.termGrace))
        return;
    TerminateJobObject(state_->job.get(), 1);
    WaitForSingleObject(state_->process.get(), INFINITE);
    state_->reaped = true;
}

std::int64_t ProcessSupervisor::processId() const { return state_->pid; }
std::optional<std::uint64_t> ProcessSupervisor::shareHandle(void* handle,
                                                            const std::uint32_t access) {
    if (state_->reaped || !state_->process.valid() || handle == nullptr)
        return std::nullopt;
    HANDLE shared = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(handle), state_->process.get(),
                         &shared, access, FALSE, 0))
        return std::nullopt;
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(shared));
}
std::optional<ProcessMemory> ProcessSupervisor::memory() const {
    if (state_->reaped || state_->process.get() == nullptr)
        return std::nullopt;
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(state_->process.get(), &counters, sizeof(counters)))
        return std::nullopt;
    return ProcessMemory{static_cast<std::uint64_t>(counters.WorkingSetSize),
                         std::max<std::uint64_t>(counters.WorkingSetSize,
                                                 counters.PeakWorkingSetSize)};
}

std::optional<ProcessExit> ProcessSupervisor::waitExit(const std::chrono::milliseconds grace) {
    if (!state_->process.valid() || !state_->waitUntil(Clock::now() + grace))
        return std::nullopt;
    DWORD code = 1;
    if (!GetExitCodeProcess(state_->process.get(), &code))
        return ProcessExit{false, 1};
    return ProcessExit{false, static_cast<int>(code)};
}

bool processWorkerBootstrap() {
    wchar_t value[32] = {};
    const DWORD length = GetEnvironmentVariableW(kGateVariable, value, 32);
    if (length == 0 || length >= 32)
        return false;
    SetEnvironmentVariableW(kGateVariable, nullptr);
    std::uintptr_t raw = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (value[i] < L'0' || value[i] > L'9')
            return false;
        raw = raw * 10U + static_cast<std::uintptr_t>(value[i] - L'0');
    }
    Handle gate(reinterpret_cast<HANDLE>(raw));
    char ready = 0;
    DWORD read = 0;
    const BOOL ok = ReadFile(gate.get(), &ready, 1, &read, nullptr);
    if (!ok || read != 1 || ready != 'R')
        return false;
    wchar_t confined[4] = {};
    if (GetEnvironmentVariableW(kConfinedVariable, confined, 4) == 0)
        return true;
    SetEnvironmentVariableW(kConfinedVariable, nullptr);
    // A confined launch: while this thread still has the launching user's access, lower the
    // process to the untrusted integrity level, then drop that access for good. From here the
    // process is only what its restricted token allows.
    // The system random source opens its provider on first use, which a confined process cannot
    // do; libraries that seed themselves from it (an XML parser's hash salt) end the process when
    // it fails. Use it once now so it stays usable.
    unsigned int primed = 0;
    if (rand_s(&primed) != 0)
        return false;
    HANDLE processToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &processToken))
        return false;
    const Handle token(processToken);
    std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
    DWORD size = static_cast<DWORD>(storage.size());
    if (!CreateWellKnownSid(WinUntrustedLabelSid, nullptr, storage.data(), &size))
        return false;
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Sid = storage.data();
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    if (!SetTokenInformation(token.get(), TokenIntegrityLevel, &label,
                             sizeof(label) + GetLengthSid(storage.data())))
        return false;
    return IsTokenRestricted(token.get()) != 0 && RevertToSelf() != 0;
}
} // namespace bloom::platform

// The process supervisor's contract, on every platform, against children that are this program
// itself started in a child mode: an external tool that never bootstraps, a worker that does, exit
// codes, the end of a child's output, deadlines, cancellation, stop() and the memory limit.

#ifdef _WIN32
#define _CRT_RAND_S // rand_s, before any standard header
#endif
#include <bloom/platform/process_supervisor.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#endif

namespace {
using namespace std::chrono_literals;
using bloom::platform::ProcessError;
using bloom::platform::ProcessFailure;
using bloom::platform::ProcessOptions;
using bloom::platform::ProcessSupervisor;
using Clock = std::chrono::steady_clock;

int gFailures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++gFailures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

// --- child modes ---------------------------------------------------------------------------

int childTool() {
    // An external tool: no bootstrap, some output, a normal exit.
    std::fputs("tool output", stdout);
    std::fflush(stdout);
    return 0;
}

int childWorker() {
    if (!bloom::platform::processWorkerBootstrap())
        return 9;
    std::array<char, 4> request{};
    if (std::fread(request.data(), 1, request.size(), stdin) != request.size())
        return 8;
    std::fwrite(request.data(), 1, request.size(), stdout);
    std::fflush(stdout);
    return 0;
}

int childHang() {
    static_cast<void>(bloom::platform::processWorkerBootstrap());
    std::fputs("up", stdout);
    std::fflush(stdout);
    for (;;)
        std::this_thread::sleep_for(1s);
}

int childAllocate() {
    static_cast<void>(bloom::platform::processWorkerBootstrap());
    // Far more than the limit the parent set. Touch every page so the memory is really used.
    constexpr std::size_t kBytes = std::size_t{1} << 30;
    // Called through a volatile pointer: a compiler that sees malloc and free together may
    // remove both, and the test would then report memory it never asked for.
    void* (*volatile allocate)(std::size_t) = std::malloc;
    auto* block = static_cast<unsigned char*>(allocate(kBytes));
    if (block == nullptr) {
        std::fputs("refused", stdout);
        std::fflush(stdout);
        return 0;
    }
    for (std::size_t offset = 0; offset < kBytes; offset += 4096)
        block[offset] = 1;
    std::fputs("allocated", stdout);
    std::fflush(stdout);
    // Stay alive so a parent that polices the footprint has time to see it.
    std::this_thread::sleep_for(2s);
    std::free(block);
    return 0;
}

#ifdef _WIN32
// A confined worker reports what it could still do: one letter per attempt, upper case when the
// system allowed it. Then it proves the memory it was handed works.
int childConfined() {
    if (!bloom::platform::processWorkerBootstrap())
        return 9;
    std::string report;
    wchar_t self[MAX_PATH + 1] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    // Read a file the launching user can read: this program.
    HANDLE file = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    report.push_back(file != INVALID_HANDLE_VALUE ? 'R' : 'r');
    // Create a file beside it.
    const std::wstring made = std::wstring(self) + L".confined-probe";
    file = CreateFileW(made.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    report.push_back(file != INVALID_HANDLE_VALUE ? 'W' : 'w');
    // Open a socket.
    WSADATA data{};
    bool socketOpened = false;
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0)
        socketOpened = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) != INVALID_SOCKET;
    report.push_back(socketOpened ? 'S' : 's');
    // Start a process.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION started{};
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child-tool";
    const bool spawned =
        CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &started) != 0;
    report.push_back(spawned ? 'P' : 'p');
    // Open another process: the one that launched this.
    report.push_back(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                    static_cast<DWORD>(std::strtoul(
                        std::getenv("BLOOM_TEST_PARENT") ? std::getenv("BLOOM_TEST_PARENT") : "0",
                        nullptr, 10))) != nullptr
            ? 'O'
            : 'o');
    // The system random source still works: libraries seed themselves from it.
    unsigned int random = 0;
    report.push_back(rand_s(&random) == 0 ? 'N' : 'n');
    std::fwrite(report.data(), 1, report.size(), stdout);
    std::fflush(stdout);
    // The shared memory: two handle values on stdin, a read-only input and a writable output.
    std::array<std::uint64_t, 2> handles{};
    if (std::fread(handles.data(), sizeof(std::uint64_t), 2, stdin) != 2)
        return 8;
    const auto input = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handles[0]));
    const auto output = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handles[1]));
    const auto* in = static_cast<const char*>(MapViewOfFile(input, FILE_MAP_READ, 0, 0, 4));
    auto* out = static_cast<char*>(MapViewOfFile(output, FILE_MAP_WRITE, 0, 0, 4));
    const bool inputWritable = MapViewOfFile(input, FILE_MAP_WRITE, 0, 0, 4) != nullptr;
    if (in == nullptr || out == nullptr)
        return 7;
    for (int index = 0; index < 4; ++index)
        out[index] = static_cast<char>(in[3 - index]);
    std::fputc(inputWritable ? 'M' : 'm', stdout);
    std::fflush(stdout);
    return 0;
}
#endif

// --- parent --------------------------------------------------------------------------------

std::unique_ptr<ProcessSupervisor> launch(const std::string& self, const std::string& mode,
                                          const std::uint64_t addressSpaceBytes = 0) {
    ProcessOptions options;
    options.executable = self;
    options.arguments = {mode};
    if (addressSpaceBytes != 0)
        options.addressSpaceBytes = addressSpaceBytes;
    auto launched = ProcessSupervisor::launch(options);
    if (auto* failure = std::get_if<ProcessFailure>(&launched)) {
        std::cerr << "launch " << mode << " failed: code " << static_cast<int>(failure->code)
                  << " native " << failure->nativeCode << '\n';
        return nullptr;
    }
    return std::move(std::get<std::unique_ptr<ProcessSupervisor>>(launched));
}

struct Stream {
    std::string text;
    ProcessFailure end{};
};

// Reads a byte at a time to the first failure, as callers that stream tool output do.
Stream readToEnd(ProcessSupervisor& child, const Clock::time_point deadline) {
    Stream stream;
    std::array<std::byte, 1> buffer{};
    for (;;) {
        auto read = child.read(buffer, deadline, {});
        if (auto* failure = std::get_if<ProcessFailure>(&read)) {
            stream.end = *failure;
            return stream;
        }
        stream.text.push_back(static_cast<char>(buffer[0]));
    }
}

void testExternalTool(const std::string& self) {
    auto child = launch(self, "--child-tool");
    expect(child != nullptr, "an external tool launches");
    if (!child)
        return;
    const auto stream = readToEnd(*child, Clock::now() + 30s);
    expect(stream.text == "tool output", "the tool's whole output is read");
    expect(stream.end.code == ProcessError::Crashed && stream.end.nativeCode == 0,
           "the end of the output is reported as the end of the stream (Crashed, native 0)");
    const auto finished = child->finish(Clock::now() + 30s);
    expect(std::holds_alternative<int>(finished) && std::get<int>(finished) == 0,
           "a tool that exits normally finishes with zero");
}

void testWorkerRoundTrip(const std::string& self) {
    auto child = launch(self, "--child-worker");
    expect(child != nullptr, "a bootstrapping worker launches");
    if (!child)
        return;
    const std::array<std::byte, 4> request{std::byte{'p'}, std::byte{'i'}, std::byte{'n'},
                                           std::byte{'g'}};
    const auto wrote = child->write(request, Clock::now() + 30s, {});
    expect(std::holds_alternative<std::size_t>(wrote) && std::get<std::size_t>(wrote) == 4,
           "the request is written whole");
    std::array<std::byte, 4> reply{};
    const auto read = child->read(reply, Clock::now() + 30s, {});
    expect(std::holds_alternative<std::size_t>(read) && std::get<std::size_t>(read) == 4 &&
               std::memcmp(reply.data(), request.data(), 4) == 0,
           "the worker's reply is read whole");
    const auto finished = child->finish(Clock::now() + 30s);
    expect(std::holds_alternative<int>(finished) && std::get<int>(finished) == 0,
           "the worker finishes with zero");
}

void testExitCode(const std::string& self) {
    auto child = launch(self, "--child-exit3");
    expect(child != nullptr, "a failing tool launches");
    if (!child)
        return;
    static_cast<void>(readToEnd(*child, Clock::now() + 30s));
    const auto finished = child->finish(Clock::now() + 30s);
    const auto* failure = std::get_if<ProcessFailure>(&finished);
    // The native code is the system's own: the exit code on Windows, the wait status on POSIX.
#ifdef _WIN32
    const bool three = failure != nullptr && failure->nativeCode == 3;
#else
    // A plain int of its own: some systems' wait macros take the address of their argument as a
    // pointer to non-const int.
    int status = failure != nullptr ? failure->nativeCode : 0;
    const bool three = failure != nullptr && WIFEXITED(status) && WEXITSTATUS(status) == 3;
#endif
    expect(failure != nullptr && failure->code == ProcessError::Crashed && three,
           "a nonzero exit is reported as Crashed with the exit code");
}

void testDeadlineCancelAndStop(const std::string& self) {
    auto child = launch(self, "--child-hang");
    expect(child != nullptr, "a hanging worker launches");
    if (!child)
        return;
    std::array<std::byte, 2> up{};
    const auto first = child->read(up, Clock::now() + 30s, {});
    expect(std::holds_alternative<std::size_t>(first), "the hanging worker's first bytes arrive");
    std::array<std::byte, 1> buffer{};
    const auto timed = child->read(buffer, Clock::now() + 200ms, {});
    const auto* timeout = std::get_if<ProcessFailure>(&timed);
    expect(timeout != nullptr && timeout->code == ProcessError::Timeout,
           "a read with nothing to read ends at its deadline");
    const auto cancelled = child->read(buffer, Clock::now() + 30s, [] { return true; });
    const auto* cancel = std::get_if<ProcessFailure>(&cancelled);
    expect(cancel != nullptr && cancel->code == ProcessError::Cancelled,
           "a cancelled read reports cancellation");
    expect(!child->waitExit(0ms).has_value(), "the hanging worker is still running");
    child->stop();
    expect(child->waitExit(0ms).has_value() || !child->memory().has_value(),
           "stop() ends and reaps a worker that ignores the request");
}

void testMemoryLimit(const std::string& self) {
    constexpr std::uint64_t kLimit = 256ULL * 1024 * 1024;
    auto child = launch(self, "--child-allocate", kLimit);
    expect(child != nullptr, "a worker with a memory limit launches");
    if (!child)
        return;
    const auto stream = readToEnd(*child, Clock::now() + 60s);
    expect(stream.text != "allocated" || stream.end.code == ProcessError::ResourceLimit,
           "a worker cannot use four times its memory limit unnoticed");
    child->stop();
}

// A confined launch: Windows gives the child no ambient authority and hands it memory by handle;
// Linux and macOS confine inside the worker and refuse the option.
void testConfined(const std::string& self) {
    ProcessOptions options;
    options.executable = self;
    options.arguments = {"--child-confined"};
    options.confined = true;
#ifdef _WIN32
    options.environment = {"BLOOM_TEST_PARENT=" + std::to_string(GetCurrentProcessId())};
    auto launched = ProcessSupervisor::launch(options);
    auto* started = std::get_if<std::unique_ptr<ProcessSupervisor>>(&launched);
    expect(started != nullptr, "a confined worker launches");
    if (started == nullptr) {
        const auto& failure = std::get<ProcessFailure>(launched);
        std::cerr << "confined launch: code " << static_cast<int>(failure.code) << " native "
                  << failure.nativeCode << '\n';
        return;
    }
    auto& child = **started;
    std::array<std::byte, 6> report{};
    const auto read = child.read(report, Clock::now() + 30s, {});
    const std::string text(reinterpret_cast<const char*>(report.data()), report.size());
    expect(std::holds_alternative<std::size_t>(read), "the confined worker reports");
    if (const auto* failure = std::get_if<ProcessFailure>(&read)) {
        std::cerr << "confined report: code " << static_cast<int>(failure->code) << " native "
                  << failure->nativeCode;
        if (const auto exit = child.waitExit(5000ms))
            std::cerr << ", worker " << bloom::platform::describeProcessExit(*exit);
        std::cerr << '\n';
        return;
    }
    expect(text == "rwspoN",
           "a confined worker cannot read a file, create one, open a socket, start a process or "
           "open another process, and can still draw random numbers (got " +
               text + ")");
    const HANDLE input =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
    const HANDLE output =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
    auto* in = static_cast<char*>(MapViewOfFile(input, FILE_MAP_WRITE, 0, 0, 4));
    const auto* out = static_cast<const char*>(MapViewOfFile(output, FILE_MAP_READ, 0, 0, 4));
    expect(in != nullptr && out != nullptr, "shared memory is created");
    if (in == nullptr || out == nullptr)
        return;
    std::memcpy(in, "abcd", 4);
    const auto sharedInput = child.shareHandle(input, SECTION_MAP_READ | SECTION_QUERY);
    const auto sharedOutput =
        child.shareHandle(output, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY);
    expect(sharedInput && sharedOutput, "memory is handed to the confined worker");
    const std::array<std::uint64_t, 2> handles{sharedInput.value_or(0), sharedOutput.value_or(0)};
    const auto wrote = child.write(std::as_bytes(std::span(handles)), Clock::now() + 30s, {});
    expect(std::holds_alternative<std::size_t>(wrote), "the handle values are sent");
    std::array<std::byte, 1> writable{};
    const auto mapped = child.read(writable, Clock::now() + 30s, {});
    expect(std::holds_alternative<std::size_t>(mapped) && writable[0] == std::byte{'m'},
           "the worker cannot write to memory it was handed read-only");
    const auto finished = child.finish(Clock::now() + 30s);
    expect(std::holds_alternative<int>(finished) && std::get<int>(finished) == 0,
           "the confined worker finishes with zero");
    expect(std::memcmp(out, "dcba", 4) == 0, "the worker's result arrives in the shared memory");
    UnmapViewOfFile(in);
    UnmapViewOfFile(out);
    CloseHandle(input);
    CloseHandle(output);
    std::error_code ignored;
    expect(!std::filesystem::exists(self + ".confined-probe", ignored),
           "the confined worker left no file behind");
#else
    const auto launched = ProcessSupervisor::launch(options);
    const auto* failure = std::get_if<ProcessFailure>(&launched);
    expect(failure != nullptr && failure->code == ProcessError::Spawn,
           "a launch-time confinement request is refused where workers confine themselves");
    auto unconfined = launch(self, "--child-hang");
    expect(unconfined != nullptr && !unconfined->shareHandle(nullptr, 0).has_value(),
           "no handle is shared where descriptors travel over a socket");
#endif
}
} // namespace

int main(const int argc, char** argv) {
    if (argc >= 2) {
        const std::string_view mode = argv[1];
        if (mode == "--child-tool")
            return childTool();
        if (mode == "--child-worker")
            return childWorker();
        if (mode == "--child-exit3")
            return 3;
        if (mode == "--child-hang")
            return childHang();
        if (mode == "--child-allocate")
            return childAllocate();
#ifdef _WIN32
        if (mode == "--child-confined")
            return childConfined();
#endif
        return 2;
    }
    std::error_code error;
    const auto self = std::filesystem::absolute(argv[0], error);
    if (error)
        return EXIT_FAILURE;
    std::string path = self.string();
#ifdef _WIN32
    if (self.extension().empty())
        path += ".exe";
#endif
    testExternalTool(path);
    testWorkerRoundTrip(path);
    testExitCode(path);
    testDeadlineCancelAndStop(path);
    testMemoryLimit(path);
    testConfined(path);
    if (gFailures != 0) {
        std::cerr << gFailures << " process supervisor expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

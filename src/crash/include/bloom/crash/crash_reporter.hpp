#pragma once

// Starting Crashpad's out-of-process crash handler in a Bloom program (issue #736;
// docs/architecture/platform-support.md, "Crash Reports").
//
// The application, bloom-cli and bloom-mcp call startCrashReporter() first thing in main. It
//
//   * prepares the program's text report in the crash folder's `sessions` folder: version, build,
//     platform, OS version, and later the GPU and driver and the last log lines, written as they
//     happen by ordinary code, never in a signal handler;
//   * starts bloom-crash-handler, the program beside this one, with no upload address; Crashpad is
//     built without any network transport, so nothing leaves the machine;
//   * hands the handler the text report as an attachment, which the handler copies beside the
//     minidump when the program crashes;
//   * collects and prunes earlier crashes on a background thread (crash_reports.hpp).
//
// The handler is a separate process, so a crash on any thread, an abort, a stack overflow or a
// crash inside a GPU driver still leaves a minidump with every thread's stack.

#include <filesystem>
#include <string>

namespace bloom::crash {

struct ReporterOptions final {
    // "bloom", "bloom-cli" or "bloom-mcp": the name the report and its folder carry.
    std::string program;
    // The running program; the handler is looked for beside it.
    std::filesystem::path executable;
    // Empty: crashDirectory().
    std::filesystem::path directory;
    // Collect and prune earlier crashes on a background thread.
    bool collect = true;
};

enum class ReporterState : unsigned char {
    Running,
    // No handler runs: no crash folder, no handler beside the program, a build without Crashpad,
    // or the handler did not start. The program runs on; a crash is not recorded.
    Unavailable,
};

struct ReporterStatus final {
    ReporterState state = ReporterState::Unavailable;
    std::filesystem::path directory;
    // Why the reporter is unavailable; empty when it runs.
    std::string detail;
};

// Starts the reporter once per process; a second call returns the first call's status.
ReporterStatus startCrashReporter(const ReporterOptions& options);
[[nodiscard]] ReporterStatus crashReporterStatus();

// Waits for the background collection to finish (the application's notice reads the folder after
// it; call it off the UI thread).
void waitForCollection();

// A clean exit: removes the program's prepared text report and waits for the collection. A
// program that does not call it leaves its text report behind, which the next collection removes.
void finishCrashReporter() noexcept;

// Copies every line written to std::cerr (and std::clog) into the text report as well as to
// standard error. bloom-cli and bloom-mcp report their diagnostics there.
void captureStandardError();

// The text report's header lines for this process, as the report shows them. Starts nothing.
[[nodiscard]] std::string describeThisProcess(const std::string& program);

} // namespace bloom::crash

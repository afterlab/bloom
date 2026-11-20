// Checks what a deliberate crash left in a crash folder (issue #736), after crash_test.cmake ran
// the crashing program:
//
//   bloom_crash_check <mode> <crash folder> <program>
//
// It collects the folder as the next start of a Bloom program does, then requires exactly one
// crash of <program> with a minidump that holds every thread's stack and the right exception, a
// text report with the version, build, platform, OS version, GPU line and the last log lines, and
// that the report is offered once: offered on the first look, not on the second. Mode `clean`
// requires that nothing was recorded and that the program removed its prepared text report.

#include <bloom/core/version.hpp>
#include <bloom/crash/crash_reports.hpp>
#include <bloom/crash/minidump_summary.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << '\n';
        ++failures;
    }
}

[[nodiscard]] bool contains(const std::string_view text, const std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// The id after the last "<label> " in the report's log lines.
[[nodiscard]] std::optional<std::uint64_t> loggedThread(const std::string& report,
                                                        const std::string& label) {
    const auto at = report.rfind(label + " ");
    if (at == std::string::npos)
        return std::nullopt;
    std::istringstream stream(report.substr(at + label.size() + 1));
    std::uint64_t id = 0;
    if (!(stream >> id))
        return std::nullopt;
    return id;
}

[[nodiscard]] std::vector<std::uint32_t> expectedCodes(const std::string_view mode,
                                                       const std::string& platform) {
    const bool abort = mode == "abort";
    const bool overflow = mode == "stack-overflow";
    if (platform == "linux")
        // A stack overflow is a SIGSEGV on the guard page, handled on the alternate stack.
        return {abort ? 6U : 11U};
    if (platform == "macos")
        // An abort is a signal, which Crashpad records as code 0 with the signal (EXC_CRASH);
        // a null write and a stack overflow are EXC_BAD_ACCESS.
        return abort ? std::vector<std::uint32_t>{0U, 10U} : std::vector<std::uint32_t>{1U};
    if (platform == "windows")
        // STATUS_FATAL_APP_EXIT is what Crashpad's SIGABRT handler raises.
        return {abort ? 0x40000015U : overflow ? 0xC00000FDU : 0xC0000005U};
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: bloom_crash_check <mode> <crash folder> <program>\n";
        return 2;
    }
    const std::string mode = argv[1];
    const std::filesystem::path folder = argv[2];
    const std::string program = argv[3];

    const auto collected = bloom::crash::collectReports(folder, std::chrono::system_clock::now());
    for (const auto& problem : collected.problems)
        std::cerr << "collection: " << problem << '\n';
    const auto reports = bloom::crash::listReports(folder);

    if (mode == "clean") {
        expect(reports.empty(), "a clean exit records no crash");
        std::error_code error;
        bool sessionLeft = false;
        for (std::filesystem::directory_iterator entry(folder / "sessions", error), end;
             !error && entry != end; entry.increment(error))
            sessionLeft = true;
        expect(!sessionLeft, "a clean exit removes the prepared text report");
        return failures == 0 ? 0 : 1;
    }

    expect(collected.collected == 1, "the collection made one crash folder");
    expect(reports.size() == 1, "the crash folder holds one crash");
    if (reports.size() != 1)
        return 1;
    const auto& report = reports.front();
    expect(report.program == program,
           "the crash is " + program + "'s, not " + report.program + "'s");
    expect(!report.helperProcess, "the crash is the program's own");

    const auto summary = bloom::crash::readMinidumpSummary(report.minidump());
    expect(summary.has_value(), "the minidump can be read");
    const std::string text = bloom::crash::readTextReport(report);
    std::cerr << "---- report.txt\n" << text << "----\n";
    if (summary) {
        std::cerr << "minidump: " << summary->threads.size() << " threads, "
                  << bloom::crash::describeException(*summary) << ", platform " << summary->platform
                  << '\n';
        expect(!summary->threads.empty(), "the minidump lists the threads");
        for (const auto& thread : summary->threads)
            expect(thread.stackBytes > 0,
                   "thread " + std::to_string(thread.id) + " has its stack in the minidump");
        const auto codes = expectedCodes(mode, summary->platform);
        expect(summary->exceptionCode &&
                   std::ranges::find(codes, *summary->exceptionCode) != codes.end(),
               "the exception is the one the mode causes");
        // The test program logs its threads' ids; bloom-cli's script cannot.
        if (program == "bloom" && (mode == "worker-thread" || mode == "stack-overflow")) {
            expect(summary->threads.size() >= 2,
                   "the minidump lists the main and the worker thread");
            const auto main = loggedThread(text, "main thread");
            expect(summary->crashedThreadId && main &&
                       *summary->crashedThreadId != static_cast<std::uint32_t>(*main),
                   "the crashed thread is not the main thread");
        }
        if (program == "bloom" &&
            (mode == "null-write" || mode == "worker-thread" || mode == "stack-overflow")) {
            const auto crashing = loggedThread(text, "crashing thread");
            expect(summary->crashedThreadId && crashing &&
                       *summary->crashedThreadId == static_cast<std::uint32_t>(*crashing),
                   "the crashed thread is the one that was made to crash");
        }
    }

    expect(contains(text, "Bloom quit unexpectedly"), "the report says Bloom quit unexpectedly");
    expect(contains(text, "Program: " + program + "\n"), "the report names the program");
    expect(contains(text, "Version: " + std::string(bloom::core::version()) + "\n"),
           "the report names the version");
    for (const std::string_view line : {"Build: ", "Platform: ", "OS version: ", "GPU: ",
                                        "Crashed: ", "Crash: ", "Minidump: ", "Last log lines:"})
        expect(contains(text, std::string("\n") + std::string(line)),
               "the report has a '" + std::string(line) + "' line");
    if (program == "bloom") {
        expect(contains(text, "GPU: Test GPU (discrete), driver test 1.0, Vulkan 1.3\n"),
               "the report names the GPU and driver");
        expect(contains(text, " log line before the crash\n"), "the report has the last log lines");
    }

    // The next start offers it, and the start after that does not.
    const auto offered = bloom::crash::reportsToOffer(folder);
    expect(offered.size() == 1, "the next start offers the crash");
    if (!offered.empty())
        expect(bloom::crash::markOffered(offered.front()), "the offer is recorded");
    expect(bloom::crash::reportsToOffer(folder).empty(), "a second start no longer offers it");
    expect(bloom::crash::listReports(folder).size() == 1, "the offered report stays in the folder");
    return failures == 0 ? 0 : 1;
}

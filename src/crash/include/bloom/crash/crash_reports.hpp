#pragma once

// The crash folder and the reports in it (issue #736; docs/architecture/platform-support.md,
// "Crash Reports"). Qt-free; the application's next-start notice (src/ui) reads it, and every
// Bloom program collects and prunes it when it starts (crash_reporter.hpp).
//
// The crash folder is `crashes` in Bloom's per-user data folder:
//
//   crashes/database/        Crashpad's database, written by the crash handler
//   crashes/sessions/        the text report each running program keeps ready
//   crashes/<when>_<program>_<id>/minidump.dmp, report.txt
//                            one folder per crash, made from the database by collectReports()
//
// Nothing in this module sends anything anywhere.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::crash {

// At most this many crashes are kept, and none older than kMaximumReportAge.
inline constexpr std::size_t kMaximumKeptReports = 20;
inline constexpr std::chrono::hours kMaximumReportAge{24 * 30};
// The text report keeps this many of the last log lines.
inline constexpr std::size_t kReportLogLines = 100;

// The test-only variable that moves the crash folder (cmake/BloomTesting.cmake sets it for every
// test, so a test run never writes into the user's crash folder). Nothing in the product sets it;
// a value that is not an absolute path is ignored.
inline constexpr std::string_view kTestCrashDirectoryVariable = "BLOOM_TEST_CRASH_DIRECTORY";

// The crash folder: the test variable's folder when it is set, else `crashes` in
// platform::userDataDirectory(). Empty when neither resolves. Not created here.
[[nodiscard]] std::optional<std::filesystem::path> crashDirectory();

struct CrashReport final {
    std::filesystem::path folder;
    // The program that crashed: "bloom", "bloom-cli" or "bloom-mcp".
    std::string program;
    // When the crash was collected into its folder, from the folder's name.
    std::chrono::system_clock::time_point time;
    // A process the program started crashed (a macOS child inherits the program's crash
    // handler), not the program itself. Kept, never offered as "Bloom quit unexpectedly".
    bool helperProcess = false;
    // The application has offered it once already.
    bool offered = false;

    [[nodiscard]] std::filesystem::path minidump() const { return folder / "minidump.dmp"; }
    [[nodiscard]] std::filesystem::path textReport() const { return folder / "report.txt"; }
};

struct CollectResult final {
    std::size_t collected = 0;
    std::size_t pruned = 0;
    // What could not be done, one line each; nothing here stops the program.
    std::vector<std::string> problems;
};

// Makes a folder for every finished report in the database (its minidump and its text report),
// removes it from the database, removes the prepared text reports of programs that have exited,
// and prunes. Several programs may call it at once: it holds a lock file in the folder. Does file
// I/O: never call it on a UI thread.
CollectResult collectReports(const std::filesystem::path& directory,
                             std::chrono::system_clock::time_point now);

// Keeps the newest kMaximumKeptReports crash folders and removes any older than
// kMaximumReportAge. Returns how many it removed.
std::size_t pruneReports(const std::filesystem::path& directory,
                         std::chrono::system_clock::time_point now);

// The crash folders, newest first.
[[nodiscard]] std::vector<CrashReport> listReports(const std::filesystem::path& directory);

// The crashes of Bloom's own programs the application has not offered yet, newest first.
[[nodiscard]] std::vector<CrashReport> reportsToOffer(const std::filesystem::path& directory);

// Records that the application offered the report, so the next start does not offer it again.
bool markOffered(const CrashReport& report);

// The report's text (bounded; empty when it cannot be read).
[[nodiscard]] std::string readTextReport(const CrashReport& report);

// The address of a new GitHub issue in Bloom's repository with the text report in its body. Only
// the text report: the minidump is attached by hand. The log lines are shortened from the oldest
// until the address fits the length a browser and GitHub accept.
[[nodiscard]] std::string newIssueAddress(std::string_view reportText);
inline constexpr std::size_t kMaximumIssueAddressLength = 8000;

} // namespace bloom::crash

#pragma once

// Internal to the crash module: turning a report in Crashpad's database into a crash folder.

#include <bloom/crash/crash_reports.hpp>
#include <bloom/crash/minidump_summary.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::crash::detail {

struct CrashFacts final {
    std::chrono::system_clock::time_point time;
    std::optional<MinidumpSummary> summary;
    bool helperProcess = false;
};

// The finished text report: the program's prepared header, the crash, and the last
// kReportLogLines log lines.
[[nodiscard]] std::string composeTextReport(std::string_view session, const CrashFacts& facts);
[[nodiscard]] std::string sessionValue(std::string_view session, std::string_view key);

// Writes <directory>/<name>/ with the minidump's copy and the text report, through a partial
// folder renamed at the end so a reader never sees half a report.
bool writeReportFolder(const std::filesystem::path& directory, const std::string& name,
                       const std::filesystem::path& minidump, const std::string& text,
                       bool helperProcess, std::string& problem);

// Moves every finished report out of the database at <directory>/database. Without Crashpad in
// the build there is no database and nothing to do.
void collectDatabase(const std::filesystem::path& directory, CollectResult& result);

} // namespace bloom::crash::detail

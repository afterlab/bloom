#include "crash_collect.hpp"

#include "crash_internal.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#if BLOOM_CRASH_HAVE_CRASHPAD
#include "client/crash_report_database.h"
#endif

namespace bloom::crash::detail {
namespace {

constexpr std::uintmax_t kMaximumSessionBytes = 1U << 20;

[[nodiscard]] std::string readSmallFile(const std::filesystem::path& file) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > kMaximumSessionBytes)
        return {};
    std::ifstream stream(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// The program's text report among the report's attachments: the one .txt file.
[[nodiscard]] std::string sessionAttachment(const std::filesystem::path& attachments) {
    std::error_code error;
    for (std::filesystem::directory_iterator entry(attachments, error), end; !error && entry != end;
         entry.increment(error)) {
        if (entry->path().extension() == ".txt")
            return readSmallFile(entry->path());
    }
    return {};
}

[[nodiscard]] std::optional<std::uint32_t> parseProcess(const std::string& text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

// A program name fit for a folder name; anything else is "unknown".
[[nodiscard]] std::string folderProgram(const std::string& program) {
    const bool plain = !program.empty() && program.size() <= 32 &&
                       std::ranges::all_of(program, [](const char character) {
                           return (character >= 'a' && character <= 'z') ||
                                  (character >= '0' && character <= '9') || character == '-';
                       });
    return plain ? program : std::string("unknown");
}

} // namespace

#if BLOOM_CRASH_HAVE_CRASHPAD

void collectDatabase(const std::filesystem::path& directory, CollectResult& result) {
    const auto databasePath = directory / "database";
    std::error_code error;
    if (!std::filesystem::is_directory(databasePath, error))
        return;
    const auto database = crashpad::CrashReportDatabase::InitializeWithoutCreating(
        base::FilePath(databasePath.native()));
    if (!database) {
        result.problems.push_back("cannot open the crash database");
        return;
    }
    std::vector<crashpad::CrashReportDatabase::Report> reports;
    std::vector<crashpad::CrashReportDatabase::Report> completed;
    if (database->GetPendingReports(&reports) != crashpad::CrashReportDatabase::kNoError)
        result.problems.push_back("cannot list the crash database's pending reports");
    if (database->GetCompletedReports(&completed) != crashpad::CrashReportDatabase::kNoError)
        result.problems.push_back("cannot list the crash database's completed reports");
    reports.insert(reports.end(), completed.begin(), completed.end());

    for (const auto& report : reports) {
        const std::string uuid = report.uuid.ToString();
        // Crashpad keeps a report's attachments in <database>/attachments/<uuid>.
        const std::string session = sessionAttachment(databasePath / "attachments" / uuid);
        const std::filesystem::path minidump(report.file_path.value());

        CrashFacts facts;
        facts.time = std::chrono::system_clock::from_time_t(report.creation_time);
        facts.summary = readMinidumpSummary(minidump);
        const auto sessionProcess = parseProcess(sessionValue(session, "Process"));
        facts.helperProcess = facts.summary && facts.summary->processId && sessionProcess &&
                              *facts.summary->processId != *sessionProcess;

        const std::string program = folderProgram(sessionValue(session, "Program"));
        const std::string name = folderTime(facts.time) + "_" + program + "_" + uuid.substr(0, 8);
        std::string problem;
        if (!writeReportFolder(directory, name, minidump, composeTextReport(session, facts),
                               facts.helperProcess, problem)) {
            result.problems.push_back(std::move(problem));
            continue;
        }
        if (database->DeleteReport(report.uuid) != crashpad::CrashReportDatabase::kNoError)
            result.problems.push_back("collected " + name +
                                      " but could not remove it from the database");
        ++result.collected;
    }
}

#else

void collectDatabase(const std::filesystem::path&, CollectResult&) {}

#endif

} // namespace bloom::crash::detail

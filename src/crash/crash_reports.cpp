#include <bloom/crash/crash_reports.hpp>

#include "crash_collect.hpp"
#include "crash_internal.hpp"

#include <bloom/platform/user_data_directory.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace bloom::crash {
namespace {

constexpr std::string_view kOfferedMarker = "offered";
constexpr std::string_view kHelperMarker = "helper";
constexpr std::string_view kLogHeading = "Last log lines:";
constexpr std::uintmax_t kMaximumTextReportBytes = 1U << 20;

// "<YYYY-MM-DD_HH-MM-SS>_<program>_<id>": the time, the program and an id that keeps two crashes
// of one second apart.
struct FolderName final {
    std::chrono::system_clock::time_point time;
    std::string program;
};

[[nodiscard]] std::optional<FolderName> parseFolderName(const std::string& name) {
    if (name.size() < 23 || name[19] != '_')
        return std::nullopt;
    const auto time = detail::parseFolderTime(std::string_view(name).substr(0, 19));
    const auto last = name.rfind('_');
    if (!time || last == std::string::npos || last <= 20)
        return std::nullopt;
    return FolderName{.time = *time, .program = name.substr(20, last - 20)};
}

[[nodiscard]] bool pathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

[[nodiscard]] std::string percentEncoded(const std::string_view text) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            out += character;
        } else {
            out += '%';
            out += digits[byte >> 4U];
            out += digits[byte & 0xFU];
        }
    }
    return out;
}

[[nodiscard]] std::string issueAddress(const std::string_view title,
                                       const std::string_view report) {
    std::string body =
        "**What were you doing when Bloom quit?**\n\n\n\n"
        "**Crash report**\n\n"
        "The minidump is in the crash folder beside this report; attach it here if you choose.\n\n"
        "```\n";
    body += report;
    if (!report.ends_with('\n'))
        body += '\n';
    body += "```\n";
    return "https://github.com/afterlab/bloom/issues/new?title=" + percentEncoded(title) +
           "&body=" + percentEncoded(body);
}

[[nodiscard]] std::string headerValue(const std::string_view report, const std::string_view key) {
    std::istringstream lines{std::string(report)};
    std::string line;
    const std::string prefix = std::string(key) + ": ";
    while (std::getline(lines, line)) {
        if (line == kLogHeading)
            break;
        if (line.starts_with(prefix))
            return line.substr(prefix.size());
    }
    return {};
}

} // namespace

std::optional<std::filesystem::path> crashDirectory() {
    if (const char* value = std::getenv(
            std::string(kTestCrashDirectoryVariable).c_str()); // NOLINT(concurrency-mt-unsafe)
        value != nullptr && *value != '\0') {
        const std::filesystem::path path(value);
        if (path.is_absolute())
            return path;
    }
    if (const auto data = platform::userDataDirectory())
        return *data / "crashes";
    return std::nullopt;
}

std::vector<CrashReport> listReports(const std::filesystem::path& directory) {
    std::vector<CrashReport> reports;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code typeError;
        if (!entry->is_directory(typeError))
            continue;
        const auto parsed = parseFolderName(entry->path().filename().string());
        if (!parsed)
            continue;
        CrashReport report{.folder = entry->path(),
                           .program = parsed->program,
                           .time = parsed->time,
                           .helperProcess = pathExists(entry->path() / std::string(kHelperMarker)),
                           .offered = pathExists(entry->path() / std::string(kOfferedMarker))};
        if (!pathExists(report.minidump()) && !pathExists(report.textReport()))
            continue;
        reports.push_back(std::move(report));
    }
    std::ranges::sort(reports, [](const CrashReport& left, const CrashReport& right) {
        if (left.time != right.time)
            return left.time > right.time;
        return left.folder.filename() > right.folder.filename();
    });
    return reports;
}

std::size_t pruneReports(const std::filesystem::path& directory,
                         const std::chrono::system_clock::time_point now) {
    const auto reports = listReports(directory);
    std::size_t removed = 0;
    for (std::size_t index = 0; index < reports.size(); ++index) {
        const bool tooMany = index >= kMaximumKeptReports;
        const bool tooOld = reports[index].time + kMaximumReportAge < now;
        if (!tooMany && !tooOld)
            continue;
        std::error_code error;
        std::filesystem::remove_all(reports[index].folder, error);
        if (!error)
            ++removed;
    }
    return removed;
}

std::vector<CrashReport> reportsToOffer(const std::filesystem::path& directory) {
    auto reports = listReports(directory);
    std::erase_if(reports,
                  [](const CrashReport& report) { return report.offered || report.helperProcess; });
    return reports;
}

bool markOffered(const CrashReport& report) {
    std::ofstream marker(report.folder / std::string(kOfferedMarker), std::ios::binary);
    marker << "The application offered this report.\n";
    return static_cast<bool>(marker);
}

std::string readTextReport(const CrashReport& report) {
    std::error_code error;
    const auto size = std::filesystem::file_size(report.textReport(), error);
    if (error || size > kMaximumTextReportBytes)
        return {};
    std::ifstream stream(report.textReport(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::string newIssueAddress(const std::string_view reportText) {
    std::string title = "Bloom quit unexpectedly";
    if (const auto program = headerValue(reportText, "Program"); !program.empty()) {
        title += " (" + program;
        if (const auto version = headerValue(reportText, "Version"); !version.empty())
            title += " " + version;
        title += ")";
    }
    std::string report(reportText);
    std::string address = issueAddress(title, report);
    // Shorten the log from its oldest line, then the report itself, until the address fits.
    const auto heading = report.find(std::string(kLogHeading) + "\n");
    while (address.size() > kMaximumIssueAddressLength) {
        const auto logStart =
            heading == std::string::npos ? std::string::npos : heading + kLogHeading.size() + 1;
        if (logStart != std::string::npos && logStart < report.size()) {
            const auto lineEnd = report.find('\n', logStart);
            report.erase(logStart,
                         lineEnd == std::string::npos ? std::string::npos : lineEnd - logStart + 1);
        } else {
            report.resize(report.size() * 3 / 4);
            report += "\n(shortened; the whole report is in the crash folder)\n";
        }
        address = issueAddress(title, report);
    }
    return address;
}

CollectResult collectReports(const std::filesystem::path& directory,
                             const std::chrono::system_clock::time_point now) {
    CollectResult result;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        result.problems.push_back("cannot create the crash folder: " + error.message());
        return result;
    }
    detail::HeldFile lock;
    if (!lock.openWaiting(directory / "collect.lock")) {
        result.problems.push_back("cannot lock the crash folder");
        return result;
    }
    detail::collectDatabase(directory, result);

    // The text reports of programs that are no longer running: one that exited cleanly removed
    // its own, so these belong to programs that crashed (the handler copied them) or were killed.
    for (std::filesystem::directory_iterator entry(directory / "sessions", error), end;
         !error && entry != end; entry.increment(error)) {
        if (!detail::heldByAnotherProcess(entry->path())) {
            std::error_code ignored;
            std::filesystem::remove(entry->path(), ignored);
        }
    }
    result.pruned = pruneReports(directory, now);
    lock.close(false);
    return result;
}

namespace detail {

std::string composeTextReport(const std::string_view session, const CrashFacts& facts) {
    std::string header;
    std::vector<std::string> log;
    {
        std::istringstream lines{std::string(session)};
        std::string line;
        bool inLog = false;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (inLog) {
                log.push_back(std::move(line));
            } else if (line == kLogHeading) {
                inLog = true;
            } else if (!line.empty()) {
                header += line + "\n";
            }
        }
    }
    if (header.empty())
        header = "Program: unknown (the program's own text report was not found)\n";
    if (log.size() > kReportLogLines)
        log.erase(log.begin(), log.end() - static_cast<std::ptrdiff_t>(kReportLogLines));

    std::string text = facts.helperProcess ? "A process Bloom started quit unexpectedly\n\n"
                                           : "Bloom quit unexpectedly\n\n";
    text += header;
    text += "Crashed: " + readableTime(facts.time) + "\n";
    if (facts.summary) {
        text += "Crash: " + describeException(*facts.summary);
        if (facts.summary->crashedThreadId)
            text += " on thread " + std::to_string(*facts.summary->crashedThreadId);
        text += ", " + std::to_string(facts.summary->threads.size()) + " threads\n";
        // #756: a thread that was exiting at the crash has no stack in the minidump; the report
        // says so rather than leaving a reader to take it for a capture that failed.
        if (const auto exited = exitedThreads(*facts.summary); !exited.empty()) {
            text += "Exited at the crash (no stack): thread";
            text += exited.size() == 1 ? " " : "s ";
            for (std::size_t index = 0; index < exited.size(); ++index)
                text += (index == 0 ? "" : ", ") + std::to_string(exited[index]);
            text += "\n";
        }
        if (facts.helperProcess && facts.summary->processId)
            text += "Crashed process: " + std::to_string(*facts.summary->processId) + "\n";
    } else {
        text += "Crash: the minidump could not be read\n";
    }
    text += "Minidump: minidump.dmp, in the same folder as this report\n";
    text += "\n";
    text += kLogHeading;
    text += "\n";
    if (log.empty())
        text += "(none)\n";
    for (const auto& line : log)
        text += line + "\n";
    return text;
}

std::string sessionValue(const std::string_view session, const std::string_view key) {
    return headerValue(session, key);
}

bool writeReportFolder(const std::filesystem::path& directory, const std::string& name,
                       const std::filesystem::path& minidump, const std::string& text,
                       const bool helperProcess, std::string& problem) {
    const auto partial = directory / ("." + name + ".partial");
    const auto finalFolder = directory / name;
    std::error_code error;
    std::filesystem::remove_all(partial, error);
    std::filesystem::create_directories(partial, error);
    if (error) {
        problem = "cannot create " + partial.string() + ": " + error.message();
        return false;
    }
    std::filesystem::copy_file(minidump, partial / "minidump.dmp",
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        problem = "cannot copy " + minidump.string() + ": " + error.message();
        std::filesystem::remove_all(partial, error);
        return false;
    }
    {
        std::ofstream report(partial / "report.txt", std::ios::binary);
        report << text;
        if (helperProcess)
            std::ofstream(partial / std::string(kHelperMarker), std::ios::binary)
                << "A process Bloom started crashed, not Bloom itself.\n";
        if (!report) {
            problem = "cannot write the text report";
            std::filesystem::remove_all(partial, error);
            return false;
        }
    }
    std::filesystem::rename(partial, finalFolder, error);
    if (error) {
        problem = "cannot finish " + finalFolder.string() + ": " + error.message();
        std::filesystem::remove_all(partial, error);
        return false;
    }
    return true;
}

} // namespace detail

} // namespace bloom::crash

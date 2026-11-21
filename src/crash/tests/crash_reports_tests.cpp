// The crash folder without a crash (issue #736): reading a minidump's streams, the text report a
// collection writes, which reports the application offers and that it offers each once, pruning
// to 20 crashes and 30 days, the prefilled GitHub address, the held text report file and the
// test folder variable.

#include "crash_collect.hpp"
#include "crash_internal.hpp"

#include <bloom/crash/crash_reports.hpp>
#include <bloom/crash/minidump_summary.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

using Clock = std::chrono::system_clock;

void put32(std::vector<unsigned char>& bytes, const std::size_t offset, const std::uint32_t value) {
    if (bytes.size() < offset + 4)
        bytes.resize(offset + 4);
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<unsigned char>(value >> (8 * index));
}

struct SyntheticThread final {
    std::uint32_t id = 0;
    std::uint32_t stackBytes = 0;
};

// A minidump with the given thread list, an exception on `crashed`, Linux system information and
// process 4321. By default two threads, 100 and 101, and the exception on 101.
std::vector<unsigned char> syntheticMinidump(
    const std::vector<SyntheticThread>& list = {{100, 4096}, {101, 8192}},
    const std::uint32_t crashed = 101) {
    std::vector<unsigned char> bytes(32, 0);
    put32(bytes, 0, 0x504d444dU);
    put32(bytes, 4, 0xa793U);
    put32(bytes, 8, 4);   // streams
    put32(bytes, 12, 32); // directory
    const std::size_t directory = 32;
    const std::size_t threads = directory + 4 * 12;
    const std::size_t exception = threads + 4 + list.size() * 48;
    const std::size_t system = exception + 168;
    const std::size_t misc = system + 56;
    const std::size_t end = misc + 24;
    bytes.resize(end, 0);
    const auto entry = [&](const std::size_t index, const std::uint32_t type,
                           const std::uint32_t size, const std::size_t at) {
        put32(bytes, directory + index * 12, type);
        put32(bytes, directory + index * 12 + 4, size);
        put32(bytes, directory + index * 12 + 8, static_cast<std::uint32_t>(at));
    };
    entry(0, 3, static_cast<std::uint32_t>(4 + list.size() * 48), threads);
    entry(1, 6, 168, exception);
    entry(2, 7, 56, system);
    entry(3, 15, 24, misc);
    put32(bytes, threads, static_cast<std::uint32_t>(list.size()));
    for (std::size_t index = 0; index < list.size(); ++index) {
        put32(bytes, threads + 4 + index * 48, list[index].id);
        put32(bytes, threads + 4 + index * 48 + 32, list[index].stackBytes); // its stack bytes
    }
    put32(bytes, exception, crashed);
    put32(bytes, exception + 8, 11);   // SIGSEGV
    put32(bytes, system + 20, 0x8201); // Linux
    put32(bytes, misc, 24);
    put32(bytes, misc + 4, 1); // the process id is valid
    put32(bytes, misc + 8, 4321);
    return bytes;
}

void writeBytes(const std::filesystem::path& file, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void write(const std::filesystem::path& file, const std::string_view content) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary);
    stream << content;
}

// A crash folder as a collection makes it, at the given time.
std::filesystem::path makeReport(const std::filesystem::path& folder, const Clock::time_point time,
                                 const std::string& program, const std::string& id) {
    const auto report =
        folder / (bloom::crash::detail::folderTime(time) + "_" + program + "_" + id);
    write(report / "minidump.dmp", "MDMP");
    write(report / "report.txt", "Bloom quit unexpectedly\n\nProgram: " + program + "\n");
    return report;
}

void minidumps(const std::filesystem::path& scratch) {
    const auto file = scratch / "synthetic.dmp";
    const auto bytes = syntheticMinidump();
    writeBytes(file, bytes);
    const auto summary = bloom::crash::readMinidumpSummary(file);
    expect(summary.has_value(), "minidump: a well-formed minidump is read");
    if (!summary)
        return;
    expect(summary->threads.size() == 2 && summary->threads[0].id == 100 &&
               summary->threads[0].stackBytes == 4096 && summary->threads[1].stackBytes == 8192,
           "minidump: the threads and their stacks");
    expect(summary->crashedThreadId == 101U && summary->exceptionCode == 11U,
           "minidump: the crashed thread and the exception");
    expect(summary->platform == "linux" && summary->processId == 4321U,
           "minidump: the platform and the process");
    expect(bloom::crash::describeException(*summary) == "SIGSEGV (0x0000000b)",
           "minidump: the exception's name");
    expect(!summary->threads[0].exited && !summary->threads[1].exited &&
               bloom::crash::exitedThreads(*summary).empty(),
           "minidump: a thread with its stack is not recorded as exited");

    // #756: a thread exiting at the crash has no stack in the minidump. It is recorded as exited,
    // and the live threads keep their stacks; the crashed thread is never recorded as exited.
    writeBytes(file, syntheticMinidump({{100, 4096}, {102, 0}, {101, 8192}, {103, 0}}));
    const auto exiting = bloom::crash::readMinidumpSummary(file);
    expect(exiting && exiting->threads.size() == 4 && !exiting->threads[0].exited &&
               exiting->threads[1].exited && !exiting->threads[2].exited &&
               exiting->threads[3].exited,
           "minidump: the threads with no stack are recorded as exited, the others are not");
    expect(exiting && bloom::crash::exitedThreads(*exiting) == std::vector<std::uint32_t>{102, 103},
           "minidump: the exited threads in the minidump's order");
    writeBytes(file, syntheticMinidump({{100, 4096}, {101, 0}}));
    const auto crashedWithout = bloom::crash::readMinidumpSummary(file);
    expect(crashedWithout && crashedWithout->threads.size() == 2 &&
               !crashedWithout->threads[1].exited &&
               bloom::crash::exitedThreads(*crashedWithout).empty(),
           "minidump: the crashed thread is not recorded as exited, even without a stack");
    writeBytes(file, bytes);

    auto truncated = bytes;
    truncated.resize(bytes.size() - 30);
    writeBytes(file, truncated);
    expect(!bloom::crash::readMinidumpSummary(file),
           "minidump: a stream that lies outside the file is refused");
    write(file, "not a minidump at all, just some text that is long enough");
    expect(!bloom::crash::readMinidumpSummary(file), "minidump: another file is refused");
}

void textReports() {
    std::string session = "Program: bloom\nVersion: 1.2.3\nProcess: 4321\nGPU: Test GPU\n"
                          "Last log lines:\n";
    for (int line = 0; line < 150; ++line)
        session += "10:00:00 line " + std::to_string(line) + "\n";
    bloom::crash::detail::CrashFacts facts;
    facts.time = Clock::now();
    const std::string text = bloom::crash::detail::composeTextReport(session, facts);
    expect(text.starts_with("Bloom quit unexpectedly\n\nProgram: bloom\nVersion: 1.2.3\n"),
           "text report: the heading and the program's own lines first");
    expect(text.find("Crash: the minidump could not be read\n") != std::string::npos,
           "text report: an unreadable minidump is named");
    expect(text.find("line 49\n") == std::string::npos &&
               text.find("line 50\n") != std::string::npos &&
               text.find("line 149\n") != std::string::npos,
           "text report: the last 100 log lines are kept");
    const auto thread = [](const std::uint32_t id, const std::uint32_t stackBytes,
                           const bool exited = false) {
        bloom::crash::MinidumpThread record;
        record.id = id;
        record.stackBytes = stackBytes;
        record.exited = exited;
        return record;
    };
    facts.summary.emplace();
    facts.summary->threads = {thread(100, 4096), thread(101, 8192)};
    facts.summary->crashedThreadId = 101;
    facts.summary->exceptionCode = 11;
    facts.summary->platform = "linux";
    const std::string live = bloom::crash::detail::composeTextReport(session, facts);
    expect(live.find("\nCrash: SIGSEGV (0x0000000b) on thread 101, 2 threads\n") !=
                   std::string::npos &&
               live.find("Exited at the crash") == std::string::npos,
           "text report: the crash, its thread and the thread count; no thread exited");
    facts.summary->threads = {thread(100, 4096), thread(102, 0, true), thread(101, 8192)};
    expect(bloom::crash::detail::composeTextReport(session, facts)
                   .find("on thread 101, 3 threads\nExited at the crash (no stack): thread 102\n") !=
               std::string::npos,
           "text report: a thread exiting at the crash is recorded as exited");
    facts.summary->threads.push_back(thread(103, 0, true));
    expect(bloom::crash::detail::composeTextReport(session, facts)
                   .find("Exited at the crash (no stack): threads 102, 103\n") != std::string::npos,
           "text report: every exited thread is named");
    facts.summary.reset();
    facts.helperProcess = true;
    expect(bloom::crash::detail::composeTextReport("", facts).starts_with(
               "A process Bloom started quit unexpectedly\n\nProgram: unknown"),
           "text report: a helper's crash and a missing prepared report are named");
    expect(bloom::crash::detail::sessionValue(session, "Process") == "4321" &&
               bloom::crash::detail::sessionValue(session, "line 3").empty(),
           "text report: header values are read from the header only");
}

void offering(const std::filesystem::path& scratch) {
    const auto folder = scratch / "offer";
    const auto now = Clock::now();
    makeReport(folder, now - std::chrono::hours(2), "bloom", "aaaaaaaa");
    makeReport(folder, now - std::chrono::hours(1), "bloom-cli", "bbbbbbbb");
    const auto helper = makeReport(folder, now, "bloom", "cccccccc");
    write(helper / "helper", "a helper");
    write(folder / "not-a-report" / "report.txt", "x");

    const auto reports = bloom::crash::listReports(folder);
    expect(reports.size() == 3, "offer: every crash folder is listed, nothing else");
    expect(reports.size() == 3 && reports[0].helperProcess && reports[1].program == "bloom-cli" &&
               reports[2].program == "bloom",
           "offer: newest first, with their programs");
    auto offered = bloom::crash::reportsToOffer(folder);
    expect(offered.size() == 2 && offered[0].program == "bloom-cli",
           "offer: the crashes not yet offered, without a helper process's");
    for (const auto& report : offered)
        expect(bloom::crash::markOffered(report), "offer: the offer is recorded");
    expect(bloom::crash::reportsToOffer(folder).empty(), "offer: the next start offers none");
    expect(bloom::crash::listReports(folder).size() == 3, "offer: offered reports are kept");
    expect(bloom::crash::readTextReport(offered.front()).starts_with("Bloom quit unexpectedly"),
           "offer: the text report is read");
}

void pruning(const std::filesystem::path& scratch) {
    const auto now = Clock::now();
    {
        const auto folder = scratch / "prune-count";
        for (int index = 0; index < 25; ++index)
            makeReport(folder, now - std::chrono::minutes(index), "bloom",
                       "id" + std::to_string(100000 + index));
        const auto removed = bloom::crash::pruneReports(folder, now);
        const auto kept = bloom::crash::listReports(folder);
        expect(removed == 5 && kept.size() == bloom::crash::kMaximumKeptReports,
               "prune: at most 20 crashes are kept");
        expect(!kept.empty() && kept.front().folder.filename().string().ends_with("id100000") &&
                   kept.back().folder.filename().string().ends_with("id100019"),
               "prune: the newest 20 are kept");
    }
    {
        const auto folder = scratch / "prune-age";
        makeReport(folder, now - std::chrono::hours(24 * 31), "bloom", "old00001");
        makeReport(folder, now - std::chrono::hours(24 * 29), "bloom", "new00001");
        const auto removed = bloom::crash::pruneReports(folder, now);
        const auto kept = bloom::crash::listReports(folder);
        expect(removed == 1 && kept.size() == 1 &&
                   kept.front().folder.filename().string().ends_with("new00001"),
               "prune: a crash older than 30 days is removed");
    }
}

void issueAddresses() {
    std::string report = "Bloom quit unexpectedly\n\nProgram: bloom\nVersion: 0.1.0-alpha.2\n"
                         "\nLast log lines:\n";
    const std::string shortAddress = bloom::crash::newIssueAddress(report);
    expect(
        shortAddress.starts_with("https://github.com/afterlab/bloom/issues/new?title="
                                 "Bloom%20quit%20unexpectedly%20%28bloom%200.1.0-alpha.2%29&body="),
        "issue: the address and the title");
    expect(shortAddress.find("Program%3A%20bloom%0A") != std::string::npos,
           "issue: the body carries the text report");
    for (int line = 0; line < 400; ++line)
        report += "10:00:00 a long log line number " + std::to_string(line) + " with some words\n";
    const std::string longAddress = bloom::crash::newIssueAddress(report);
    expect(longAddress.size() <= bloom::crash::kMaximumIssueAddressLength,
           "issue: a long report is shortened to fit");
    expect(longAddress.find("number%20399%20") != std::string::npos &&
               longAddress.find("number%200%20") == std::string::npos,
           "issue: the oldest log lines go first");
    expect(longAddress.find("Version%3A%200.1.0-alpha.2") != std::string::npos,
           "issue: the header stays");
}

void heldFiles(const std::filesystem::path& scratch) {
    const auto file = scratch / "held.txt";
    bloom::crash::detail::HeldFile held;
    expect(held.open(file), "held: the file is created and locked");
    expect(held.replace("first\n") && held.append("second\n") && held.size() == 13,
           "held: the content is replaced and appended to");
    expect(bloom::crash::detail::heldByAnotherProcess(file), "held: a held file reads as held");
    held.close(false);
    expect(std::filesystem::exists(file) && !bloom::crash::detail::heldByAnotherProcess(file),
           "held: a closed file is free");
    expect(held.open(file), "held: it can be held again");
    held.close(true);
    expect(!std::filesystem::exists(file), "held: closing can remove it");
}

void folderVariable() {
    const auto folder = bloom::crash::crashDirectory();
    const char* value = std::getenv("BLOOM_TEST_CRASH_DIRECTORY"); // NOLINT(concurrency-mt-unsafe)
    expect(value != nullptr && folder && *folder == std::filesystem::path(value),
           "folder: a test's crash folder is the test variable's");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bloom_crash_reports_test <scratch folder>\n";
        return 2;
    }
    const std::filesystem::path scratch = argv[1];
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    minidumps(scratch);
    textReports();
    offering(scratch);
    pruning(scratch);
    issueAddresses();
    heldFiles(scratch);
    folderVariable();
    std::filesystem::remove_all(scratch);
    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}

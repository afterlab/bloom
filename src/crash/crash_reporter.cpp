#include <bloom/crash/crash_reporter.hpp>

#include <bloom/crash/crash_reports.hpp>

#include "crash_internal.hpp"

#include <bloom/core/diagnostic_record.hpp>
#include <bloom/core/executable_suffix.hpp>
#include <bloom/core/version.hpp>
#include <bloom/core/worker_thread.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <streambuf>
#include <system_error>
#include <vector>

#if BLOOM_CRASH_HAVE_CRASHPAD
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/settings.h"
#endif

namespace bloom::crash {
namespace {

constexpr std::size_t kMaximumLogLineBytes = 2000;
// The prepared report is rewritten from memory (header and the last kReportLogLines lines) once
// appending has made it larger than this.
constexpr std::uint64_t kSessionRewriteBytes = 256U * 1024U;

[[nodiscard]] std::string clockTime() {
    const std::time_t seconds = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    std::array<char, 16> buffer{};
    const std::size_t length = std::strftime(buffer.data(), buffer.size(), "%H:%M:%S", &parts);
    return std::string(buffer.data(), length);
}

// The program's prepared text report: the header, the GPU line and the last log lines, kept in
// memory and in the held file the crash handler copies beside the minidump.
class Session final : public core::DiagnosticRecordSink {
  public:
    bool open(const std::filesystem::path& file, std::string header) {
        const std::lock_guard lock(mutex_);
        header_ = std::move(header);
        if (!file_.open(file))
            return false;
        return file_.replace(contentLocked());
    }

    void logLine(const std::string_view text) noexcept override {
        try {
            std::string line = clockTime() + " ";
            line.append(text.substr(0, kMaximumLogLineBytes));
            for (char& character : line) {
                if (character == '\n' || character == '\r')
                    character = ' ';
            }
            const std::lock_guard lock(mutex_);
            recent_.push_back(line);
            while (recent_.size() > kReportLogLines)
                recent_.pop_front();
            if (!file_.isOpen())
                return;
            if (file_.size() > kSessionRewriteBytes)
                file_.replace(contentLocked());
            else
                file_.append(line + "\n");
        } catch (...) {
            // A log line that cannot be kept is dropped; the program runs on.
        }
    }

    void gpuDevice(const std::string_view description) noexcept override {
        try {
            const std::lock_guard lock(mutex_);
            gpu_.assign(description);
            if (file_.isOpen())
                file_.replace(contentLocked());
        } catch (...) {
        }
    }

    void finish() noexcept {
        const std::lock_guard lock(mutex_);
        file_.close(true);
    }

    [[nodiscard]] std::filesystem::path path() {
        const std::lock_guard lock(mutex_);
        return file_.path();
    }

  private:
    [[nodiscard]] std::string contentLocked() const {
        std::string content = header_;
        content += "GPU: " + gpu_ + "\n";
        content += "Last log lines:\n";
        for (const auto& line : recent_)
            content += line + "\n";
        return content;
    }

    std::mutex mutex_;
    detail::HeldFile file_;
    std::string header_;
    std::string gpu_ = "none created";
    std::deque<std::string> recent_;
};

// std::cerr's own buffer, and a copy of every complete line into the text report.
class StandardErrorTee final : public std::streambuf {
  public:
    explicit StandardErrorTee(std::streambuf* original) : original_(original) {}

  protected:
    int_type overflow(const int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof()))
            return traits_type::not_eof(character);
        const char value = traits_type::to_char_type(character);
        const std::lock_guard lock(mutex_);
        keep(&value, 1);
        return original_->sputc(value);
    }

    std::streamsize xsputn(const char* text, const std::streamsize count) override {
        const std::lock_guard lock(mutex_);
        keep(text, static_cast<std::size_t>(count));
        return original_->sputn(text, count);
    }

    int sync() override { return original_->pubsync(); }

  private:
    void keep(const char* text, std::size_t count) {
        while (count > 0) {
            const char* const end = static_cast<const char*>(std::memchr(text, '\n', count));
            const std::size_t length =
                end == nullptr ? count : static_cast<std::size_t>(end - text);
            if (line_.size() < kMaximumLogLineBytes)
                line_.append(text, std::min(length, kMaximumLogLineBytes - line_.size()));
            if (end == nullptr)
                return;
            core::recordLogLine(line_);
            line_.clear();
            text += length + 1;
            count -= length + 1;
        }
    }

    std::streambuf* original_;
    std::mutex mutex_;
    std::string line_;
};

struct ReporterGlobals final {
    std::mutex mutex;
    bool started = false;
    ReporterStatus status;
    // Created once and kept to the end of the process: the sink may be called from any thread
    // until the process exits, and Crashpad's client must outlive every crash.
    Session* session = nullptr;
    core::WorkerThread collector;
    // The background collection has finished (or never started).
    bool collected = true;
    std::condition_variable collectedChanged;
#if BLOOM_CRASH_HAVE_CRASHPAD
    crashpad::CrashpadClient* client = nullptr;
#endif
};

ReporterGlobals& globals() {
    static auto* const value = new ReporterGlobals();
    return *value;
}

[[nodiscard]] std::string randomTag() {
    std::random_device device;
    constexpr std::string_view digits = "0123456789abcdef";
    std::string tag;
    for (int index = 0; index < 8; ++index)
        tag += digits[device() & 0xFU];
    return tag;
}

[[nodiscard]] bool validProgramName(const std::string_view program) {
    return !program.empty() && program.size() <= 32 &&
           std::ranges::all_of(program, [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '-';
           });
}

ReporterStatus unavailable(ReporterStatus status, std::string detail) {
    status.state = ReporterState::Unavailable;
    status.detail = std::move(detail);
    return status;
}

#if BLOOM_CRASH_HAVE_CRASHPAD
[[nodiscard]] base::FilePath crashpadPath(const std::filesystem::path& path) {
    return base::FilePath(path.native());
}
#endif

// Starts the handler. Returns an empty string, or why it did not start.
[[nodiscard]] std::string startHandler(ReporterGlobals& state, const ReporterOptions& options,
                                       const std::filesystem::path& directory,
                                       const std::filesystem::path& sessionFile) {
#if BLOOM_CRASH_HAVE_CRASHPAD
    const auto handler =
        options.executable.parent_path() / "bloom-crash-handler" BLOOM_EXECUTABLE_SUFFIX;
    std::error_code error;
    if (!std::filesystem::is_regular_file(handler, error))
        return "the crash handler is not beside the program (" + handler.string() + ")";
    const auto database = directory / "database";
    {
        // Uploads are off in the database's settings as well; the handler has no transport.
        const auto opened = crashpad::CrashReportDatabase::Initialize(crashpadPath(database));
        if (!opened)
            return "the crash database cannot be created in " + database.string();
        if (auto* settings = opened->GetSettings())
            settings->SetUploadsEnabled(false);
    }
    const std::map<std::string, std::string> annotations{
        {"prod", "Bloom"},
        {"ver", std::string(core::version())},
        {"program", options.program},
    };
    // No upload address: the handler keeps reports in the database. --no-rate-limit and
    // --no-upload-gzip only concern uploads, which this build cannot make.
    const std::vector<std::string> arguments{"--no-rate-limit"};
    std::vector<base::FilePath> attachments;
    if (!sessionFile.empty())
        attachments.push_back(crashpadPath(sessionFile));
    state.client = new crashpad::CrashpadClient();
    if (!state.client->StartHandler(crashpadPath(handler), crashpadPath(database), base::FilePath(),
                                    std::string(), std::string(), annotations, arguments,
                                    /*restartable=*/false, /*asynchronous_start=*/false,
                                    attachments)) {
        return "the crash handler did not start";
    }
    return {};
#else
    static_cast<void>(state);
    static_cast<void>(options);
    static_cast<void>(directory);
    static_cast<void>(sessionFile);
    return "this build has no crash handler (Crashpad is not in its dependency prefix)";
#endif
}

} // namespace

std::string describeThisProcess(const std::string& program) {
    std::string header;
    header += "Program: " + program + "\n";
    header += "Version: " + std::string(core::version()) + "\n";
    header += "Build: " + detail::buildDescription() + "\n";
    header += "Platform: " + detail::platformName() + "\n";
    header += "OS version: " + detail::operatingSystemVersion() + "\n";
    header += "Process: " + std::to_string(detail::currentProcessId()) + "\n";
    header += "Started: " + detail::readableTime(std::chrono::system_clock::now()) + "\n";
    return header;
}

ReporterStatus startCrashReporter(const ReporterOptions& options) {
    auto& state = globals();
    const std::lock_guard lock(state.mutex);
    if (state.started)
        return state.status;
    state.started = true;

    ReporterStatus status;
    const auto directory =
        options.directory.empty() ? crashDirectory() : std::optional(options.directory);
    if (!directory) {
        state.status = unavailable(status, "no per-user data folder for crash reports");
        return state.status;
    }
    status.directory = *directory;
    if (!validProgramName(options.program)) {
        state.status = unavailable(status, "the program name is not a plain name");
        return state.status;
    }
    std::error_code error;
    std::filesystem::create_directories(*directory / "sessions", error);
    if (error) {
        state.status = unavailable(status, "cannot create the crash folder " + directory->string() +
                                               ": " + error.message());
        return state.status;
    }

    // The text report, prepared before anything can crash.
    auto* session = new Session();
    const auto sessionFile = *directory / "sessions" /
                             (options.program + "-" + std::to_string(detail::currentProcessId()) +
                              "-" + randomTag() + ".txt");
    if (!session->open(sessionFile, describeThisProcess(options.program))) {
        delete session;
        session = nullptr;
    }

    const std::string problem = startHandler(
        state, options, *directory, session != nullptr ? sessionFile : std::filesystem::path());
    if (!problem.empty()) {
        if (session != nullptr) {
            session->finish();
            delete session;
            session = nullptr;
        }
        status = unavailable(status, problem);
    } else {
        status.state = ReporterState::Running;
        if (session != nullptr) {
            state.session = session;
            core::setDiagnosticRecordSink(session);
        }
    }
    if (options.collect) {
        state.collected = false;
        state.collector = core::WorkerThread([folder = *directory, &state] {
            try {
                static_cast<void>(collectReports(folder, std::chrono::system_clock::now()));
            } catch (...) {
                // Collection is housekeeping: a failure leaves the reports where they are.
            }
            const std::lock_guard done(state.mutex);
            state.collected = true;
            state.collectedChanged.notify_all();
        });
    }
    state.status = status;
    return state.status;
}

ReporterStatus crashReporterStatus() {
    auto& state = globals();
    const std::lock_guard lock(state.mutex);
    return state.status;
}

void waitForCollection() {
    auto& state = globals();
    std::unique_lock lock(state.mutex);
    state.collectedChanged.wait(lock, [&state] { return state.collected; });
}

void finishCrashReporter() noexcept {
    auto& state = globals();
    try {
        waitForCollection();
        core::WorkerThread collector;
        {
            const std::lock_guard lock(state.mutex);
            collector = std::move(state.collector);
        }
        if (collector.joinable())
            collector.join();
    } catch (...) {
    }
    const std::lock_guard lock(state.mutex);
    if (state.session != nullptr) {
        core::setDiagnosticRecordSink(nullptr);
        // Kept allocated: a thread may still be inside a call it started before the sink was
        // removed. Only the file goes.
        state.session->finish();
    }
}

void captureStandardError() {
    static auto* const errorTee = new StandardErrorTee(std::cerr.rdbuf());
    static const bool installed = [] {
        // std::clog writes to the same standard error.
        std::clog.rdbuf(errorTee);
        std::cerr.rdbuf(errorTee);
        return true;
    }();
    static_cast<void>(installed);
}

} // namespace bloom::crash

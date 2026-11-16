#include <bloom/ui/update_service.hpp>

#include "qt_update_transport.hpp"

#include <bloom/core/version.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <atomic>
#include <string_view>
#include <utility>

namespace bloom::ui {

namespace {

[[nodiscard]] QString toQString(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

[[nodiscard]] std::filesystem::path toPath(const QString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
}

// The name a downloaded file is kept under: the last part of its address when that is a plain
// file name, and a fixed name otherwise. The name decides nothing; the file is verified by its
// bytes.
[[nodiscard]] std::string downloadName(const std::string_view url) {
    const auto slash = url.find_last_of('/');
    const auto name = slash == std::string_view::npos ? std::string_view{} : url.substr(slash + 1);
    const bool plain = !name.empty() && name.size() <= 120 && name.front() != '.' &&
                       name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                              "0123456789._-") == std::string_view::npos;
    return plain ? std::string(name) : std::string("update");
}

[[nodiscard]] update::StagingResult stageDownload(const std::filesystem::path& file,
                                                  const update::InstallFacts& install,
                                                  const std::function<bool()>& cancelled) {
    if (install.kind == update::InstallKind::AppImage) {
        return update::stageAppImage(file, install.root);
    }
    return update::stageFolder(file, install.root, cancelled);
}

[[nodiscard]] runtime::TaskRequest request(std::string name, const runtime::TaskPriority priority) {
    return {std::move(name),
            {.kind = runtime::TaskOwnerKind::Application,
             .id = runtime::TaskOwnerId::fromRaw(734)},
            priority, runtime::TaskExecutor::BlockingIo};
}

} // namespace

struct UpdateService::Progress final {
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> total{0};
};

UpdateEnvironment applicationUpdateEnvironment() {
    UpdateEnvironment environment;
    const auto variables = update::processEnvironment();
    const auto executable = update::currentExecutablePath();
    environment.configuration =
        update::effectiveConfiguration(update::builtInConfiguration(), variables);
    environment.current =
        update::ReleaseVersion::parse(core::version()).value_or(update::ReleaseVersion{});
    environment.install = update::detectInstall(executable, variables);
    environment.platformKey = std::string(update::currentPlatformKey());
    environment.folders.root =
        toPath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)) / "updates";
    environment.installedHelper = update::installedHelperProgram(executable);
    environment.processId = update::currentProcessId();
    environment.makeTransport = [] { return std::make_unique<QtUpdateTransport>(); };
    environment.stage = stageDownload;
    environment.startHelper = [](const std::filesystem::path& helper,
                                 const std::filesystem::path& plan) {
        return QProcess::startDetached(toQString(helper),
                                       {QStringLiteral("--plan"), toQString(plan)},
                                       QDir::rootPath());
    };
    return environment;
}

UpdateService::UpdateService(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                             UpdateEnvironment environment, QObject* parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge),
      environment_(std::make_shared<const UpdateEnvironment>(std::move(environment))) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &UpdateService::poll);
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &UpdateService::poll);
}

UpdateService::~UpdateService() {
    // A job that is still running reads only what it was given; it is told to stop and ends on
    // its own thread.
    cancel();
}

update::StartupResult UpdateService::settleAtStartup() {
    if (environment_->install.kind == update::InstallKind::Development) {
        return {};
    }
    auto result = update::settleAtStartup(environment_->folders, environment_->current.text());
    if (result.finishPlan && environment_->startHelper && !environment_->installedHelper.empty()) {
        // The swap happened and this is the new version: the helper only has to see the
        // confirmation that was just written, and ends.
        (void)environment_->startHelper(environment_->folders.helper(),
                                        environment_->folders.plan());
    }
    return result;
}

void UpdateService::setState(const State state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged();
}

bool UpdateService::checkNow() { return startCheck(false); }

bool UpdateService::checkAutomatically(const update::AutomaticCheckState& state,
                                       const std::int64_t nowSeconds) {
    // A build that is not configured, or was not installed, has nothing to ask.
    if (!environment_->configuration.configured() ||
        environment_->install.kind == update::InstallKind::Development ||
        !update::automaticCheckDue(state, nowSeconds)) {
        return false;
    }
    return startCheck(true);
}

bool UpdateService::startCheck(const bool automatic) {
    if (state_ != State::Idle) {
        return false;
    }
    auto submission = scheduler_.submit<std::shared_ptr<update::CheckResult>>(
        request("Check for updates", automatic ? runtime::TaskPriority::Background
                                               : runtime::TaskPriority::Foreground),
        [environment = environment_](runtime::TaskContext& context) {
            const auto cancelled = [&context] { return context.isCancellationRequested(); };
            // The transport is made here, on the job's thread, where it lives and ends.
            const auto transport =
                environment->makeTransport ? environment->makeTransport() : nullptr;
            auto result = std::make_shared<update::CheckResult>();
            if (transport != nullptr) {
                *result = update::checkForUpdate(environment->configuration, *transport,
                                                 environment->current, environment->install.kind,
                                                 environment->platformKey, cancelled);
            }
            if (result->status == update::CheckStatus::Cancelled) {
                return runtime::TaskResult<std::shared_ptr<update::CheckResult>>::cancelled();
            }
            return runtime::TaskResult<std::shared_ptr<update::CheckResult>>::succeeded(
                std::move(result));
        });
    if (!submission.accepted()) {
        return false;
    }
    automatic_ = automatic;
    check_.emplace(std::move(submission.handle));
    setState(State::Checking);
    bridge_.wake();
    return true;
}

bool UpdateService::startDownload() {
    if (state_ != State::Idle || !lastCheck_.has_value() ||
        lastCheck_->status != update::CheckStatus::Decided || !lastCheck_->feed.has_value() ||
        !lastCheck_->decision.file.has_value() ||
        (lastCheck_->decision.action != update::UpdateAction::ReplaceFolder &&
         lastCheck_->decision.action != update::UpdateAction::ReplaceAppImage)) {
        return false;
    }
    const auto entry = *lastCheck_->decision.file;
    const auto version = lastCheck_->feed->version.text();
    progress_ = std::make_shared<Progress>();
    progress_->total.store(entry.size, std::memory_order_relaxed);
    reportedReceived_ = 0;
    auto submission = scheduler_.submit<std::shared_ptr<UpdateDownloadOutcome>>(
        request("Download Bloom " + version, runtime::TaskPriority::Foreground),
        [environment = environment_, entry, version,
         progress = progress_](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<UpdateDownloadOutcome>>;
            const auto cancelled = [&context] { return context.isCancellationRequested(); };
            auto outcome = std::make_shared<UpdateDownloadOutcome>();
            outcome->version = version;
            const auto transport =
                environment->makeTransport ? environment->makeTransport() : nullptr;
            if (transport == nullptr) {
                return Result::succeeded(std::move(outcome));
            }
            runtime::TaskProgress shown;
            shown.phase = "Downloading";
            shown.total = entry.size;
            context.reportProgress(shown);
            outcome->download = update::downloadUpdateFile(
                *transport, entry, environment->configuration.keys,
                environment->folders.downloads() / downloadName(entry.url), cancelled,
                [&](const std::uint64_t received, std::uint64_t /*announced*/) {
                    progress->received.store(received, std::memory_order_relaxed);
                    shown.completed = received;
                    context.reportProgress(shown);
                });
            if (outcome->download.succeeded() && !cancelled()) {
                shown.phase = "Preparing";
                shown.completed = 0;
                shown.total.reset();
                context.reportProgress(shown);
                outcome->staging = environment->stage
                                       ? environment->stage(outcome->download.file,
                                                            environment->install, cancelled)
                                       : update::StagingResult{};
                // The download has done its part once the version is prepared (or could not be).
                std::error_code error;
                std::filesystem::remove(outcome->download.file, error);
            }
            outcome->cancelled =
                cancelled() && !(outcome->download.succeeded() && outcome->staging.succeeded());
            if (outcome->cancelled && outcome->staging.succeeded()) {
                std::error_code error;
                std::filesystem::remove_all(outcome->staging.staged, error);
            }
            return Result::succeeded(std::move(outcome));
        });
    if (!submission.accepted()) {
        progress_.reset();
        return false;
    }
    download_.emplace(std::move(submission.handle));
    setState(State::Downloading);
    bridge_.wake();
    return true;
}

void UpdateService::cancel() {
    if (check_.has_value()) {
        check_->cancel();
    }
    if (download_.has_value()) {
        download_->cancel();
    }
}

void UpdateService::poll() {
    if (check_.has_value()) {
        if (auto result = check_->tryTakeResult(); result.has_value()) {
            check_.reset();
            update::CheckResult finished;
            finished.status = update::CheckStatus::Cancelled;
            if (result->state() == runtime::TaskState::Succeeded && result->value().has_value() &&
                *result->value() != nullptr) {
                finished = std::move(**result->value());
            } else if (result->state() != runtime::TaskState::Cancelled) {
                finished.status = update::CheckStatus::Unreachable;
            }
            lastCheck_ = std::move(finished);
            const bool automatic = automatic_;
            setState(State::Idle);
            emit checkFinished(automatic);
        }
    }
    if (download_.has_value()) {
        if (progress_ != nullptr) {
            const auto received = progress_->received.load(std::memory_order_relaxed);
            if (received != reportedReceived_) {
                reportedReceived_ = received;
                emit downloadProgress(received, progress_->total.load(std::memory_order_relaxed));
            }
        }
        if (auto result = download_->tryTakeResult(); result.has_value()) {
            download_.reset();
            progress_.reset();
            UpdateDownloadOutcome finished;
            finished.cancelled = true;
            if (result->state() == runtime::TaskState::Succeeded && result->value().has_value() &&
                *result->value() != nullptr) {
                finished = std::move(**result->value());
            }
            lastDownload_ = std::move(finished);
            setState(lastDownload_->ready() ? State::ReadyToRestart : State::Idle);
            emit downloadFinished();
        }
    }
}

bool UpdateService::restartIntoUpdate() {
    if (state_ != State::ReadyToRestart || !lastDownload_.has_value() || !lastDownload_->ready() ||
        environment_->installedHelper.empty() || !environment_->startHelper) {
        return false;
    }
    const auto plan = update::makeSwapPlan(environment_->install, lastDownload_->staging.staged,
                                           lastDownload_->version, environment_->folders,
                                           environment_->processId);
    const auto helper =
        update::writePlanAndHelper(plan, environment_->installedHelper, environment_->folders);
    if (!helper.has_value() || !environment_->startHelper(*helper, environment_->folders.plan())) {
        return false;
    }
    emit restartRequested();
    return true;
}

} // namespace bloom::ui

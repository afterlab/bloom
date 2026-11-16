#pragma once

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/update/check.hpp>
#include <bloom/update/staging.hpp>
#include <bloom/update/zsync.hpp>

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::ui {

class TaskUiBridge;

// What the update service runs against (ADR 0041). The application fills it from the build and
// from where the program runs (`applicationUpdateEnvironment`); a test gives its own.
struct UpdateEnvironment final {
    update::UpdateConfiguration configuration;
    update::ReleaseVersion current;
    update::InstallFacts install;
    std::string platformKey;
    update::UpdateFolders folders;
    // The helper inside the package, and this process as the helper waits for it.
    std::filesystem::path installedHelper;
    std::uint64_t processId = 0;
    // Makes the transport of one job, on that job's thread.
    std::function<std::unique_ptr<update::UpdateTransport>()> makeTransport;
    // Prepares a verified download beside the installation, on the job's thread.
    std::function<update::StagingResult(const std::filesystem::path& file,
                                        const update::InstallFacts& install,
                                        const std::function<bool()>& cancelled)>
        stage;
    // Starts the helper's copy with its plan, as a process of its own.
    std::function<bool(const std::filesystem::path& helper, const std::filesystem::path& plan)>
        startHelper;
};

// The environment of the running application: the build's update settings, the installation
// around the program, the user's data folder, the network transport and a detached helper.
[[nodiscard]] UpdateEnvironment applicationUpdateEnvironment();

// How a download ended.
struct UpdateDownloadOutcome final {
    // The version that was downloaded.
    std::string version;
    update::DownloadResult download;
    // An AppImage's delta: how much came from the installed file, or why the whole file was
    // downloaded.
    update::DeltaStatistics delta;
    // The prepared version, when the download was accepted.
    update::StagingResult staging;
    bool cancelled = false;

    [[nodiscard]] bool ready() const noexcept {
        return !cancelled && download.succeeded() && staging.succeeded();
    }
};

// Checks for updates and downloads one, each as a background job of the task scheduler
// (blocking I/O, with progress and cancel, shown in the Jobs panel), never on the interface
// thread. This object lives on the interface thread and only starts jobs and takes their
// results. It shows nothing itself: the Help menu, the update window and Preferences read it.
class UpdateService final : public QObject {
    Q_OBJECT

  public:
    enum class State : std::uint8_t {
        Idle,
        Checking,
        Downloading,
        // A version is prepared beside the installation; `restartIntoUpdate` installs it.
        ReadyToRestart,
    };

    // `bridge` is borrowed and must outlive this object; `scheduler` is the one it wraps.
    UpdateService(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                  UpdateEnvironment environment, QObject* parent = nullptr);
    ~UpdateService() override;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const UpdateEnvironment& environment() const noexcept { return *environment_; }
    // The last check that finished; nothing before the first.
    [[nodiscard]] const std::optional<update::CheckResult>& lastCheck() const noexcept {
        return lastCheck_;
    }
    // The last download that finished or was cancelled.
    [[nodiscard]] const std::optional<UpdateDownloadOutcome>& lastDownload() const noexcept {
        return lastDownload_;
    }

    // At start-up, before anything else: confirms this version's start to a waiting helper,
    // takes the notice a restored version finds, and settles a plan an earlier run left.
    [[nodiscard]] update::StartupResult settleAtStartup();

    // Starts a check. False while a job is running or a version waits for the restart.
    bool checkNow();
    // The automatic check: starts one when the rule allows it now (on, and none in the last
    // day). True when it started; the caller then records `nowSeconds` as the last check.
    bool checkAutomatically(const update::AutomaticCheckState& state, std::int64_t nowSeconds);
    // Downloads and prepares the file the last check decided on. False when there is none, or
    // a job is running.
    bool startDownload();
    // Cancels the running job. Its end is reported like any other.
    void cancel();
    // Hands the prepared version to the helper and asks the application to quit
    // (`restartRequested`). False, with nothing changed, when the helper could not be started.
    bool restartIntoUpdate();

  public slots:
    // Takes a finished job's result. Connected to the bridge; a test may call it.
    void poll();

  signals:
    void stateChanged();
    // `automatic`: nobody asked for this check, so only an update is worth showing.
    void checkFinished(bool automatic);
    void downloadProgress(std::uint64_t received, std::uint64_t total);
    void downloadFinished();
    // The helper is waiting for this process to exit.
    void restartRequested();

  private:
    struct Progress;
    using CheckHandle = runtime::TaskHandle<std::shared_ptr<update::CheckResult>>;
    using DownloadHandle = runtime::TaskHandle<std::shared_ptr<UpdateDownloadOutcome>>;

    bool startCheck(bool automatic);
    void setState(State state);

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    // Shared with the jobs, which outlive this object when the application closes first.
    std::shared_ptr<const UpdateEnvironment> environment_;
    State state_ = State::Idle;
    bool automatic_ = false;
    std::optional<CheckHandle> check_;
    std::optional<DownloadHandle> download_;
    std::shared_ptr<Progress> progress_;
    std::uint64_t reportedReceived_ = 0;
    std::optional<update::CheckResult> lastCheck_;
    std::optional<UpdateDownloadOutcome> lastDownload_;
};

} // namespace bloom::ui

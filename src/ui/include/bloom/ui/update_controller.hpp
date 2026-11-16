#pragma once

#include <bloom/ui/update_service.hpp>
#include <bloom/update/check_policy.hpp>
#include <bloom/update/staging.hpp>

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <string>

class QWidget;

namespace bloom::ui {

class PlatformUpdateDriver;
class UpdateWindow;

// What the update window shows (ADR 0041, #734). One screen for each thing the artist can be told.
enum class UpdateScreen : std::uint8_t {
    Checking,
    UpToDate,
    // The feed has no build for this system.
    NotPublished,
    // A newer version that Bloom downloads and installs itself.
    Offer,
    Downloading,
    Ready,
    Refused,
    Unreachable,
    NotConfigured,
    // A build folder, which is never updated.
    BuildFolder,
    PackageManager,
    RunInstallerAgain,
    // A newer version that is fetched by hand: the Windows zip, or a platform updater that this
    // build does not carry.
    DownloadManually,
};

// How the last download ended, when it did not end in a prepared version.
enum class UpdateDownloadNote : std::uint8_t { None, Cancelled, Unreachable, Refused, NotPrepared };

// What the controller reads and writes outside itself, so a test gives its own.
struct UpdateControllerHooks final {
    // The automatic check's inputs: the Preferences switch and the last check's time.
    std::function<update::AutomaticCheckState()> readCheckState;
    // Records that a check started now.
    std::function<void(const update::AutomaticCheckState&)> writeCheckState;
    // Seconds since 1970.
    std::function<std::int64_t()> nowSeconds;
    // Asks the application to close the normal way, as the artist's own Quit does: unsaved
    // changes and running exports are asked about, and the artist may cancel. The application
    // reports the request (`closeRequested`) and, when it really closes, `applicationClosing`.
    std::function<void()> requestClose;
};

// The hooks of the running application: the switch is the stored preference, the last check's
// time is session state (`updates/last-check-seconds`), the clock is the system's.
[[nodiscard]] UpdateControllerHooks applicationUpdateHooks(std::function<void()> requestClose);

// Owns what the update interface does (Help > Check for Updates, the automatic check, the
// window, the restart), so the menu and the window stay thin. Lives on the interface thread and
// borrows the service and the driver.
class UpdateController final : public QObject {
    Q_OBJECT

  public:
    // `owner` is the window the update window belongs to. `driver` may be null.
    UpdateController(UpdateService& service, PlatformUpdateDriver* driver,
                     UpdateControllerHooks hooks, QWidget& owner, QObject* parent = nullptr);
    ~UpdateController() override;

    // What the window shows now, and what it says about it.
    [[nodiscard]] UpdateScreen screen() const;
    [[nodiscard]] const UpdateEnvironment& environment() const { return service_.environment(); }
    [[nodiscard]] std::string currentVersion() const;
    [[nodiscard]] std::string newVersion() const;
    [[nodiscard]] const std::string& releaseNotes() const;
    [[nodiscard]] std::string packageManager() const;
    [[nodiscard]] update::UpdateRefusal refusal() const;
    [[nodiscard]] UpdateDownloadNote downloadNote() const noexcept { return downloadNote_; }
    [[nodiscard]] update::UpdateRefusal downloadRefusal() const noexcept {
        return downloadRefusal_;
    }
    [[nodiscard]] std::uint64_t downloadReceived() const noexcept { return received_; }
    [[nodiscard]] std::uint64_t downloadTotal() const noexcept { return total_; }
    // The update window, once it was made.
    [[nodiscard]] QWidget* window() const;

    // Help > Check for Updates: starts a check and shows the window. While a check or a
    // download is running (or a version waits for the restart) it shows that instead.
    void checkNow();
    // Once the main window is up: asks now, and again every hour while Bloom runs. The rule
    // decides whether a check happens, so it is at most once a day.
    void startAutomaticChecks();
    // One automatic check, if the rule allows it now.
    void runAutomaticCheck();
    // The window's buttons.
    void startDownload();
    void cancel();
    // The artist chose to install: the application is asked to close the normal way, and only
    // when that close is confirmed is the helper started. The choice belongs to that one close
    // request: when it is declined in any way, a later close installs nothing.
    void restartAndUpdate();
    // Opens the releases page in the browser.
    void openDownloadPage() const;
    // At start-up: tells the artist when the version they had was put back.
    void noticeAtStartup(const update::StartupResult& result);

    // The main window was asked to close, by anyone. A request that is not the one
    // `restartAndUpdate` made forgets a restart that an earlier, declined close was to do.
    void closeRequested();
    // The application is closing for sure (the artist confirmed or had nothing to save).
    void applicationClosing();

  signals:
    // The window's content has changed.
    void changed();

  private:
    void onCheckFinished(bool automatic);
    void onDownloadFinished();
    void showWindow(bool activate);
    void recordCheck(std::int64_t nowSeconds);
    [[nodiscard]] UpdateWindow* ensureWindow();

    UpdateService& service_;
    PlatformUpdateDriver* driver_ = nullptr;
    UpdateControllerHooks hooks_;
    QWidget& owner_;
    QPointer<UpdateWindow> window_;
    QTimer hourly_;
    UpdateDownloadNote downloadNote_ = UpdateDownloadNote::None;
    update::UpdateRefusal downloadRefusal_ = update::UpdateRefusal::None;
    std::uint64_t received_ = 0;
    std::uint64_t total_ = 0;
    bool restartArmed_ = false;
    // True while `restartAndUpdate` is inside its own close request.
    bool requestingClose_ = false;
    bool closing_ = false;
};

} // namespace bloom::ui

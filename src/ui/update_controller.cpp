#include <bloom/ui/update_controller.hpp>

#include "update_window.hpp"

#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/platform_update_driver.hpp>
#include <bloom/update/install_kind.hpp>
#include <bloom/update/refusal.hpp>

#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QSettings>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <chrono>
#include <utility>

namespace bloom::ui {

namespace {

constexpr auto kLastCheckKey = "updates/last-check-seconds";
// Asked every hour; the rule itself keeps a check to once a day.
constexpr std::chrono::hours kAutomaticCheckPoll{1};

// A newer version that the artist can act on or should know about.
[[nodiscard]] bool offersNewerVersion(const update::CheckResult& result, const bool hasDriver) {
    if (result.status != update::CheckStatus::Decided) {
        return false;
    }
    switch (result.decision.action) {
    case update::UpdateAction::ReplaceFolder:
    case update::UpdateAction::ReplaceAppImage:
    case update::UpdateAction::LeaveToPackageManager:
    case update::UpdateAction::RunInstallerAgain:
    case update::UpdateAction::DownloadManually:
        return true;
    case update::UpdateAction::PlatformUpdater:
        return !hasDriver;
    case update::UpdateAction::UpToDate:
    case update::UpdateAction::NotPublished:
    case update::UpdateAction::NotInstalled:
        return false;
    }
    return false;
}

} // namespace

UpdateControllerHooks applicationUpdateHooks(std::function<void()> requestClose) {
    UpdateControllerHooks hooks;
    hooks.readCheckState = [] {
        const QSettings settings;
        update::AutomaticCheckState state;
        state.enabled = loadApplicationPreferences(settings).automaticUpdateCheck;
        const auto stored = settings.value(QLatin1String(kLastCheckKey));
        bool ok = false;
        const auto seconds = stored.toLongLong(&ok);
        if (ok) {
            state.lastCheckSeconds = seconds;
        }
        return state;
    };
    hooks.writeCheckState = [](const update::AutomaticCheckState& state) {
        if (state.lastCheckSeconds.has_value()) {
            QSettings settings;
            settings.setValue(QLatin1String(kLastCheckKey),
                              static_cast<qlonglong>(*state.lastCheckSeconds));
        }
    };
    hooks.nowSeconds = [] { return static_cast<std::int64_t>(QDateTime::currentSecsSinceEpoch()); };
    hooks.requestClose = std::move(requestClose);
    return hooks;
}

UpdateController::UpdateController(UpdateService& service, PlatformUpdateDriver* driver,
                                   UpdateControllerHooks hooks, QWidget& owner, QObject* parent)
    : QObject(parent), service_(service), driver_(driver), hooks_(std::move(hooks)), owner_(owner) {
    hourly_.setInterval(static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kAutomaticCheckPoll).count()));
    connect(&hourly_, &QTimer::timeout, this, &UpdateController::runAutomaticCheck);
    connect(&service_, &UpdateService::stateChanged, this, &UpdateController::changed);
    connect(&service_, &UpdateService::checkFinished, this, &UpdateController::onCheckFinished);
    connect(&service_, &UpdateService::downloadFinished, this,
            &UpdateController::onDownloadFinished);
    connect(&service_, &UpdateService::downloadProgress, this,
            [this](const std::uint64_t received, const std::uint64_t total) {
                received_ = received;
                total_ = total;
                emit changed();
            });
}

UpdateController::~UpdateController() = default;

UpdateScreen UpdateController::screen() const {
    switch (service_.state()) {
    case UpdateService::State::Checking:
        return UpdateScreen::Checking;
    case UpdateService::State::Downloading:
        return UpdateScreen::Downloading;
    case UpdateService::State::ReadyToRestart:
        return UpdateScreen::Ready;
    case UpdateService::State::Idle:
        break;
    }
    const auto& check = service_.lastCheck();
    if (!check.has_value()) {
        return UpdateScreen::Checking;
    }
    switch (check->status) {
    case update::CheckStatus::NotConfigured:
        return UpdateScreen::NotConfigured;
    case update::CheckStatus::Cancelled:
    case update::CheckStatus::Unreachable:
        return UpdateScreen::Unreachable;
    case update::CheckStatus::Refused:
        return UpdateScreen::Refused;
    case update::CheckStatus::Decided:
        break;
    }
    switch (check->decision.action) {
    case update::UpdateAction::UpToDate:
        return UpdateScreen::UpToDate;
    case update::UpdateAction::NotPublished:
        return UpdateScreen::NotPublished;
    case update::UpdateAction::NotInstalled:
        return UpdateScreen::BuildFolder;
    case update::UpdateAction::ReplaceFolder:
    case update::UpdateAction::ReplaceAppImage:
        return UpdateScreen::Offer;
    case update::UpdateAction::LeaveToPackageManager:
        return UpdateScreen::PackageManager;
    case update::UpdateAction::RunInstallerAgain:
        return UpdateScreen::RunInstallerAgain;
    case update::UpdateAction::PlatformUpdater:
    case update::UpdateAction::DownloadManually:
        return UpdateScreen::DownloadManually;
    }
    return UpdateScreen::UpToDate;
}

std::string UpdateController::currentVersion() const {
    return service_.environment().current.text();
}

std::string UpdateController::newVersion() const {
    const auto& check = service_.lastCheck();
    return check.has_value() && check->feed.has_value() ? check->feed->version.text()
                                                        : std::string{};
}

const std::string& UpdateController::releaseNotes() const {
    static const std::string none;
    const auto& check = service_.lastCheck();
    return check.has_value() && check->feed.has_value() ? check->feed->notes : none;
}

std::string UpdateController::packageManager() const {
    return service_.environment().install.manager;
}

update::UpdateRefusal UpdateController::refusal() const {
    const auto& check = service_.lastCheck();
    return check.has_value() ? check->diagnostic.refusal : update::UpdateRefusal::None;
}

QWidget* UpdateController::window() const { return window_.data(); }

UpdateWindow* UpdateController::ensureWindow() {
    if (window_.isNull()) {
        window_ = new UpdateWindow(*this, owner_);
    }
    return window_.data();
}

void UpdateController::showWindow(const bool activate) {
    auto* shown = ensureWindow();
    emit changed();
    if (activate) {
        shown->show();
        shown->raise();
        shown->activateWindow();
        return;
    }
    // An automatic result never takes the keyboard from what the artist is typing in.
    shown->setAttribute(Qt::WA_ShowWithoutActivating, true);
    shown->show();
    shown->setAttribute(Qt::WA_ShowWithoutActivating, false);
}

void UpdateController::recordCheck(const std::int64_t nowSeconds) {
    if (!hooks_.readCheckState || !hooks_.writeCheckState) {
        return;
    }
    auto state = hooks_.readCheckState();
    state.lastCheckSeconds = nowSeconds;
    hooks_.writeCheckState(state);
}

void UpdateController::checkNow() {
    if (service_.state() == UpdateService::State::Idle && service_.checkNow() &&
        hooks_.nowSeconds) {
        recordCheck(hooks_.nowSeconds());
    }
    showWindow(true);
}

void UpdateController::startAutomaticChecks() {
    hourly_.start();
    runAutomaticCheck();
}

void UpdateController::runAutomaticCheck() {
    if (!hooks_.readCheckState || !hooks_.nowSeconds) {
        return;
    }
    const auto now = hooks_.nowSeconds();
    if (service_.checkAutomatically(hooks_.readCheckState(), now)) {
        recordCheck(now);
    }
}

void UpdateController::onCheckFinished(const bool automatic) {
    downloadNote_ = UpdateDownloadNote::None;
    downloadRefusal_ = update::UpdateRefusal::None;
    const auto& check = service_.lastCheck();
    if (!check.has_value()) {
        return;
    }
    if (check->status == update::CheckStatus::Cancelled) {
        if (!window_.isNull()) {
            window_->hide();
        }
        return;
    }
    if (unattended_) {
        // The end-to-end update test reads this line when its update does not happen: nobody
        // sees the window of a test build that runs without one.
        qWarning().noquote() << QStringLiteral(
                                    "Unattended update: check %1, refusal %2 (%3), action %4, "
                                    "install %5, platform updater %6")
                                    .arg(static_cast<int>(check->status))
                                    .arg(QString::fromUtf8(
                                        update::refusalName(check->diagnostic.refusal)))
                                    .arg(QString::fromStdString(check->diagnostic.detail))
                                    .arg(static_cast<int>(check->decision.action))
                                    .arg(QString::fromUtf8(update::installKindName(
                                        service_.environment().install.kind)))
                                    .arg(driver_ != nullptr ? QStringLiteral("present")
                                                            : QStringLiteral("absent"));
    }
    if (check->status == update::CheckStatus::Decided &&
        check->decision.action == update::UpdateAction::PlatformUpdater && driver_ != nullptr) {
        // The platform's updater has its own window; Bloom shows none for the update itself.
        if (!window_.isNull()) {
            window_->hide();
        }
        driver_->showUpdate(check->decision.appcastUrl,
                            check->feed.has_value() ? check->feed->version.text()
                                                    : std::string{});
        return;
    }
    if (unattended_ && check->status == update::CheckStatus::Decided &&
        check->decision.file.has_value() &&
        (check->decision.action == update::UpdateAction::ReplaceFolder ||
         check->decision.action == update::UpdateAction::ReplaceAppImage)) {
        startDownload();
        return;
    }
    if (!automatic) {
        showWindow(true);
        return;
    }
    if (check->status == update::CheckStatus::Refused) {
        qWarning().noquote() << "The automatic update check was refused:"
                             << QString::fromUtf8(update::refusalName(check->diagnostic.refusal));
    }
    if (offersNewerVersion(*check, driver_ != nullptr)) {
        showWindow(false);
    } else {
        emit changed();
    }
}

void UpdateController::onDownloadFinished() {
    downloadNote_ = UpdateDownloadNote::None;
    downloadRefusal_ = update::UpdateRefusal::None;
    const auto& outcome = service_.lastDownload();
    if (outcome.has_value() && !outcome->ready()) {
        if (outcome->cancelled || outcome->download.transport.status ==
                                      update::TransportStatus::Cancelled ||
            outcome->download.diagnostic.refusal == update::UpdateRefusal::Cancelled) {
            downloadNote_ = UpdateDownloadNote::Cancelled;
        } else if (!outcome->download.diagnostic.accepted()) {
            downloadNote_ = UpdateDownloadNote::Refused;
            downloadRefusal_ = outcome->download.diagnostic.refusal;
        } else if (outcome->download.transport.status != update::TransportStatus::Ok) {
            downloadNote_ = UpdateDownloadNote::Unreachable;
        } else {
            downloadNote_ = UpdateDownloadNote::NotPrepared;
        }
    }
    emit changed();
    if (unattended_ && service_.state() == UpdateService::State::ReadyToRestart) {
        restartAndUpdate();
    }
}

void UpdateController::startDownload() {
    const auto& check = service_.lastCheck();
    received_ = 0;
    total_ = check.has_value() && check->decision.file.has_value() ? check->decision.file->size : 0;
    downloadNote_ = UpdateDownloadNote::None;
    downloadRefusal_ = update::UpdateRefusal::None;
    (void)service_.startDownload();
    emit changed();
}

void UpdateController::cancel() { service_.cancel(); }

void UpdateController::restartAndUpdate() {
    if (service_.state() != UpdateService::State::ReadyToRestart || closing_ ||
        !hooks_.requestClose) {
        return;
    }
    restartArmed_ = true;
    requestingClose_ = true;
    hooks_.requestClose();
    requestingClose_ = false;
}

void UpdateController::closeRequested() {
    // The restart was chosen for one close request. Whatever became of that one (answered
    // Cancel, the project busy, exports kept running), another request is the artist quitting
    // for a reason of their own, and Bloom must not install and open again after it.
    if (!requestingClose_ && !closing_) {
        restartArmed_ = false;
    }
}

void UpdateController::applicationClosing() {
    closing_ = true;
    if (!restartArmed_) {
        return;
    }
    restartArmed_ = false;
    if (!service_.restartIntoUpdate()) {
        qWarning() << "The update helper could not be started; Bloom closes without updating.";
    }
}

void UpdateController::installWithoutAsking() {
    unattended_ = true;
    if (service_.state() == UpdateService::State::Idle) {
        (void)service_.checkNow();
    }
}

void UpdateController::openDownloadPage() const {
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleasesPageUrl)));
}

void UpdateController::noticeAtStartup(const update::StartupResult& result) {
    if (!result.restoredFrom.has_value()) {
        return;
    }
    kit::inform(owner_, kit::KPromptTone::Warning, tr("Update"),
                tr("Bloom %1 could not start, so the version you had was put back.")
                    .arg(QString::fromStdString(*result.restoredFrom)),
                {}, QStringLiteral("updateRestoredNotice"));
}

} // namespace bloom::ui

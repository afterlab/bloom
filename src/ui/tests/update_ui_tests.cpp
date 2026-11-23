// The update interface (ADR 0041, #734): the Help item, the update window, the Preferences switch
// and the controller that joins them to the update service. Capture mode (the environment
// variable named below) writes review pictures instead of asserting.
#include "update_test_support.hpp"

#include "preferences_window.hpp"
#include "update_window.hpp"

#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/platform_update_driver.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/update_controller.hpp>
#include <bloom/update/apply.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QDesktopServices>
#include <QTextBrowser>
#include <QUrl>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace bloom;
using namespace bloom::ui::update_tests;
using bloom::test_support::awaitQtCondition;

// QDesktopServices finds its handler by name through the meta-object system, so the class is
// outside the anonymous namespace, which moc cannot describe reliably.
// A recorded https address the browser would open.
class UrlCapture final : public QObject {
    Q_OBJECT
  public:
    QUrl last;
    int count = 0;
  public slots:
    void capture(const QUrl& url) {
        last = url;
        ++count;
    }
};

namespace {

// A window with the five stand-in editors, as the main window tests build one.
struct WindowBench final {
    ui::EditorRegistry registry;
    runtime::TaskScheduler scheduler;
    ui::ProjectHost projectHost{scheduler};
    ui::CompositionSession compositionSession;
    runtime::NodeDefinitionRegistry nodeDefinitions;
    runtime::SnapshotCompiler snapshotCompiler{nodeDefinitions};
    ui::TaskUiBridge taskUiBridge{scheduler};

    WindowBench()
        : compositionSession(*projectHost.liveDocumentAndStack().first,
                             *projectHost.liveDocumentAndStack().second,
                             projectHost.lowestCompositionId()) {
        nodeDefinitions.freeze();
        const auto add = [this](std::string id, QString name) {
            (void)registry.registerEditor({.id = std::move(id),
                                           .displayName = std::move(name),
                                           .create = [](QWidget* parent) {
                                               return new QLabel(QStringLiteral("editor"), parent);
                                           }});
        };
        add("bloom.viewer", "Compositor");
        add("bloom.nodes", "Nodes");
        add("bloom.timeline", "Timeline");
        add("bloom.assets", "Assets");
        add("bloom.properties", "Properties");
    }
};

[[nodiscard]] QString dprName(const QWidget& widget) {
    return widget.devicePixelRatioF() > 1.0 ? QStringLiteral("dpr1.5") : QStringLiteral("dpr1");
}

void saveCapture(const QString& phase, const QString& what, const QWidget& widget,
                 const QPixmap& picture) {
    const auto directory = qEnvironmentVariable("BLOOM_UPDATE_CAPTURE_DIR");
    (void)QDir().mkpath(directory);
    const auto name = QStringLiteral("734-%1-%2-%3-%4.png")
                          .arg(phase, what)
                          .arg(widget.width())
                          .arg(dprName(widget));
    (void)picture.save(QDir(directory).filePath(name));
}

void settle() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

// The pictures that exist before and after the change: the General page of Preferences at three
// widths, and the Help menu.
void captureExisting(const QString& phase, QWidget& owner) {
    ui::PreferencesWindow preferences(owner, ui::ApplicationPreferences{});
    preferences.show();
    if (auto* rail = preferences.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail")))
        rail->setCurrentRow(0);
    for (const int width : {2000, 900, 640}) {
        preferences.resize(width, 900);
        settle();
        saveCapture(phase, QStringLiteral("preferences-general"), preferences,
                    preferences.grab());
    }
    preferences.hide();

    WindowBench bench;
    ui::MainWindow window(bench.registry, bench.compositionSession, bench.projectHost);
    window.show();
    settle();
    for (auto* action : window.menuBar()->actions()) {
        if (action->text().remove(QLatin1Char('&')) == QStringLiteral("Help") &&
            action->menu() != nullptr) {
            auto* menu = action->menu();
            menu->popup(window.mapToGlobal(QPoint(40, 40)));
            settle();
            saveCapture(phase, QStringLiteral("help-menu"), *menu, menu->grab());
            menu->hide();
        }
    }
}


// What the controller reads and writes, in memory, with a clock the test moves.
struct Rig final {
    QWidget owner;
    Loopback server;
    Bench bench;
    update::AutomaticCheckState state;
    std::int64_t now = 1'800'000'000;
    int closeRequests = 0;
    bool acceptClose = false;
    int checks = 0;
    int downloads = 0;
    QByteArray fileBody = fixture("update.bin");
    std::shared_ptr<std::vector<std::filesystem::path>> helperStarts =
        std::make_shared<std::vector<std::filesystem::path>>();
    std::unique_ptr<ui::UpdateService> service;
    std::unique_ptr<ui::UpdateController> controller;

    Rig(const std::string& name, const QByteArray& feed,
        const std::function<void(ui::UpdateEnvironment&)>& tweak = {},
        ui::PlatformUpdateDriver* driver = nullptr, const bool closedPort = false)
        : bench(name) {
        server.respond = [this, feed](const QByteArray& request) {
            return request.startsWith("GET /alpha.json ") ? answer(feed) : answer(fileBody);
        };
        std::string origin = server.origin();
        if (closedPort) {
            Loopback gone;
            origin = gone.origin();
        }
        auto environment = bench.environment(origin);
        environment.installedHelper = bench.root / "helper-in-package";
        std::ofstream(environment.installedHelper) << "helper\n";
        environment.startHelper = [starts = helperStarts](const std::filesystem::path&,
                                                          const std::filesystem::path& plan) {
            starts->push_back(plan);
            return true;
        };
        if (tweak) {
            tweak(environment);
        }
        service = std::make_unique<ui::UpdateService>(bench.scheduler, bench.bridge,
                                                      std::move(environment));
        ui::UpdateControllerHooks hooks;
        hooks.readCheckState = [this] { return state; };
        hooks.writeCheckState = [this](const update::AutomaticCheckState& next) { state = next; };
        hooks.nowSeconds = [this] { return now; };
        hooks.requestClose = [this] {
            // As the main window does: every close request is reported, and a close that is
            // not declined reports that the application is closing.
            ++closeRequests;
            controller->closeRequested();
            if (acceptClose) {
                controller->applicationClosing();
            }
        };
        controller = std::make_unique<ui::UpdateController>(*service, driver, std::move(hooks),
                                                            owner);
        QObject::connect(service.get(), &ui::UpdateService::checkFinished, service.get(),
                         [this](bool) { ++checks; });
        QObject::connect(service.get(), &ui::UpdateService::downloadFinished, service.get(),
                         [this] { ++downloads; });
    }

    // Stands in for unpacking: the fixture file is not a package.
    void standInForStaging(ui::UpdateEnvironment& environment) const;

    [[nodiscard]] bool checkDone(const int count = 1) {
        return awaitQtCondition("the check finishes", [&] { return checks == count; });
    }
    [[nodiscard]] QWidget* window() const { return controller->window(); }
    [[nodiscard]] QString text(const char* name) const {
        auto* label = window() != nullptr ? window()->findChild<QLabel*>(QLatin1String(name))
                                          : nullptr;
        return label != nullptr ? label->text() : QString();
    }
    [[nodiscard]] QAbstractButton* button(const char* name) const {
        return window() != nullptr ? window()->findChild<QAbstractButton*>(QLatin1String(name))
                                   : nullptr;
    }
    [[nodiscard]] bool buttonShown(const char* name, const QString& label) const {
        auto* found = button(name);
        return found != nullptr && found->isVisibleTo(window()) && found->text() == label;
    }
    [[nodiscard]] bool buttonHidden(const char* name) const {
        auto* found = button(name);
        return found != nullptr && !found->isVisibleTo(window());
    }
    [[nodiscard]] bool windowShown() const { return window() != nullptr && window()->isVisible(); }
};

void stage(ui::UpdateEnvironment& environment, const std::filesystem::path& staged) {
    environment.stage = [staged](const std::filesystem::path&, const update::InstallFacts&,
                                 const std::function<bool()>&) {
        std::error_code error;
        std::filesystem::create_directories(staged, error);
        return update::StagingResult{staged, {}};
    };
}

struct DriverStandIn final : ui::PlatformUpdateDriver {
    std::vector<std::string> shown;
    std::vector<std::string> versions;
    void showUpdate(const std::string& appcastUrl, const std::string& version) override {
        shown.push_back(appcastUrl);
        versions.push_back(version);
    }
};

[[nodiscard]] QByteArray newer() { return fixture("feed.json"); }

void testHelpMenu() {
    WindowBench bench;
    ui::MainWindow window(bench.registry, bench.compositionSession, bench.projectHost);
    auto* action = window.checkForUpdatesAction();
    expect(action != nullptr && action->text() == QString::fromUtf8("Check for Updates…") &&
               action->objectName() == "checkForUpdatesAction" && action->isEnabled() &&
               action->menuRole() == QAction::NoRole,
           "help menu: Check for Updates… exists, is never disabled, and keeps its place on macOS");
    auto* report = window.findChild<QAction*>(QStringLiteral("reportIssueAction"));
    QMenu* help = action != nullptr ? qobject_cast<QMenu*>(action->parent()) : nullptr;
    bool ordered = false;
    if (help != nullptr && report != nullptr) {
        const auto actions = help->actions();
        const auto at = actions.indexOf(action);
#ifdef Q_OS_MACOS
        // macOS has Install Command-Line Tool… right after it.
        ordered = at == 0 && actions.size() > 3 &&
                  actions[1] == window.installCommandLineToolAction() &&
                  actions[2]->isSeparator() && actions[3] == report;
#else
        ordered = at == 0 && actions.size() > 2 && actions[1]->isSeparator() &&
                  actions[2] == report;
#endif
    }
    expect(ordered, "help menu: it is first, with a separator before Report an Issue…");

    Rig rig("help", newer(), {});
    rig.controller->noticeAtStartup({});
    window.setUpdateController(rig.controller.get());
    expect(rig.window() == nullptr, "help menu: no window before it is asked for");
    action->trigger();
    expect(rig.service->state() == ui::UpdateService::State::Checking && rig.windowShown() &&
               rig.text("updateTitle") == QString::fromUtf8("Checking for updates…") &&
               rig.buttonShown("updateCancelButton", QStringLiteral("Cancel")),
           "help menu: it starts a check and shows the window checking, with Cancel");
    expect(rig.checkDone(), "help menu: the check finishes");
    expect(rig.text("updateTitle") == QStringLiteral("Bloom 0.2.0-alpha.3 is available"),
           "help menu: the window shows the result");
    const auto* first = rig.window();
    action->trigger();
    expect(rig.window() == first && rig.windowShown(),
           "help menu: asking again raises the one window");
    expect(rig.state.lastCheckSeconds == rig.now, "help menu: a check that was asked for is recorded");
}

void testRefusalSentences() {
    using R = update::UpdateRefusal;
    for (int value = 0; value <= static_cast<int>(R::Cancelled); ++value) {
        const auto refusal = static_cast<R>(value);
        const auto sentence = ui::UpdateWindow::refusalSentence(refusal);
        const auto name = QString::fromUtf8(update::refusalName(refusal));
        expect(refusal == R::None ? sentence.isEmpty()
                                  : (!sentence.isEmpty() && sentence.endsWith(QLatin1Char('.')) &&
                                     (!name.contains(QLatin1Char('-')) ||
                                      !sentence.contains(name, Qt::CaseInsensitive)) &&
                                     !sentence.contains(QLatin1Char('_'))),
               "refusals: every reason has a plain sentence without its typed name (" +
                   name.toStdString() + ")");
    }
    expect(ui::UpdateWindow::progressText(5ULL << 20, 80ULL << 20) ==
               QStringLiteral("5.0 MiB of 80.0 MiB"),
           "progress: received and total in binary units");
}

void testStates() {
    // A newer version that Bloom installs itself.
    {
        Rig rig("offer", newer());
        rig.controller->checkNow();
        expect(rig.checkDone(), "offer: the check finishes");
        expect(rig.text("updateTitle") == QStringLiteral("Bloom 0.2.0-alpha.3 is available") &&
                   rig.text("updateBody") == QStringLiteral("You have 0.1.0-alpha.1."),
               "offer: says what is available and what the artist has");
        auto* notes = rig.window()->findChild<QTextBrowser*>(QStringLiteral("updateReleaseNotes"));
        expect(notes != nullptr && notes->isVisibleTo(rig.window()) &&
                   notes->toPlainText().contains(QStringLiteral("fixture note")) &&
                   !notes->openLinks(),
               "offer: the release notes are shown, and their links do not open by themselves");
        expect(rig.buttonShown("updatePrimaryButton", QStringLiteral("Download and Install")) &&
                   rig.buttonShown("updateCloseButton", QStringLiteral("Later")) &&
                   rig.buttonHidden("updateCancelButton"),
               "offer: Download and Install and Later");
    }
    // Up to date.
    {
        Rig rig("current", newer(), [](ui::UpdateEnvironment& environment) {
            environment.current = update::ReleaseVersion::parse("0.2.0-alpha.3").value();
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "up to date: the check finishes");
        expect(rig.text("updateBody") == QStringLiteral("Bloom 0.2.0-alpha.3 is the newest version.") &&
                   rig.buttonShown("updateCloseButton", QStringLiteral("Close")) &&
                   rig.buttonHidden("updatePrimaryButton"),
               "up to date: says so, and offers Close");
    }
    // A feed this build does not trust.
    {
        Rig rig("refused", fixture("feed-other-key.json"));
        rig.controller->checkNow();
        expect(rig.checkDone(), "refused: the check finishes");
        const auto body = rig.text("updateBody");
        expect(body.startsWith(QStringLiteral(
                   "The update list could not be verified, so nothing was installed. ")) &&
                   body.contains(ui::UpdateWindow::refusalSentence(
                       update::UpdateRefusal::FeedUnknownKey)) &&
                   rig.buttonHidden("updatePrimaryButton"),
               "refused: says nothing was installed, with the reason in plain words");
    }
    // A build that was not set up for updates still answers.
    {
        Rig rig("unconfigured", newer(), [](ui::UpdateEnvironment& environment) {
            environment.configuration = {};
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "not configured: the check finishes");
        expect(rig.text("updateBody") ==
                       QStringLiteral("This build of Bloom was not set up to check for updates.") &&
                   rig.server.requests.isEmpty(),
               "not configured: says so and contacts nothing");
    }
    // Nothing listens at the address.
    {
        Rig rig("unreachable", newer(), {}, nullptr, true);
        rig.controller->checkNow();
        expect(rig.checkDone(), "unreachable: the check finishes");
        expect(rig.text("updateTitle") ==
                       QStringLiteral("Bloom could not reach the update server") &&
                   rig.text("updateBody") == QStringLiteral("Check your connection and try again.") &&
                   rig.buttonShown("updatePrimaryButton", QStringLiteral("Try Again")) &&
                   rig.buttonShown("updateCloseButton", QStringLiteral("Close")),
               "unreachable: says so, with Try Again and Close");
        rig.button("updatePrimaryButton")->click();
        expect(rig.service->state() == ui::UpdateService::State::Checking &&
                   rig.text("updateTitle") == QString::fromUtf8("Checking for updates…"),
               "unreachable: Try Again checks again");
        expect(rig.checkDone(2), "unreachable: and finishes");
    }
    // A package manager owns the install.
    for (const char* managerText : {"pacman", ""}) {
        const std::string manager = managerText;
        Rig rig("package", newer(), [&manager](ui::UpdateEnvironment& environment) {
            environment.install.kind = update::InstallKind::PackageManager;
            environment.install.manager = manager;
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "package manager: the check finishes");
        const auto owner = manager.empty() ? QStringLiteral("your package manager")
                                           : QStringLiteral("pacman");
        expect(rig.text("updateTitle") == QStringLiteral("Bloom 0.2.0-alpha.3 is available") &&
                   rig.text("updateBody") ==
                       QStringLiteral("This copy of Bloom was installed by %1. Update it there.")
                           .arg(owner) &&
                   rig.buttonHidden("updatePrimaryButton") &&
                   rig.buttonShown("updateCloseButton", QStringLiteral("Close")),
               "package manager: leaves the update to it, naming it");
    }
    // A system-wide install.
    {
        UrlCapture urls;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &urls, "capture");
        Rig rig("system", newer(), [](ui::UpdateEnvironment& environment) {
            environment.install.kind = update::InstallKind::ReadOnlyFolder;
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "system-wide: the check finishes");
        expect(rig.text("updateBody") ==
                       QStringLiteral("This copy is installed for all users of this computer, so "
                                      "it cannot update itself. Run the installer again to update "
                                      "it.") &&
                   rig.buttonShown("updatePrimaryButton", QStringLiteral("Open the Download Page")),
               "system-wide: says to run the installer again, with the download page");
        rig.button("updatePrimaryButton")->click();
        expect(urls.count == 1 && urls.last.toString() ==
                                      QStringLiteral("https://github.com/afterlab/bloom/releases"),
               "system-wide: the button opens the releases page in the browser");
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    }
    // A Windows zip, and a platform updater this build does not carry.
    {
        Rig rig("portable", newer(), [](ui::UpdateEnvironment& environment) {
            environment.install.kind = update::InstallKind::WindowsPortable;
            environment.platformKey = "windows-x86_64";
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "portable: the check finishes");
        expect(rig.text("updateTitle") == QStringLiteral("Bloom 0.2.0-alpha.3 is available") &&
                   rig.buttonShown("updatePrimaryButton", QStringLiteral("Open the Download Page")),
               "portable: names the version and offers the download page");
    }
    {
        Rig rig("no-driver", newer(), [](ui::UpdateEnvironment& environment) {
            environment.install.kind = update::InstallKind::MacBundle;
            environment.platformKey = "macos-arm64";
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "no platform updater: the check finishes");
        expect(rig.windowShown() &&
                   rig.buttonShown("updatePrimaryButton", QStringLiteral("Open the Download Page")),
               "no platform updater: the window says where to get the version");
    }
    // A build folder, and a system the feed has no build for.
    {
        Rig rig("development", newer(), [](ui::UpdateEnvironment& environment) {
            environment.install.kind = update::InstallKind::Development;
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "build folder: the check finishes");
        expect(rig.text("updateBody") ==
                   QStringLiteral("This copy of Bloom runs from a build folder and is not updated."),
               "build folder: says it is not updated");
    }
    {
        Rig rig("unpublished", newer(), [](ui::UpdateEnvironment& environment) {
            environment.platformKey = "linux-riscv64";
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "not published: the check finishes");
        expect(rig.text("updateTitle") ==
                   QStringLiteral("Bloom 0.1.0-alpha.1 is the newest version for this system"),
               "not published: the newest version for this system");
    }
    // Cancelling a check closes the window.
    {
        Rig rig("check-cancel", newer());
        rig.server.stallAfter = 0;
        rig.controller->checkNow();
        expect(awaitQtCondition("the request arrives",
                                [&] { return rig.server.requests.size() == 1; }),
               "cancel check: the request arrives");
        rig.button("updateCancelButton")->click();
        expect(rig.checkDone() && !rig.windowShown(),
               "cancel check: Cancel ends the check and closes the window");
    }
}

void testDownload() {
    // Progress, Cancel, and a window that closes without cancelling.
    {
        Rig rig("download-cancel", newer(), [](ui::UpdateEnvironment& environment) {
            stage(environment, environment.install.root.parent_path() / "staged");
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "download: the check finishes");
        rig.server.stallAfter = 380;
        rig.button("updatePrimaryButton")->click();
        expect(rig.service->state() == ui::UpdateService::State::Downloading &&
                   rig.text("updateTitle") == QStringLiteral("Downloading Bloom 0.2.0-alpha.3") &&
                   rig.buttonShown("updateCancelButton", QStringLiteral("Cancel")) &&
                   rig.buttonHidden("updatePrimaryButton"),
               "download: the window shows the download, with Cancel");
        expect(awaitQtCondition("bytes arrive",
                                [&] { return rig.controller->downloadReceived() > 0; }),
               "download: progress arrives");
        expect(rig.text("updateProgressText") ==
                   ui::UpdateWindow::progressText(rig.controller->downloadReceived(),
                                                  rig.controller->downloadTotal()) &&
                   rig.controller->downloadTotal() == 9001,
               "download: the progress text follows the bytes received");
        rig.window()->close();
        expect(!rig.windowShown() && rig.service->state() == ui::UpdateService::State::Downloading,
               "download: closing the window does not cancel it");
        rig.controller->checkNow();
        expect(rig.windowShown() &&
                   rig.text("updateTitle") == QStringLiteral("Downloading Bloom 0.2.0-alpha.3"),
               "download: opening the window again shows the download");
        rig.button("updateCancelButton")->click();
        expect(awaitQtCondition("the download ends", [&] { return rig.downloads == 1; }),
               "download: Cancel ends it");
        expect(rig.service->state() == ui::UpdateService::State::Idle &&
                   rig.text("updateTitle") == QStringLiteral("Bloom 0.2.0-alpha.3 is available") &&
                   rig.text("updateNote") == QStringLiteral("The download was cancelled.") &&
                   rig.buttonShown("updatePrimaryButton", QStringLiteral("Download and Install")),
               "download: back to the offer, with one sentence saying it was cancelled");
    }
    // A file that does not match the list is refused, and the offer says so.
    {
        Rig rig("download-refused", newer(), [](ui::UpdateEnvironment& environment) {
            stage(environment, environment.install.root.parent_path() / "staged");
        });
        rig.fileBody = fixture("update.bin").left(100);
        rig.controller->checkNow();
        expect(rig.checkDone(), "refused download: the check finishes");
        rig.button("updatePrimaryButton")->click();
        expect(awaitQtCondition("the download ends", [&] { return rig.downloads == 1; }),
               "refused download: it ends");
        expect(rig.text("updateNote").contains(QStringLiteral("refused")) &&
                   rig.text("updateNote").contains(ui::UpdateWindow::refusalSentence(
                       update::UpdateRefusal::FileSizeMismatch)),
               "refused download: the offer says what was refused");
    }
    // A finished download is ready; Restart and Update goes through the close path.
    {
        Rig rig("download-ready", newer(), [](ui::UpdateEnvironment& environment) {
            stage(environment, environment.install.root.parent_path() / "staged");
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "ready: the check finishes");
        rig.button("updatePrimaryButton")->click();
        expect(awaitQtCondition("the download ends", [&] { return rig.downloads == 1; }),
               "ready: the download ends");
        expect(rig.service->state() == ui::UpdateService::State::ReadyToRestart &&
                   rig.text("updateTitle") == QStringLiteral("Bloom 0.2.0-alpha.3 is ready") &&
                   rig.text("updateBody") ==
                       QStringLiteral("Bloom closes, installs it and opens again. If the new "
                                      "version cannot start, the version you have now is put "
                                      "back.") &&
                   rig.buttonShown("updatePrimaryButton", QStringLiteral("Restart and Update")) &&
                   rig.buttonShown("updateCloseButton", QStringLiteral("Later")),
               "ready: Restart and Update and Later");

        // The artist cancels the close: nothing starts, and the window still offers the restart.
        rig.button("updatePrimaryButton")->click();
        expect(rig.closeRequests == 1 && rig.helperStarts->empty(),
               "restart: it asks the application to close and starts nothing yet");
        expect(rig.buttonShown("updatePrimaryButton", QStringLiteral("Restart and Update")),
               "restart: the window still offers the restart");
        // Later the artist quits for a reason of their own: a close request that is not the
        // update's, confirmed. Nothing is installed.
        rig.controller->closeRequested();
        rig.controller->applicationClosing();
        expect(rig.helperStarts->empty(),
               "restart: a later quit does not install what a declined close asked for");
    }
    // The end-to-end test's run: check, download and restart follow each other with no window
    // and no question.
    {
        Rig rig("download-unattended", newer(), [](ui::UpdateEnvironment& environment) {
            stage(environment, environment.install.root.parent_path() / "staged");
        });
        rig.acceptClose = true;
        rig.controller->installWithoutAsking();
        expect(awaitQtCondition("the helper is started",
                                [&] { return rig.helperStarts->size() == 1; }),
               "unattended: the check leads to the download and the restart");
        expect(rig.checks == 1 && rig.downloads == 1 && rig.closeRequests == 1 &&
                   !rig.windowShown(),
               "unattended: one check, one download, one close request and no window");
    }
    // The close the restart asked for is confirmed later (the artist first answers a prompt):
    // no other close request came between, so the helper starts.
    {
        Rig rig("download-restart-after-prompt", newer(), [](ui::UpdateEnvironment& environment) {
            stage(environment, environment.install.root.parent_path() / "staged");
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "restart after a prompt: the check finishes");
        rig.button("updatePrimaryButton")->click();
        expect(awaitQtCondition("the download ends", [&] { return rig.downloads == 1; }),
               "restart after a prompt: the download ends");
        rig.button("updatePrimaryButton")->click();
        expect(rig.closeRequests == 1 && rig.helperStarts->empty(),
               "restart after a prompt: nothing starts while the prompt is open");
        rig.controller->applicationClosing();
        expect(rig.helperStarts->size() == 1,
               "restart after a prompt: the confirmed close starts the helper once");
    }
    // Confirmed: the application is really closing, and only then is the helper started.
    {
        Rig rig("download-restart", newer(), [](ui::UpdateEnvironment& environment) {
            stage(environment, environment.install.root.parent_path() / "staged");
        });
        rig.controller->checkNow();
        expect(rig.checkDone(), "restart: the check finishes");
        rig.button("updatePrimaryButton")->click();
        expect(awaitQtCondition("the download ends", [&] { return rig.downloads == 1; }),
               "restart: the download ends");
        rig.acceptClose = true;
        rig.button("updatePrimaryButton")->click();
        expect(rig.closeRequests == 1 && rig.helperStarts->size() == 1,
               "restart: once the close is accepted the helper is started once");
    }
}

void testAutomatic() {
    // Turned off: nothing starts.
    {
        Rig rig("auto-off", newer());
        rig.state.enabled = false;
        rig.controller->runAutomaticCheck();
        expect(rig.service->state() == ui::UpdateService::State::Idle && rig.server.requests.isEmpty() &&
                   !rig.state.lastCheckSeconds.has_value(),
               "automatic: turned off, nothing starts and nothing is recorded");
    }
    // On, a day since the last check: a newer version opens the window; an hour later, no request.
    {
        Rig rig("auto-on", newer());
        rig.state.enabled = true;
        rig.state.lastCheckSeconds = rig.now - update::kAutomaticCheckIntervalSeconds;
        rig.controller->runAutomaticCheck();
        expect(rig.service->state() == ui::UpdateService::State::Checking &&
                   rig.state.lastCheckSeconds == rig.now,
               "automatic: a check starts and its time is written");
        expect(rig.window() == nullptr, "automatic: nothing is shown while it checks");
        expect(rig.checkDone(), "automatic: the check finishes");
        expect(rig.windowShown() &&
                   rig.text("updateTitle") == QStringLiteral("Bloom 0.2.0-alpha.3 is available"),
               "automatic: a newer version opens the window");
        rig.now += 3600;
        rig.controller->runAutomaticCheck();
        expect(rig.service->state() == ui::UpdateService::State::Idle &&
                   rig.server.requests.size() == 1,
               "automatic: an hour later no second request is made");
    }
    // Up to date, refused and unreachable show nothing.
    {
        Rig upToDate("auto-current", newer(), [](ui::UpdateEnvironment& environment) {
            environment.current = update::ReleaseVersion::parse("0.2.0-alpha.3").value();
        });
        upToDate.controller->runAutomaticCheck();
        expect(upToDate.checkDone() && !upToDate.windowShown(),
               "automatic: up to date shows nothing");
        Rig refused("auto-refused", fixture("feed-other-key.json"));
        refused.controller->runAutomaticCheck();
        expect(refused.checkDone() && !refused.windowShown(), "automatic: a refusal shows nothing");
        Rig unreachable("auto-unreachable", newer(), {}, nullptr, true);
        unreachable.controller->runAutomaticCheck();
        expect(unreachable.checkDone() && !unreachable.windowShown(),
               "automatic: an unreachable server shows nothing");
    }
}

void testPlatformDriver() {
    for (const bool automatic : {false, true}) {
        DriverStandIn driver;
        Rig rig(automatic ? "driver-auto" : "driver-asked", newer(),
                [](ui::UpdateEnvironment& environment) {
                    environment.install.kind = update::InstallKind::MacBundle;
                    environment.platformKey = "macos-arm64";
                },
                &driver);
        if (automatic) {
            rig.controller->runAutomaticCheck();
        } else {
            rig.controller->checkNow();
        }
        expect(rig.checkDone(), "platform updater: the check finishes");
        expect(driver.shown.size() == 1 &&
                   driver.shown.front() == "https://github.com/afterlab/bloom/releases/download/"
                                           "v0.2.0-alpha.3/appcast-macos.xml",
               "platform updater: it is handed the verified feed's appcast address");
        expect(!rig.windowShown(), "platform updater: no Bloom window opens for the update");
    }
}

void testPreferencesSwitch() {
    WindowBench bench;
    ui::MainWindow main(bench.registry, bench.compositionSession, bench.projectHost);
    {
        const QSettings settings;
        expect(ui::loadApplicationPreferences(settings).automaticUpdateCheck,
               "preferences: the automatic check is on by default");
    }
    ui::PreferencesWindow window(main, ui::loadApplicationPreferences(QSettings()));
    auto* control = window.findChild<QAbstractButton*>(QStringLiteral("preferencesAutomaticUpdateSwitch"));
    auto* apply = window.findChild<QAbstractButton*>(QStringLiteral("preferencesApplyButton"));
    expect(control != nullptr && control->isChecked() && apply != nullptr && !apply->isEnabled(),
           "preferences: the switch is on and the page is clean");
    auto* hint = window.findChild<QLabel*>(QStringLiteral("preferencesAutomaticUpdateHint"));
    expect(hint != nullptr &&
               hint->text() == QStringLiteral("Once a day, Bloom asks whether a newer version "
                                              "exists. It sends nothing but that request."),
           "preferences: the line under the switch says what is sent");
    ui::ApplicationPreferences applied;
    QObject::connect(&window, &ui::PreferencesWindow::preferencesApplied, &window,
                     [&](const ui::ApplicationPreferences& next) { applied = next; });
    control->setChecked(false);
    expect(apply->isEnabled(), "preferences: switching it marks the page dirty");
    apply->click();
    expect(!applied.automaticUpdateCheck, "preferences: Apply carries the choice");
    main.commitPreferences(applied);
    expect(!ui::loadApplicationPreferences(QSettings()).automaticUpdateCheck &&
               !QSettings().value(QStringLiteral("updates/automatic-check")).toBool(),
           "preferences: it is stored under updates/automatic-check");
    ui::PreferencesWindow again(main, ui::loadApplicationPreferences(QSettings()));
    auto* reread = again.findChild<QAbstractButton*>(QStringLiteral("preferencesAutomaticUpdateSwitch"));
    expect(reread != nullptr && !reread->isChecked(), "preferences: a new window reads it back");
    expect(!ui::applicationUpdateHooks({}).readCheckState().enabled,
           "preferences: the controller reads the stored switch");
}

void testRestoredNotice() {
    Rig rig("notice", newer());
    const auto count = [] {
        int found = 0;
        for (auto* widget : QApplication::allWidgets()) {
            found += widget->objectName() == QStringLiteral("updateRestoredNoticeHeading") ? 1 : 0;
        }
        return found;
    };
    rig.controller->noticeAtStartup({});
    expect(count() == 0, "notice: nothing is said when nothing was put back");
    update::StartupResult restored;
    restored.restoredFrom = "0.2.0-alpha.3";
    rig.controller->noticeAtStartup(restored);
    QLabel* heading = nullptr;
    for (auto* widget : QApplication::allWidgets()) {
        if (widget->objectName() == QStringLiteral("updateRestoredNoticeHeading")) {
            heading = qobject_cast<QLabel*>(widget);
        }
    }
    expect(heading != nullptr &&
               heading->text() == QStringLiteral("Bloom 0.2.0-alpha.3 could not start, so the "
                                                 "version you had was put back."),
           "notice: says which version could not start and that the earlier one was put back");

    // #788: an update the Windows installer could not install is told, with its reason.
    update::StartupResult failed;
    failed.installFailed = update::InstallFailure{
        .version = "0.2.0-alpha.3", .reason = "Something kept Bloom's folder in use."};
    rig.controller->noticeAtStartup(failed);
    QLabel* failure = nullptr;
    for (auto* widget : QApplication::allWidgets()) {
        if (widget->objectName() == QStringLiteral("updateInstallFailedNoticeHeading")) {
            failure = qobject_cast<QLabel*>(widget);
        }
    }
    expect(failure != nullptr &&
               failure->text() ==
                   QStringLiteral("Bloom 0.2.0-alpha.3 could not be installed. The version you "
                                  "had is still installed and works as before. Something kept "
                                  "Bloom's folder in use."),
           "notice: says which update was not installed, that the old version works, and why");
}

// Every state of the window, for review (BLOOM_UPDATE_CAPTURE_DIR). Written after the change only.
void captureWindowStates(const QString& phase) {
    const auto shoot = [&](Rig& rig, const QString& what) {
        settle();
        if (rig.window() != nullptr) {
            rig.window()->adjustSize();
            settle();
            saveCapture(phase, what, *rig.window(), rig.window()->grab());
        }
    };
    const auto asked = [&](const QString& what, const QByteArray& feed,
                           const std::function<void(ui::UpdateEnvironment&)>& tweak,
                           const bool closed = false) {
        Rig rig("capture-" + what.toStdString(), feed, tweak, nullptr, closed);
        rig.controller->checkNow();
        (void)rig.checkDone();
        shoot(rig, what);
    };
    {
        Rig rig("capture-checking", newer());
        rig.server.stallAfter = 0;
        rig.controller->checkNow();
        shoot(rig, QStringLiteral("window-checking"));
        rig.service->cancel();
        (void)rig.checkDone();
    }
    asked(QStringLiteral("window-uptodate"), newer(), [](ui::UpdateEnvironment& environment) {
        environment.current = update::ReleaseVersion::parse("0.2.0-alpha.3").value();
    });
    asked(QStringLiteral("window-refused"), fixture("feed-other-key.json"), {});
    asked(QStringLiteral("window-unreachable"), newer(), {}, true);
    asked(QStringLiteral("window-notconfigured"), newer(), [](ui::UpdateEnvironment& environment) {
        environment.configuration = {};
    });
    asked(QStringLiteral("window-buildfolder"), newer(), [](ui::UpdateEnvironment& environment) {
        environment.install.kind = update::InstallKind::Development;
    });
    asked(QStringLiteral("window-package"), newer(), [](ui::UpdateEnvironment& environment) {
        environment.install.kind = update::InstallKind::PackageManager;
        environment.install.manager = "pacman";
    });
    asked(QStringLiteral("window-system"), newer(), [](ui::UpdateEnvironment& environment) {
        environment.install.kind = update::InstallKind::ReadOnlyFolder;
    });
    asked(QStringLiteral("window-portable"), newer(), [](ui::UpdateEnvironment& environment) {
        environment.install.kind = update::InstallKind::WindowsPortable;
        environment.platformKey = "windows-x86_64";
    });
    asked(QStringLiteral("window-notpublished"), newer(), [](ui::UpdateEnvironment& environment) {
        environment.platformKey = "linux-riscv64";
    });
    // The offer at its natural width, at 900 and at its smallest.
    {
        Rig rig("capture-offer", newer(), [](ui::UpdateEnvironment& environment) {
            stage(environment, environment.install.root.parent_path() / "staged");
        });
        rig.controller->checkNow();
        (void)rig.checkDone();
        shoot(rig, QStringLiteral("window-offer"));
        for (const int width : {900, 1}) {
            rig.window()->resize(width, rig.window()->height());
            settle();
            saveCapture(phase, QStringLiteral("window-offer"), *rig.window(), rig.window()->grab());
        }
        rig.window()->adjustSize();
        // Downloading, then ready.
        rig.server.stallAfter = 380;
        rig.button("updatePrimaryButton")->click();
        (void)awaitQtCondition("bytes arrive", [&] { return rig.controller->downloadReceived() > 0; });
        shoot(rig, QStringLiteral("window-downloading"));
        rig.server.stallAfter = -1;
        rig.button("updateCancelButton")->click();
        (void)awaitQtCondition("the download ends", [&] { return rig.downloads == 1; });
        shoot(rig, QStringLiteral("window-offer-cancelled"));
        rig.button("updatePrimaryButton")->click();
        (void)awaitQtCondition("the download ends", [&] { return rig.downloads == 2; });
        shoot(rig, QStringLiteral("window-ready"));
    }
}

} // namespace

#include "update_ui_tests.moc"

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QTemporaryDir home;
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BloomUpdateUiTests"));
    QCoreApplication::setApplicationName(QStringLiteral("update-ui-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
    QApplication application(argc, argv);
    QWidget owner;
    if (!qEnvironmentVariable("BLOOM_UPDATE_CAPTURE_DIR").isEmpty()) {
        const auto phase = qEnvironmentVariable("BLOOM_UPDATE_CAPTURE_PHASE", QStringLiteral("after"));
        captureExisting(phase, owner);
        if (phase == QStringLiteral("after")) {
            captureWindowStates(phase);
        }
        return 0;
    }
    testHelpMenu();
    testRefusalSentences();
    testStates();
    testDownload();
    testAutomatic();
    testPlatformDriver();
    testPreferencesSwitch();
    testRestoredNotice();
    return failures + bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
}

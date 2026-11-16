// The Windows driver of the platform updater (ADR 0041): WinSparkle, for a copy of Bloom that the
// installer installed. Compiled only on Windows.
//
// WinSparkle.dll is not linked. It is loaded by its full path from beside the program when the
// first update is shown, so only an installed Bloom ever loads it; a test program, a build
// folder and the portable zip's start never need it. The functions used here are declared as
// WinSparkle's header (winsparkle.h, 0.9.4) declares them.
//
// WinSparkle downloads the next installer in its own window, checks its Ed25519 signature
// against the key Bloom gives it, asks Bloom whether it can close, starts the installer with the
// arguments the update list gives (a silent install that starts Bloom again), and then asks
// Bloom to close. Its two callbacks come from WinSparkle's own thread; what they need from the
// application is asked for on the interface thread.

#include <bloom/ui/platform_update_driver.hpp>

#include <bloom/core/version.hpp>
#include <bloom/ui/update_service.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QMetaObject>
#include <QString>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <windows.h>

namespace bloom::ui {

namespace {

// How long WinSparkle's thread waits for the interface thread to say whether Bloom can close.
constexpr std::chrono::seconds kAnswerWait{10};

using CanShutdownCallback = int(__cdecl*)();
using ShutdownRequestCallback = void(__cdecl*)();

// The functions of WinSparkle.dll that the driver calls.
struct WinSparkleFunctions final {
    void(__cdecl* init)() = nullptr;
    void(__cdecl* cleanup)() = nullptr;
    void(__cdecl* setLanguage)(const char*) = nullptr;
    void(__cdecl* setAppcastUrl)(const char*) = nullptr;
    int(__cdecl* setEddsaPublicKey)(const char*) = nullptr;
    void(__cdecl* setAppDetails)(const wchar_t*, const wchar_t*, const wchar_t*) = nullptr;
    void(__cdecl* setRegistryPath)(const char*) = nullptr;
    void(__cdecl* setAutomaticCheck)(int) = nullptr;
    void(__cdecl* setCanShutdownCallback)(CanShutdownCallback) = nullptr;
    void(__cdecl* setShutdownRequestCallback)(ShutdownRequestCallback) = nullptr;
    void(__cdecl* checkWithUi)() = nullptr;
    void(__cdecl* checkWithUiAndInstall)() = nullptr;
};

template <typename Function>
[[nodiscard]] bool resolve(const HMODULE library, const char* name, Function& function) {
    // GetProcAddress gives one pointer type for every function; this is the cast its use needs.
    function = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(library, name)));
    return function != nullptr;
}

// Loads WinSparkle.dll from the program's own folder and nowhere else.
[[nodiscard]] bool loadWinSparkle(WinSparkleFunctions& functions) {
    const auto path =
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath() +
                                 QStringLiteral("/WinSparkle.dll"))
            .toStdWString();
    const HMODULE library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (library == nullptr) {
        return false;
    }
    // The library stays loaded until the program ends: WinSparkle runs a thread of its own.
    return resolve(library, "win_sparkle_init", functions.init) &&
           resolve(library, "win_sparkle_cleanup", functions.cleanup) &&
           resolve(library, "win_sparkle_set_lang", functions.setLanguage) &&
           resolve(library, "win_sparkle_set_appcast_url", functions.setAppcastUrl) &&
           resolve(library, "win_sparkle_set_eddsa_public_key", functions.setEddsaPublicKey) &&
           resolve(library, "win_sparkle_set_app_details", functions.setAppDetails) &&
           resolve(library, "win_sparkle_set_registry_path", functions.setRegistryPath) &&
           resolve(library, "win_sparkle_set_automatic_check_for_updates",
                   functions.setAutomaticCheck) &&
           resolve(library, "win_sparkle_set_can_shutdown_callback",
                   functions.setCanShutdownCallback) &&
           resolve(library, "win_sparkle_set_shutdown_request_callback",
                   functions.setShutdownRequestCallback) &&
           resolve(library, "win_sparkle_check_update_with_ui", functions.checkWithUi) &&
           resolve(library, "win_sparkle_check_update_with_ui_and_install",
                   functions.checkWithUiAndInstall);
}

// What WinSparkle's callbacks read. They carry no context of their own, so the one driver of the
// process leaves this where they find it, and each holds it for as long as it runs: a callback
// that is still running while the driver ends reads nothing that is gone.
struct Shared final {
    UpdateEnvironment environment;
    PlatformUpdateHooks hooks;
    // The version the updater was last shown for. Written and read on the interface thread.
    std::string version;
};

[[nodiscard]] std::mutex& sharedMutex() {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] std::shared_ptr<Shared>& sharedSlot() {
    static std::shared_ptr<Shared> slot;
    return slot;
}

[[nodiscard]] std::shared_ptr<Shared> shared() {
    const std::scoped_lock lock(sharedMutex());
    return sharedSlot();
}

// WinSparkle asks, before it starts the installer, whether Bloom can close. The installer needs
// Bloom gone, and a close that stops to ask about unsaved changes would leave it waiting, so the
// answer is yes only when Bloom can close without a question. On "no" WinSparkle tells the user
// to save and try again.
int __cdecl canShutdown() {
    try {
        const auto state = shared();
        auto* application = QCoreApplication::instance();
        if (state == nullptr || application == nullptr || !state->hooks.readyToClose) {
            return 0;
        }
        const auto answer = std::make_shared<std::promise<bool>>();
        auto future = answer->get_future();
        const bool posted = QMetaObject::invokeMethod(
            application, [state, answer] { answer->set_value(state->hooks.readyToClose()); },
            Qt::QueuedConnection);
        if (!posted || future.wait_for(kAnswerWait) != std::future_status::ready) {
            return 0;
        }
        return future.get() ? 1 : 0;
    } catch (...) {
        // An application that is ending answers nothing; nothing may leave a C callback.
        return 0;
    }
}

// WinSparkle has started the installer and asks Bloom to close. The helper is started first, so
// that it watches the new version's start and puts the kept version back when it does not come.
void __cdecl shutdownRequested() {
    try {
        const auto state = shared();
        auto* application = QCoreApplication::instance();
        if (state == nullptr || application == nullptr) {
            return;
        }
        (void)QMetaObject::invokeMethod(
            application,
            [state] {
                if (!startUpdateWatch(state->environment, state->version)) {
                    qWarning() << "The update helper could not be started; the update goes on "
                                  "without it.";
                }
                if (state->hooks.requestClose) {
                    state->hooks.requestClose();
                }
            },
            Qt::QueuedConnection);
    } catch (...) {
        // Nothing may leave a C callback.
    }
}

class WinSparkleDriver final : public PlatformUpdateDriver {
  public:
    WinSparkleDriver(const UpdateEnvironment& environment, PlatformUpdateHooks hooks,
                     const WinSparkleFunctions& functions)
        : state_(std::make_shared<Shared>()), functions_(functions) {
        state_->environment = environment;
        state_->hooks = std::move(hooks);
        const std::scoped_lock lock(sharedMutex());
        sharedSlot() = state_;
    }

    ~WinSparkleDriver() override {
        {
            const std::scoped_lock lock(sharedMutex());
            sharedSlot().reset();
        }
        if (started_) {
            functions_.cleanup();
        }
    }

    void showUpdate(const std::string& appcastUrl, const std::string& version) override {
        state_->version = version;
        if (!started_ && !start(appcastUrl)) {
            return;
        }
        // A test build that is told to goes on to the install without the update window's
        // question, so the end-to-end test needs nobody at the keyboard.
        if (state_->environment.configuration.testBuild &&
            qEnvironmentVariableIsSet("BLOOM_UPDATE_TEST_INSTALL_NOW")) {
            functions_.checkWithUiAndInstall();
        } else {
            functions_.checkWithUi();
        }
    }

  private:
    // WinSparkle takes its settings once, before it starts. The address of the update list is
    // the one the first verified feed of this run gave.
    [[nodiscard]] bool start(const std::string& appcastUrl) {
        const auto& keys = state_->environment.configuration.keys;
        if (keys.empty() || appcastUrl.empty()) {
            return false;
        }
        const auto key = QByteArray(reinterpret_cast<const char*>(keys.front().data()),
                                    static_cast<qsizetype>(keys.front().size()))
                             .toBase64();
        const auto company = QCoreApplication::organizationName();
        const auto name = QCoreApplication::applicationName();
        // WinSparkle keeps what the user chose (a skipped version, a reminder) in the registry,
        // under the names this run of Bloom goes by, so a test never writes Bloom's own.
        const auto registryPath = (QStringLiteral("Software\\") + company + QLatin1Char('\\') +
                                   name + QStringLiteral("\\Updates"))
                                      .toLocal8Bit();
        functions_.setLanguage("en");
        functions_.setAppcastUrl(appcastUrl.c_str());
        // The update list gives versions in the compact form, which is what is compared.
        functions_.setAppDetails(company.toStdWString().c_str(), name.toStdWString().c_str(),
                                 QString::fromUtf8(core::buildVersion().data(),
                                                   static_cast<qsizetype>(
                                                       core::buildVersion().size()))
                                     .toStdWString()
                                     .c_str());
        functions_.setRegistryPath(registryPath.constData());
        if (functions_.setEddsaPublicKey(key.constData()) != 1) {
            qWarning() << "The platform updater refused the update key; no update is shown.";
            return false;
        }
        // Bloom checks, at most once a day and only when its own switch is on.
        functions_.setAutomaticCheck(0);
        functions_.setCanShutdownCallback(&canShutdown);
        functions_.setShutdownRequestCallback(&shutdownRequested);
        functions_.init();
        started_ = true;
        return true;
    }

    std::shared_ptr<Shared> state_;
    WinSparkleFunctions functions_;
    bool started_ = false;
};

} // namespace

std::unique_ptr<PlatformUpdateDriver>
makePlatformUpdateDriver(const UpdateEnvironment& environment, PlatformUpdateHooks hooks) {
    // Only a copy the installer installed is replaced by the next installer. A copy from the zip
    // is told where the next version is.
    if (environment.install.kind != update::InstallKind::WindowsInstaller ||
        !environment.configuration.configured()) {
        return nullptr;
    }
    WinSparkleFunctions functions;
    if (!loadWinSparkle(functions)) {
        return nullptr;
    }
    return std::make_unique<WinSparkleDriver>(environment, std::move(hooks), functions);
}

} // namespace bloom::ui

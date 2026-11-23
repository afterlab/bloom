#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bloom::ui {

struct UpdateEnvironment;

// The platform's own updater (ADR 0041): Sparkle on macOS, WinSparkle for a copy the Windows
// installer installed. Bloom checks and verifies its update feed first; when the feed says that
// this copy is updated by the platform's updater, Bloom hands it the address of the list that
// updater reads, taken from the verified feed. The updater then shows its own window, downloads
// in its own background with its own progress and cancel, verifies the file with the same
// public key, and installs.
class PlatformUpdateDriver {
  public:
    PlatformUpdateDriver() = default;
    PlatformUpdateDriver(const PlatformUpdateDriver&) = delete;
    PlatformUpdateDriver& operator=(const PlatformUpdateDriver&) = delete;
    virtual ~PlatformUpdateDriver() = default;

    // Shows the platform updater's window for the update at this list. `appcastUrl` and
    // `version` (the new version's label) come from a verified feed and nowhere else.
    virtual void showUpdate(const std::string& appcastUrl, const std::string& version) = 0;
};

// The end-to-end update test sets this variable for a test build (BLOOM_UPDATE_TEST_BUILD): the
// update is then installed without a question. A build that is not a test build never reads it.
inline constexpr char kUpdateTestInstallNowVariable[] = "BLOOM_UPDATE_TEST_INSTALL_NOW";

// What a driver asks of the application. Both are called on the interface thread.
struct PlatformUpdateHooks final {
    // True when Bloom can close at once, without a question: nothing unsaved, nothing running.
    // The platform's updater asks before it starts an installer that needs Bloom closed.
    std::function<bool()> readyToClose;
    // Closes Bloom the normal way.
    std::function<void()> requestClose;
};

// The two versions the platform's updater is given (#787). Its window names the installed
// version as `displayed`, the version's label (0.1.0-alpha.4, the form of the About window and
// the release names). It compares `compared`, the compact build version (0.1.0a4), with the
// appcast's sparkle:version; the appcast's sparkle:shortVersionString is the new version's label.
struct PlatformUpdaterVersions final {
    std::string displayed;
    std::string compared;
};
[[nodiscard]] PlatformUpdaterVersions platformUpdaterVersions();

// Starts the update helper in its watch mode for `version`, which the platform's updater is
// about to install: the helper waits for Bloom to exit and for that version to confirm its
// start, and puts the kept previous version back when it does not (ADR 0041). False when the
// install has no helper or the plan could not be written; the update then goes on unwatched.
[[nodiscard]] bool startUpdateWatch(const UpdateEnvironment& environment,
                                    std::string_view version);

// The driver of this platform and install, or nothing where Bloom installs updates itself
// (Linux), where the copy is not one the platform's updater can replace, or in a build without
// the updater's library.
[[nodiscard]] std::unique_ptr<PlatformUpdateDriver>
makePlatformUpdateDriver(const UpdateEnvironment& environment, PlatformUpdateHooks hooks);

} // namespace bloom::ui

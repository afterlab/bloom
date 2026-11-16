#pragma once

#include <memory>
#include <string>

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

    // Shows the platform updater's window for the update at this list. `appcastUrl` comes from
    // a verified feed and nowhere else.
    virtual void showUpdate(const std::string& appcastUrl) = 0;
};

// The driver of this platform and install, or nothing where Bloom installs updates itself
// (Linux), where the copy is not one the platform's updater can replace, or in a build without
// the updater's library.
[[nodiscard]] std::unique_ptr<PlatformUpdateDriver>
makePlatformUpdateDriver(const UpdateEnvironment& environment);

} // namespace bloom::ui

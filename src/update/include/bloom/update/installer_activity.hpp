#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// What Bloom knows of its Windows installer while it starts (#788, ADR 0041).
//
// The installer (packaging/windows/bloom.iss) holds a named mutex for as long as it runs. A Bloom
// that is opened by hand while it runs would start the version being replaced and hold its
// folder, which the installer has to rename to keep that version for going back. So such a
// Bloom shows the installer's window, which says that Bloom is being updated and opens Bloom
// when it is done, and ends at once. The installer's own start of Bloom carries an argument,
// because the installer still runs at that moment.
//
// When an update the updater started could not be installed, the installer puts the version
// that was there back, leaves a notice in its folder and starts it; that start tells the artist.
namespace bloom::update {

// The mutex the installer holds while it runs: SetupMutex in packaging/windows/bloom.iss.
inline constexpr std::string_view kWindowsSetupMutex = "AfterlabBloomSetup";

// The argument the installer starts Bloom with (bloom.iss, [Run] and DeinitializeSetup).
inline constexpr std::string_view kStartedByInstallerArgument = "--started-by-installer";

// The titles of the installer's window: an update's (bloom.iss, InitializeWizard) and Inno
// Setup's own for an install the artist started.
inline constexpr std::wstring_view kUpdatingWindowTitle = L"Updating Bloom";
inline constexpr std::wstring_view kSetupWindowTitle = L"Setup - Bloom";

// The notice the installer leaves in the installation folder when an update failed.
inline constexpr std::string_view kInstallFailureNoticeName = "update-failed";

// Removes every kStartedByInstallerArgument from argv (argv[0] stays) and lowers argc; true when
// one was there.
[[nodiscard]] bool takeStartedByInstallerArgument(int& argc, char** argv);

// True while some process holds the mutex of that name in this session. Always false outside
// Windows, where no such installer runs.
[[nodiscard]] bool installerRunning(std::string_view mutexName = kWindowsSetupMutex);

// Brings a window of the installer to the front and flashes it; false when none was found (and
// always outside Windows).
bool showInstallerWindow();

// The update that could not be installed, as the installer's notice tells it.
struct InstallFailure final {
    // The version that was not installed.
    std::string version;
    // What happened, in the installer's words.
    std::string reason;
};

// Reads and removes the installer's notice from the installation folder `installRoot`.
[[nodiscard]] std::optional<InstallFailure>
takeInstallFailure(const std::filesystem::path& installRoot);

} // namespace bloom::update

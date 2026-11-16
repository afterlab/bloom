#pragma once

#include <QString>
#include <QStringList>

namespace bloom::ui {

// The names Qt keeps Bloom's settings and per-user folders under, set once before anything reads
// a setting.
//
// Normally they are Bloom's own ("Afterlab", "Bloom") and the settings are in the operating
// system's store.
//
// The package check (cmake/package/check_package.cmake) starts a packaged Bloom to see what it
// loads. That run must never read or change the settings of the person whose computer it is, so
// the check sets BLOOM_PACKAGE_CHECK_PROFILE to a directory of its own, and then
//
//   * the settings are a file under that directory instead of the system's store, and
//   * the organization name is kPackageCheckOrganization, so the per-user data and cache folders
//     Qt names after it are not Bloom's own either.
//
// The variable is the check's; it is not a way to run Bloom with a second profile. Setting it
// at all gives up Bloom's own names: a value that is not an absolute directory does not fall
// back to them, it stops the program (ApplicationIdentity::refusal).
inline constexpr char kPackageCheckProfileVariable[] = "BLOOM_PACKAGE_CHECK_PROFILE";
inline constexpr char kPackageCheckOrganization[] = "BloomPackageCheck";

struct ApplicationIdentity final {
    QString organization;
    QString application;
    // Empty unless the package check's profile directory is in use.
    QString settingsDirectory;
    // Not empty when the check's variable is set to something that is not an absolute
    // directory. The names are then already the check's, never Bloom's own, and the program
    // must stop with this message: a mistake in a check must not become a run that reads or
    // writes the settings of the person whose computer it is.
    QString refusal;
};

// Applies the identity to QCoreApplication and QSettings and returns what it applied.
ApplicationIdentity installApplicationIdentity();

// The icon files of the package the program in `programDirectory` belongs to, smallest first:
// share/icons/hicolor/<size>x<size>/apps/id.afterlab.bloom.png beside the program's folder, as
// every Linux package and the AppImage carry them. Empty for a program that belongs to no
// package.
[[nodiscard]] QStringList packagedWindowIconFiles(const QString& programDirectory);

// Gives Bloom's windows their icon on Linux, once the application object exists. An X11 desktop
// shows the icon a window carries itself (title bar, task bar, window switcher); a Wayland
// desktop takes it from the desktop entry named above and ignores this. The icon is the
// package's own files, so a copy that was only unpacked shows it too; a program outside a
// package asks the desktop's icon theme. macOS and Windows take the icon from the bundle and
// from the program file, and nothing is set there.
void installWindowIcon();

} // namespace bloom::ui

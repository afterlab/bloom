#pragma once

#include <QString>

namespace bloom::ui {

// The names Qt keeps Bloom's settings and per-user folders under, set once before anything reads
// a setting.
//
// Normally they are Bloom's own ("Kinetik", "Bloom") and the settings are in the operating
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
// The variable is the check's; it is not a way to run Bloom with a second profile.
inline constexpr char kPackageCheckProfileVariable[] = "BLOOM_PACKAGE_CHECK_PROFILE";
inline constexpr char kPackageCheckOrganization[] = "BloomPackageCheck";

struct ApplicationIdentity final {
    QString organization;
    QString application;
    // Empty unless the package check's profile directory is in use.
    QString settingsDirectory;
};

// Applies the identity to QCoreApplication and QSettings and returns what it applied.
ApplicationIdentity installApplicationIdentity();

} // namespace bloom::ui

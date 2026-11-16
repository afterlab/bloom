#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// How this copy of Bloom was installed, which decides how it is updated (ADR 0041).
namespace bloom::update {

enum class InstallKind : std::uint8_t {
    // Not a package: a program in a build folder. It is never updated.
    Development,
    // Bloom.app on macOS: the platform's updater replaces the bundle.
    MacBundle,
    // Installed by the Windows installer: the platform's updater runs the next installer.
    WindowsInstaller,
    // An unpacked Windows zip: Bloom says where the next version is, and changes nothing.
    WindowsPortable,
    // One AppImage file: the update helper replaces the file.
    AppImage,
    // A folder the user owns (the install script, or an unpacked tar): the update helper swaps
    // the folder.
    UserFolder,
    // A package folder this user cannot write (a system-wide install): the installer is run
    // again by whoever installed it.
    ReadOnlyFolder,
    // Installed by a package manager, which owns the files: Bloom leaves updates to it.
    PackageManager,
};

[[nodiscard]] std::string_view installKindName(InstallKind kind) noexcept;

// The marker an installer leaves in the package (`install-kind` in the record directory:
// `share/bloom` on Linux, the package's own directory on Windows). Lines of `name=value`:
//   kind=script | package-manager | windows-installer
//   manager=<the package manager's name>       (kind=package-manager)
// A marker Bloom cannot read is treated as a package manager's: the files are someone else's.
struct InstallMarker final {
    std::string kind;
    std::string manager;
};
[[nodiscard]] InstallMarker parseInstallMarker(std::string_view text);

enum class InstallPlatform : std::uint8_t { Linux, MacOS, Windows };

// What was found around the running program.
struct InstallProbe final {
    InstallPlatform platform = InstallPlatform::Linux;
    // The program sits in a package (the package's record directory is beside it where the
    // layout puts it).
    bool packageLayout = false;
    // The text of the package's marker, when it has one.
    std::optional<std::string> marker;
    // Linux: the AppImage file this program was started from (the APPIMAGE variable the AppImage
    // runtime sets), when the program is inside its mount.
    std::filesystem::path appImage;
    // The package's root can be renamed by this user (its parent directory is writable).
    bool rootWritable = false;
};

struct InstallFacts final {
    InstallKind kind = InstallKind::Development;
    // The folder (or the AppImage file, or the bundle) an update replaces; empty when none is.
    std::filesystem::path root;
    // The package manager's name, when one owns the install.
    std::string manager;
};

// The decision alone, from what was found.
[[nodiscard]] InstallKind classifyInstall(const InstallProbe& probe);

// Looks around a program: `executable` is its own path, `environment` reads a variable.
using EnvironmentReader = std::function<std::optional<std::string>(std::string_view)>;
[[nodiscard]] InstallFacts detectInstall(const std::filesystem::path& executable,
                                         const EnvironmentReader& environment);

// The running program's own path; empty when the system does not say.
[[nodiscard]] std::filesystem::path currentExecutablePath();
// A reader of this process's environment.
[[nodiscard]] EnvironmentReader processEnvironment();

} // namespace bloom::update

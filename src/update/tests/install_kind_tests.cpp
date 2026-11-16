// How a copy of Bloom was installed: the decision from what is found around the program, and the
// finding itself over package trees made in a temporary directory.
#include "update_test_support.hpp"

#include <bloom/update/install_kind.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;

void write(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

[[nodiscard]] InstallProbe probe(const InstallPlatform platform, const bool package,
                                 std::optional<std::string> marker, const bool appImage,
                                 const bool writable) {
    InstallProbe result;
    result.platform = platform;
    result.packageLayout = package;
    result.marker = std::move(marker);
    result.appImage = appImage ? "/somewhere/Bloom.AppImage" : "";
    result.rootWritable = writable;
    return result;
}

} // namespace

int main() {
    Expectations checks;
    using enum InstallKind;
    const auto linux_ = InstallPlatform::Linux;
    const auto windows = InstallPlatform::Windows;
    const auto mac = InstallPlatform::MacOS;
    const std::string script = "kind=script\n";
    const std::string pacman = "# written by the package\nkind = package-manager\nmanager = pacman\n";

    checks.expect(classifyInstall(probe(linux_, false, std::nullopt, false, true)) == Development &&
                      classifyInstall(probe(linux_, false, script, true, true)) == Development,
                  "classify: a program outside a package is a development build");
    checks.expect(classifyInstall(probe(linux_, true, std::nullopt, false, true)) == UserFolder,
                  "classify: an unpacked tar the user owns is a user folder");
    checks.expect(classifyInstall(probe(linux_, true, script, false, true)) == UserFolder,
                  "classify: the install script's per-user install is a user folder");
    checks.expect(classifyInstall(probe(linux_, true, script, false, false)) == ReadOnlyFolder &&
                      classifyInstall(probe(linux_, true, std::nullopt, false, false)) ==
                          ReadOnlyFolder,
                  "classify: a folder this user cannot write is read-only");
    checks.expect(classifyInstall(probe(linux_, true, pacman, false, false)) == PackageManager &&
                      classifyInstall(probe(linux_, true, pacman, false, true)) == PackageManager,
                  "classify: a package manager's marker wins over a writable folder");
    checks.expect(classifyInstall(probe(linux_, true, "kind=something-later\n", false, true)) ==
                          PackageManager &&
                      classifyInstall(probe(linux_, true, "", false, true)) == PackageManager &&
                      classifyInstall(probe(linux_, true, "\x01\x02 not a marker", false, true)) ==
                          PackageManager,
                  "classify: a marker Bloom does not understand is someone else's install");
    checks.expect(classifyInstall(probe(linux_, true, std::nullopt, true, true)) == AppImage,
                  "classify: a package inside an AppImage is an AppImage");
    checks.expect(classifyInstall(probe(mac, true, std::nullopt, false, true)) == MacBundle &&
                      classifyInstall(probe(mac, true, std::nullopt, false, false)) == MacBundle,
                  "classify: a macOS bundle is the platform updater's");
    checks.expect(classifyInstall(probe(windows, true, "kind=windows-installer\r\n", false, true)) ==
                      WindowsInstaller,
                  "classify: the Windows installer's marker");
    checks.expect(classifyInstall(probe(windows, true, std::nullopt, false, true)) ==
                      WindowsPortable,
                  "classify: an unpacked Windows zip is portable");
    checks.expect(classifyInstall(probe(windows, true, pacman, false, true)) == PackageManager,
                  "classify: a package manager on Windows is left alone as well");

    const auto marker = parseInstallMarker(pacman);
    checks.expect(marker.kind == "package-manager" && marker.manager == "pacman",
                  "marker: the kind and the package manager's name are read");
    checks.expect(installKindName(PackageManager) == "package-manager" &&
                      installKindName(Development) == "development",
                  "names: each kind has a name for diagnostics");

    // The finding itself, where this platform's layout can be made in a folder.
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error) /
                      ("bloom-install-kind-" + std::to_string(std::random_device{}()));
    std::map<std::string, std::string, std::less<>> variables;
    const EnvironmentReader environment =
        [&variables](const std::string_view name) -> std::optional<std::string> {
        const auto found = variables.find(name);
        return found == variables.end() ? std::nullopt : std::optional(found->second);
    };
#if defined(__linux__)
    const auto tree = [&](const std::string_view name) {
        const auto root = base / name;
        write(root / "bin" / "Bloom", "program");
        write(root / "share" / "bloom" / "licenses" / "bloom" / "LICENSE", "text");
        return root;
    };
    const auto tar = tree("tar");
    const auto tarFacts = detectInstall(tar / "bin" / "Bloom", environment);
    checks.expect(tarFacts.kind == UserFolder && tarFacts.root == tar,
                  "detect: an unpacked tar is a user folder, replaced as a whole");

    const auto scripted = tree("scripted");
    write(scripted / "share" / "bloom" / "install-kind", script);
    checks.expect(detectInstall(scripted / "bin" / "Bloom", environment).kind == UserFolder,
                  "detect: the install script's marker");

    const auto packaged = tree("packaged");
    write(packaged / "share" / "bloom" / "install-kind", pacman);
    const auto packagedFacts = detectInstall(packaged / "bin" / "Bloom", environment);
    checks.expect(packagedFacts.kind == PackageManager && packagedFacts.manager == "pacman",
                  "detect: a package manager's install names its manager");

    const auto build = base / "build";
    write(build / "bin" / "Bloom", "program");
    checks.expect(detectInstall(build / "bin" / "Bloom", environment).kind == Development &&
                      detectInstall({}, environment).kind == Development,
                  "detect: a program without the package's records is a development build");

    // An AppImage: the runtime sets APPIMAGE and APPDIR, and the program is inside APPDIR.
    const auto mounted = tree("mount");
    write(base / "Bloom.AppImage", "image");
    variables["APPIMAGE"] = (base / "Bloom.AppImage").string();
    variables["APPDIR"] = mounted.string();
    const auto imageFacts = detectInstall(mounted / "bin" / "Bloom", environment);
    checks.expect(imageFacts.kind == AppImage && imageFacts.root == base / "Bloom.AppImage",
                  "detect: a program inside an AppImage's mount is replaced as that one file");
    // The variables are inherited by whatever an AppImage starts; another install is not it.
    checks.expect(detectInstall(tar / "bin" / "Bloom", environment).kind == UserFolder,
                  "detect: an install started from an AppImage is not that AppImage");
    variables.clear();

    // A folder whose parent this user cannot change.
    const auto locked = base / "locked";
    const auto inside = locked / "bloom";
    write(inside / "bin" / "Bloom", "program");
    write(inside / "share" / "bloom" / "licenses" / "bloom" / "LICENSE", "text");
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec);
    // The administrator can write anywhere, so the case says nothing when run as one.
    const bool readOnlyHolds = ::access(locked.c_str(), W_OK) != 0;
    const auto lockedKind = detectInstall(inside / "bin" / "Bloom", environment).kind;
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
    checks.expect(!readOnlyHolds || lockedKind == ReadOnlyFolder,
                  "detect: an install whose folder this user cannot replace is read-only");
#elif defined(_WIN32)
    const auto portable = base / "portable";
    write(portable / "Bloom.exe", "program");
    write(portable / "licenses" / "bloom" / "LICENSE", "text");
    checks.expect(detectInstall(portable / "Bloom.exe", environment).kind == WindowsPortable,
                  "detect: an unpacked zip is portable");
    const auto installed = base / "installed";
    write(installed / "Bloom.exe", "program");
    write(installed / "licenses" / "bloom" / "LICENSE", "text");
    write(installed / "install-kind", "kind=windows-installer\r\n");
    checks.expect(detectInstall(installed / "Bloom.exe", environment).kind == WindowsInstaller,
                  "detect: the installer's marker");
    write(base / "build" / "Bloom.exe", "program");
    checks.expect(detectInstall(base / "build" / "Bloom.exe", environment).kind == Development,
                  "detect: a program without the package's records is a development build");
#elif defined(__APPLE__)
    const auto bundle = base / "Bloom.app";
    write(bundle / "Contents" / "MacOS" / "Bloom", "program");
    write(bundle / "Contents" / "Resources" / "licenses" / "bloom" / "LICENSE", "text");
    const auto bundleFacts = detectInstall(bundle / "Contents" / "MacOS" / "Bloom", environment);
    checks.expect(bundleFacts.kind == MacBundle && bundleFacts.root == bundle,
                  "detect: a program inside a bundle with its records is the bundle");
    write(base / "build" / "Bloom.app" / "Contents" / "MacOS" / "Bloom", "program");
    checks.expect(detectInstall(base / "build" / "Bloom.app" / "Contents" / "MacOS" / "Bloom",
                                environment)
                          .kind == Development,
                  "detect: a bundle without the package's records is a development build");
#endif
    checks.expect(!currentExecutablePath().empty(), "the running program's own path is found");
    std::filesystem::remove_all(base, error);
    return checks.result();
}

// Between a verified download and the update helper: the new version is put beside the
// installed one, the plan and the helper's copy are written, and a start settles what an
// earlier run left.
#include "unique_temp_dir.hpp"
#include "update_test_support.hpp"

#include <bloom/update/staging.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;
using std::filesystem::path;

void write(const path& file, const std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

[[nodiscard]] std::string read(const path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] ApplyPlan swapPlan(const path& root, const UpdateFolders& folders,
                                 const std::string_view version) {
    InstallFacts install;
    install.kind = InstallKind::UserFolder;
    install.root = root / "opt" / "bloom";
    return makeSwapPlan(install, root / "opt" / "prepared", version, folders, 7);
}

} // namespace

int main() {
    Expectations checks;
    std::error_code error;
    const auto base = bloom::test_support::processTempDirectory() /
                      ("bloom-update-staging-" + std::to_string(std::random_device{}()));

    // An AppImage is put beside the installed one, executable, and the download is gone.
    {
        const auto installed = base / "appimage" / "Applications" / "Bloom.AppImage";
        const auto download = base / "appimage" / "data" / "download" / "Bloom-2.AppImage";
        write(installed, "old");
        write(download, "new");
        const auto staged = stageAppImage(download, installed);
        checks.expect(staged.succeeded() && staged.staged.parent_path() == installed.parent_path() &&
                          staged.staged != installed && read(staged.staged) == "new",
                      "appimage: the new file is beside the installed one");
        checks.expect(read(installed) == "old", "appimage: the installed file is untouched");
        checks.expect(!std::filesystem::exists(download), "appimage: the download is moved");
#if !defined(_WIN32)
        checks.expect((std::filesystem::status(staged.staged).permissions() &
                       std::filesystem::perms::owner_exec) != std::filesystem::perms::none,
                      "appimage: the new file can be run");
#endif
        InstallFacts install;
        install.kind = InstallKind::AppImage;
        install.root = installed;
        UpdateFolders folders{base / "appimage" / "data" / "updates"};
        const auto plan = makeSwapPlan(install, staged.staged, "2.0.0", folders, 99);
        checks.expect(plan.mode == ApplyMode::Swap && plan.installed == installed &&
                          plan.staged == staged.staged && plan.program == installed &&
                          plan.previous.parent_path() == installed.parent_path() &&
                          plan.previous != installed && plan.version == "2.0.0" &&
                          plan.processId == 99 && plan.confirmation == folders.confirmation() &&
                          plan.notice == folders.notice(),
                      "appimage: the plan swaps the file and starts it");
        removeStagingLeftovers(installed);
        checks.expect(!std::filesystem::exists(staged.staged) && read(installed) == "old",
                      "appimage: a prepared file that was never applied is removed");
    }

    // The plan and the helper's copy.
    {
        const auto root = base / "plan";
        UpdateFolders folders{root / "data" / "updates"};
        const auto plan = swapPlan(root, folders, "2.0.0");
        checks.expect(plan.program == root / "opt" / "bloom" / "bin" / "Bloom",
                      "plan: a folder install starts its program");
        write(root / "opt" / "bloom" / "libexec" / "helper", "helper program");
        write(folders.journal(), "done an earlier update\n");
        const auto helper =
            writePlanAndHelper(plan, root / "opt" / "bloom" / "libexec" / "helper", folders);
        checks.expect(helper.has_value() && *helper == folders.helper() &&
                          read(*helper) == "helper program" &&
                          helper->string().find((root / "opt").string()) == std::string::npos,
                      "plan: the helper is copied out of the installation");
        const auto written = readApplyPlan(folders.plan());
        checks.expect(written.has_value() && written->staged == plan.staged &&
                          written->version == "2.0.0",
                      "plan: the plan is written for the helper");
        checks.expect(!std::filesystem::exists(folders.journal()),
                      "plan: an earlier plan's journal does not end the new one");
        checks.expect(!writePlanAndHelper(plan, root / "no-helper", folders).has_value(),
                      "plan: without the helper nothing is started");
    }

    // The plan for an update the platform's own updater installs.
    {
        const auto root = base / "watch";
        UpdateFolders folders{root / "data" / "updates"};
        InstallFacts bundle;
        bundle.kind = InstallKind::MacBundle;
        bundle.root = root / "Applications" / "Bloom.app";
        const auto mac = makeWatchPlan(bundle, "2.0.0", folders, 11);
        checks.expect(mac.mode == ApplyMode::Watch && mac.installed == bundle.root &&
                          mac.staged.empty() &&
                          mac.previous == folders.root / "previous" / "Bloom.app" &&
                          mac.program == bundle.root / "Contents" / "MacOS" / "Bloom" &&
                          mac.version == "2.0.0" && mac.processId == 11 &&
                          mac.confirmation == folders.confirmation() &&
                          mac.notice == folders.notice(),
                      "watch: a bundle's previous version is kept in the update folder");
        InstallFacts installed;
        installed.kind = InstallKind::WindowsInstaller;
        installed.root = root / "Programs" / "Bloom";
        const auto windows = makeWatchPlan(installed, "2.0.0", folders, 12);
        checks.expect(windows.mode == ApplyMode::Watch &&
                          windows.previous.parent_path() == installed.root.parent_path() &&
                          windows.previous.filename() == "Bloom.previous" &&
                          windows.program == installed.root / "Bloom.exe",
                      "watch: an installed folder's previous version is beside it");
        checks.expect(windows.confirmTimeout > ApplyPlan{}.confirmTimeout,
                      "watch: the helper waits longer for a start an installer comes before");
        InstallFacts folder;
        folder.kind = InstallKind::UserFolder;
        folder.root = root / "opt" / "bloom";
        checks.expect(watchedPreviousVersion(folder, folders).empty(),
                      "watch: an install Bloom replaces itself has no watched version");
        write(root / "helper", "helper program");
        const auto helper = writePlanAndHelper(windows, root / "helper", folders);
        const auto written = readApplyPlan(folders.plan());
        checks.expect(helper.has_value() && written.has_value() &&
                          written->mode == ApplyMode::Watch &&
                          written->previous == windows.previous &&
                          written->confirmTimeout == windows.confirmTimeout,
                      "watch: the plan is written for the helper as it was made");
        // The new version starts while the helper still watches: the plan is the helper's.
        const auto result = settleAtStartup(folders, "2.0.0");
        checks.expect(!result.finishPlan && std::filesystem::exists(folders.plan()),
                      "watch: a start leaves a watched plan to the helper that watches it");
    }

    // A start.
    {
        const auto root = base / "start";
        UpdateFolders folders{root / "data" / "updates"};
        auto result = settleAtStartup(folders, "1.0.0");
        checks.expect(readStartConfirmation(folders.confirmation()) == std::string("1.0.0") &&
                          !result.restoredFrom.has_value() && !result.finishPlan,
                      "start: a version confirms its start");

        checks.expect(writeStartConfirmation(folders.notice(), "2.0.0"), "start: a notice");
        result = settleAtStartup(folders, "1.0.0");
        checks.expect(result.restoredFrom == std::string("2.0.0") &&
                          !std::filesystem::exists(folders.notice()),
                      "start: a restored version is told once which version could not start");

        // The swap happened and this is the new version: only the helper's end is missing.
        write(root / "opt" / "prepared" / "file", "x");
        checks.expect(writeApplyPlan(folders.plan(), swapPlan(root, folders, "2.0.0")),
                      "start: a plan");
        result = settleAtStartup(folders, "2.0.0");
        checks.expect(result.finishPlan && std::filesystem::exists(folders.plan()),
                      "start: the new version lets the helper finish its plan");

        // The update did not happen: what it prepared is removed.
        result = settleAtStartup(folders, "1.0.0");
        checks.expect(!result.finishPlan && !std::filesystem::exists(folders.plan()),
                      "start: a plan that was never carried out is dropped");

        // A finished plan.
        checks.expect(writeApplyPlan(folders.plan(), swapPlan(root, folders, "2.0.0")),
                      "start: a plan");
        write(folders.journal(), "exited\nswapped\ndone the new version started\n");
        result = settleAtStartup(folders, "2.0.0");
        checks.expect(!result.finishPlan && !std::filesystem::exists(folders.plan()) &&
                          !std::filesystem::exists(folders.journal()),
                      "start: a finished plan is cleared away");
    }

#if defined(__linux__)
    // A package tar is unpacked beside the installation, with the installation's own records.
    // (Linux only: a folder installation is replaced by the platform's updater elsewhere.)
    {
        const auto root = base / "folder";
        const auto installed = root / "opt" / "bloom";
        write(installed / "bin" / "Bloom", "old program");
        write(installed / "share" / "bloom" / "install-kind", "install-script\n");
        write(installed / "share" / "bloom" / "install-manifest", "bin/Bloom\n");
        write(root / "package" / "Bloom-2.0.0-linux-x86_64" / "bin" / "Bloom", "new program");
        write(root / "package" / "Bloom-2.0.0-linux-x86_64" / "share" / "bloom" / "LICENSE", "l");
        write(root / "other" / "something" / "readme", "not a package");
        const auto tar = root / "Bloom.tar.xz";
        const auto other = root / "other.tar.xz";
        const bool made =
            std::system(("tar -cJf '" + tar.string() + "' -C '" + (root / "package").string() +
                         "' Bloom-2.0.0-linux-x86_64")
                            .c_str()) == 0 &&
            std::system(("tar -cJf '" + other.string() + "' -C '" + (root / "other").string() +
                         "' something")
                            .c_str()) == 0;
        checks.expect(made, "folder: the test's packages are made");
        const auto never = [] { return false; };
        const auto staged = stageFolder(tar, installed, never);
        checks.expect(staged.succeeded() && read(staged.staged / "bin" / "Bloom") == "new program" &&
                          staged.staged.parent_path().parent_path() == installed.parent_path(),
                      "folder: the package is unpacked beside the installation");
        checks.expect(read(staged.staged / "share" / "bloom" / "install-kind") == "install-script\n" &&
                          read(staged.staged / "share" / "bloom" / "install-manifest") ==
                              "bin/Bloom\n",
                      "folder: the installation's records are carried into the new version");
        checks.expect(read(installed / "bin" / "Bloom") == "old program",
                      "folder: the installation is untouched");
        removeStagingLeftovers(installed);
        checks.expect(!std::filesystem::exists(staged.staged),
                      "folder: a prepared folder that was never applied is removed");
        const auto refused = stageFolder(other, installed, never);
        checks.expect(!refused.succeeded() && !refused.error.empty(),
                      "folder: a tar that holds no Bloom is refused (" + refused.error + ")");
        const auto cancelled = stageFolder(tar, installed, [] { return true; });
        checks.expect(!cancelled.succeeded(), "folder: a cancelled unpacking prepares nothing");
    }
#endif

    std::filesystem::remove_all(base, error);
    return checks.result();
}

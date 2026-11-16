#include <bloom/update/staging.hpp>

#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>

extern char** environ; // NOLINT(readability-redundant-declaration)
#endif

namespace bloom::update {
namespace {

constexpr std::string_view kStagingPrefix = ".bloom-update-";

[[nodiscard]] std::filesystem::path beside(const std::filesystem::path& path,
                                           const std::string_view suffix) {
    auto result = path;
    result += suffix;
    return result;
}

// The folder a new version is unpacked into: a hidden sibling of the installation.
[[nodiscard]] std::filesystem::path stagingFolder(const std::filesystem::path& installed) {
    return installed.parent_path() /
           (std::string(kStagingPrefix) + installed.filename().string());
}

#if defined(__linux__)
// Runs a program and waits for it; `cancelled` ends it early. True when it exited with 0.
[[nodiscard]] bool run(std::vector<std::string> arguments, const std::function<bool()>& cancelled) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t child = 0;
    if (::posix_spawnp(&child, argv.front(), nullptr, nullptr, argv.data(), environ) != 0) {
        return false;
    }
    while (true) {
        int status = 0;
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (done < 0 && errno != EINTR) {
            return false;
        }
        if (cancelled && cancelled()) {
            ::kill(child, SIGTERM);
            ::waitpid(child, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
#endif

} // namespace

std::filesystem::path UpdateFolders::helper() const {
#if defined(_WIN32)
    return root / "bloom-update-helper.exe";
#else
    return root / "bloom-update-helper";
#endif
}

StagingResult stageFolder(const std::filesystem::path& tar,
                          const std::filesystem::path& installedRoot,
                          const std::function<bool()>& cancelled) {
#if defined(__linux__)
    if (cancelled && cancelled()) {
        return {{}, "cancelled"};
    }
    std::error_code error;
    const auto folder = stagingFolder(installedRoot);
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    if (error) {
        return {{}, "the folder beside the installation cannot be written"};
    }
    // The tar was verified against the signed feed before this point; it holds one folder.
    if (!run({"tar", "-xJf", tar.string(), "-C", folder.string()}, cancelled)) {
        std::filesystem::remove_all(folder, error);
        return {{}, cancelled && cancelled() ? "cancelled" : "the package could not be unpacked"};
    }
    std::filesystem::path unpacked;
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        unpacked = entry.path();
        ++entries;
    }
    if (entries != 1 || !std::filesystem::is_regular_file(unpacked / "bin" / "Bloom", error) ||
        !std::filesystem::is_directory(unpacked / "share" / "bloom", error)) {
        std::filesystem::remove_all(folder, error);
        return {{}, "the package does not hold a Bloom installation"};
    }
    // What the installer wrote into the installed folder belongs to the installation, not to a
    // version: the marker that says how it was installed, and the installer's list of files.
    for (const std::string_view record : {"install-kind", "install-manifest"}) {
        const auto source = installedRoot / "share" / "bloom" / record;
        if (std::filesystem::exists(source, error)) {
            std::filesystem::copy_file(source, unpacked / "share" / "bloom" / record,
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                std::filesystem::remove_all(folder, error);
                return {{}, "the installation's records could not be carried over"};
            }
        }
    }
    return {unpacked, {}};
#else
    (void)tar;
    (void)installedRoot;
    (void)cancelled;
    return {{}, "a folder installation is replaced by the platform's updater here"};
#endif
}

StagingResult stageAppImage(const std::filesystem::path& appImage,
                            const std::filesystem::path& installedFile) {
    std::error_code error;
    const auto staged = installedFile.parent_path() /
                        (std::string(kStagingPrefix) + installedFile.filename().string());
    std::filesystem::remove_all(staged, error);
    // Moved when the download is on the same volume, copied otherwise.
    std::filesystem::rename(appImage, staged, error);
    if (error) {
        error.clear();
        std::filesystem::copy_file(appImage, staged,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return {{}, "the folder of the AppImage cannot be written"};
        }
        std::filesystem::remove(appImage, error);
        error.clear();
    }
    using std::filesystem::perms;
    std::filesystem::permissions(staged,
                                 perms::owner_all | perms::group_read | perms::group_exec |
                                     perms::others_read | perms::others_exec,
                                 error);
    if (error) {
        std::filesystem::remove(staged, error);
        return {{}, "the new AppImage could not be made executable"};
    }
    return {staged, {}};
}

void removeStagingLeftovers(const std::filesystem::path& installed) {
    std::error_code error;
    std::filesystem::remove_all(stagingFolder(installed), error);
    std::filesystem::remove_all(beside(installed, ".failed"), error);
    std::filesystem::remove_all(beside(beside(installed, ".previous"), ".old"), error);
}

ApplyPlan makeSwapPlan(const InstallFacts& install, const std::filesystem::path& staged,
                       const std::string_view version, const UpdateFolders& folders,
                       const std::uint64_t processId) {
    ApplyPlan plan;
    plan.mode = ApplyMode::Swap;
    plan.processId = processId;
    plan.installed = install.root;
    plan.staged = staged;
    plan.previous = beside(install.root, ".previous");
    plan.program =
        install.kind == InstallKind::AppImage ? install.root : install.root / "bin" / "Bloom";
    plan.confirmation = folders.confirmation();
    plan.notice = folders.notice();
    plan.version = std::string(version);
    return plan;
}

std::optional<std::filesystem::path>
writePlanAndHelper(const ApplyPlan& plan, const std::filesystem::path& installedHelper,
                   const UpdateFolders& folders) {
    std::error_code error;
    std::filesystem::create_directories(folders.root, error);
    std::filesystem::remove(folders.journal(), error);
    std::filesystem::remove(beside(folders.plan(), ".log"), error);
    error.clear();
    const auto helper = folders.helper();
    std::filesystem::copy_file(installedHelper, helper,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return std::nullopt;
    }
#if !defined(_WIN32)
    std::filesystem::permissions(helper, std::filesystem::perms::owner_all, error);
    if (error) {
        return std::nullopt;
    }
#endif
    if (!writeApplyPlan(folders.plan(), plan)) {
        return std::nullopt;
    }
    return helper;
}

std::filesystem::path installedHelperProgram(const std::filesystem::path& executable) {
    const auto directory = executable.parent_path();
#if defined(_WIN32)
    const auto helper = directory / "bloom-update-helper.exe";
#elif defined(__APPLE__)
    const auto helper = directory / "bloom-update-helper";
#else
    // <root>/bin/Bloom and <root>/libexec/bloom/bloom-update-helper
    const auto helper = directory.parent_path() / "libexec" / "bloom" / "bloom-update-helper";
#endif
    std::error_code error;
    return std::filesystem::is_regular_file(helper, error) ? helper : std::filesystem::path{};
}

StartupResult settleAtStartup(const UpdateFolders& folders, const std::string_view runningVersion) {
    StartupResult result;
    std::error_code error;
    std::filesystem::create_directories(folders.root, error);
    (void)writeStartConfirmation(folders.confirmation(), runningVersion);
    result.restoredFrom = takeRestoreNotice(folders.notice());

    const auto plan = readApplyPlan(folders.plan());
    if (!plan.has_value()) {
        std::filesystem::remove(folders.plan(), error);
        return result;
    }
    // A journal that ends in `done` belongs to a finished plan.
    std::ifstream journal(folders.journal());
    bool done = false;
    for (std::string line; std::getline(journal, line);) {
        done = done || line == "done" || line.starts_with("done ");
    }
    if (!done && plan->mode == ApplyMode::Swap) {
        if (plan->version == runningVersion) {
            result.finishPlan = true;
            return result;
        }
        // The update did not happen. Remove what it prepared; the next check offers it again.
        removeStagingLeftovers(plan->installed);
    }
    if (done || plan->mode == ApplyMode::Swap) {
        std::filesystem::remove(folders.plan(), error);
        std::filesystem::remove(folders.journal(), error);
        if (plan->mode == ApplyMode::Swap) {
            removeStagingLeftovers(plan->installed);
        }
    }
    return result;
}

std::uint64_t currentProcessId() noexcept {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

} // namespace bloom::update

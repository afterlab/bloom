#pragma once

#include <bloom/update/apply.hpp>
#include <bloom/update/install_kind.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Between a verified download and the update helper: the new version is put beside the
// installed one (same volume, so the helper only renames), the helper is copied out of the
// installation, and the plan is written.
namespace bloom::update {

// Where an update keeps its files in the user's data folder.
struct UpdateFolders final {
    std::filesystem::path root;

    [[nodiscard]] std::filesystem::path downloads() const { return root / "download"; }
    [[nodiscard]] std::filesystem::path plan() const { return root / "plan"; }
    [[nodiscard]] std::filesystem::path journal() const { return root / "plan.journal"; }
    [[nodiscard]] std::filesystem::path confirmation() const { return root / "started"; }
    [[nodiscard]] std::filesystem::path notice() const { return root / "restored"; }
    [[nodiscard]] std::filesystem::path helper() const;
};

struct StagingResult final {
    // The prepared version, ready for the helper; empty when it could not be prepared.
    std::filesystem::path staged;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept { return !staged.empty(); }
};

// Unpacks a verified package tar beside the installed folder, and carries the installer's own
// records (the marker and its list of files) over into the new folder. Linux only; elsewhere the
// platform's updater installs.
[[nodiscard]] StagingResult stageFolder(const std::filesystem::path& tar,
                                        const std::filesystem::path& installedRoot,
                                        const std::function<bool()>& cancelled);

// Puts a verified AppImage beside the installed one, executable.
[[nodiscard]] StagingResult stageAppImage(const std::filesystem::path& appImage,
                                          const std::filesystem::path& installedFile);

// Removes what an earlier update left beside the installation: prepared versions that were
// never applied. The kept previous version stays.
void removeStagingLeftovers(const std::filesystem::path& installed);

// The plan for the helper to replace `install` with `staged` and start it.
[[nodiscard]] ApplyPlan makeSwapPlan(const InstallFacts& install,
                                     const std::filesystem::path& staged,
                                     std::string_view version, const UpdateFolders& folders,
                                     std::uint64_t processId);

// Copies the helper out of the installation (which it is about to move) and writes the plan.
// The path of the copy to start with `--plan <folders.plan()>`, or nothing.
[[nodiscard]] std::optional<std::filesystem::path>
writePlanAndHelper(const ApplyPlan& plan, const std::filesystem::path& installedHelper,
                   const UpdateFolders& folders);

// The helper inside the package this program runs from; empty when it is not there.
[[nodiscard]] std::filesystem::path installedHelperProgram(const std::filesystem::path& executable);

// At start-up. Writes this version's start confirmation, and settles a plan an earlier run left
// unfinished: when this version is the plan's, the swap happened and only its end is missing
// (true: the caller starts the helper once more with the plan, which then finishes at once);
// otherwise the update did not happen and what it prepared is removed.
struct StartupResult final {
    // The version of an update that could not start, when this version was put back for it.
    std::optional<std::string> restoredFrom;
    bool finishPlan = false;
};
[[nodiscard]] StartupResult settleAtStartup(const UpdateFolders& folders,
                                            std::string_view runningVersion);

// This process's id, as the helper is told it.
[[nodiscard]] std::uint64_t currentProcessId() noexcept;

} // namespace bloom::update

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Applying a prepared update (ADR 0041): what `bloom-update-helper` does after Bloom has exited.
// This part uses the standard library and operating system calls only, so the helper is a small
// program with nothing of Bloom's to load, and the same code is tested in-process with a host
// that fails on command.
namespace bloom::update {

enum class ApplyMode : std::uint8_t {
    // Replace `installed` (a folder, or one file such as an AppImage) with `staged`.
    Swap,
    // The platform's own updater replaces `installed`; keep `previous` and put it back when the
    // new version does not start.
    Watch,
};

// What the helper is told, written by Bloom as a file of `name=value` lines beside which the
// helper keeps its journal (`<plan>.journal`).
struct ApplyPlan final {
    ApplyMode mode = ApplyMode::Swap;
    // The process to wait for: the running Bloom.
    std::uint64_t processId = 0;
    // The installed version: the package folder, the AppImage file, or the bundle.
    std::filesystem::path installed;
    // The prepared version, on the same volume as `installed` (Swap).
    std::filesystem::path staged;
    // Where the version that is replaced is kept, for one update.
    std::filesystem::path previous;
    // The program to start afterwards, and its arguments.
    std::filesystem::path program;
    std::vector<std::string> arguments;
    // The file a Bloom that has started writes its version into, and the version expected there.
    std::filesystem::path confirmation;
    std::string version;
    // Written when the previous version was put back, for that version to tell the user.
    std::filesystem::path notice;
    // How long the helper waits for Bloom to exit before it gives up (Swap). A watch has no
    // such limit.
    std::chrono::seconds exitTimeout{600};
    std::chrono::seconds confirmTimeout{180};
};

[[nodiscard]] bool writeApplyPlan(const std::filesystem::path& path, const ApplyPlan& plan);
[[nodiscard]] std::optional<ApplyPlan> readApplyPlan(const std::filesystem::path& path);

// What a started Bloom writes, and what the helper reads back.
[[nodiscard]] bool writeStartConfirmation(const std::filesystem::path& path,
                                          std::string_view version);
[[nodiscard]] std::optional<std::string> readStartConfirmation(const std::filesystem::path& path);
// The notice a restored version finds: the version that could not start. Reading removes it.
[[nodiscard]] std::optional<std::string> takeRestoreNotice(const std::filesystem::path& path);

enum class ApplyOutcome : std::uint8_t {
    // The new version is installed and confirmed its start.
    Updated,
    // The new version did not start (or could not be put in place); the previous one is back and
    // was started.
    Restored,
    // Watch: the platform's updater did not replace anything; the installed version started again.
    Unchanged,
    // Bloom did not exit in time; nothing was touched.
    StillRunning,
    // An earlier run of this plan already finished it.
    AlreadyDone,
    // The plan cannot be carried out and nothing could be restored; the detail says what is where.
    Failed,
};

struct ApplyResult final {
    ApplyOutcome outcome = ApplyOutcome::Failed;
    std::string detail;
};

// The system as the steps see it. The real one is `makeSystemApplyHost`; tests give their own.
class ApplyHost {
  public:
    ApplyHost() = default;
    ApplyHost(const ApplyHost&) = delete;
    ApplyHost& operator=(const ApplyHost&) = delete;
    virtual ~ApplyHost() = default;

    [[nodiscard]] virtual bool processRunning(std::uint64_t processId) = 0;
    // Starts a program on its own, not as a child that ends with the helper. Its process id, or
    // nothing when it could not be started.
    [[nodiscard]] virtual std::optional<std::uint64_t>
    launch(const std::filesystem::path& program, const std::vector<std::string>& arguments) = 0;
    virtual void terminate(std::uint64_t processId) = 0;
    // Lets `duration` pass. Tests advance their own clock instead of waiting.
    virtual void wait(std::chrono::milliseconds duration) = 0;
    // Moves a folder or file to a name that does not exist, on one volume.
    [[nodiscard]] virtual std::error_code rename(const std::filesystem::path& from,
                                                 const std::filesystem::path& to) = 0;
    // Exchanges two existing folders or files in one step that cannot be seen half done. An
    // error where the system or the volume has no such step; the steps then use two moves.
    [[nodiscard]] virtual std::error_code exchange(const std::filesystem::path& first,
                                                   const std::filesystem::path& second) = 0;
    // A text that names the folder or file itself, whatever its path, so a run that was stopped
    // can tell which of two is where. Empty when the system gives none.
    [[nodiscard]] virtual std::string identity(const std::filesystem::path& path) = 0;
};

[[nodiscard]] std::unique_ptr<ApplyHost> makeSystemApplyHost();

// Carries out a plan, or finishes one an earlier run was interrupted in. `journal` is the file
// the steps are recorded in.
[[nodiscard]] ApplyResult applyUpdate(const ApplyPlan& plan, const std::filesystem::path& journal,
                                      ApplyHost& host);

} // namespace bloom::update

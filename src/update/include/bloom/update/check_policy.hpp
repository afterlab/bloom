#pragma once

#include <bloom/update/feed.hpp>
#include <bloom/update/install_kind.hpp>
#include <bloom/update/release_version.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::update {

// The automatic check: on unless the user turned it off, and at most once a day. The clock is
// the caller's, as seconds since 1970, so the rule is tested without waiting.
inline constexpr std::int64_t kAutomaticCheckIntervalSeconds = 24 * 60 * 60;

struct AutomaticCheckState final {
    bool enabled = true;
    // When the last check (automatic or asked for) was started; nothing before the first.
    std::optional<std::int64_t> lastCheckSeconds;
};

// Whether an automatic check may start now. A last check that lies in the future (the clock was
// set back) counts as a day old: the next check is then recorded at the present time.
[[nodiscard]] bool automaticCheckDue(const AutomaticCheckState& state,
                                     std::int64_t nowSeconds) noexcept;

// What a verified feed means for this copy of Bloom.
enum class UpdateAction : std::uint8_t {
    // This is the newest version of its channel (or newer than the feed).
    UpToDate,
    // The feed has nothing for this platform.
    NotPublished,
    // A build folder: never updated.
    NotInstalled,
    // macOS and the Windows installer: the platform's updater reads `appcastUrl` and installs.
    PlatformUpdater,
    // Linux folders the user owns: download `file`, then the helper swaps the folder.
    ReplaceFolder,
    // Linux AppImage: download `file` (as a delta when it can be), then the helper swaps the file.
    ReplaceAppImage,
    // Owned by a package manager: say that a newer version exists and that it installs it.
    LeaveToPackageManager,
    // A folder this user cannot write: say that the installer must be run again.
    RunInstallerAgain,
    // An unpacked Windows zip: say where the next version is.
    DownloadManually,
};

struct UpdateDecision final {
    UpdateAction action = UpdateAction::UpToDate;
    // The feed's file for this install, where the action downloads or names one.
    std::optional<FeedFile> file;
    // The address the platform's updater reads (PlatformUpdater).
    std::string appcastUrl;
};

[[nodiscard]] UpdateDecision decideUpdate(const UpdateFeed& feed, const ReleaseVersion& current,
                                          InstallKind install, std::string_view platformKey);

} // namespace bloom::update

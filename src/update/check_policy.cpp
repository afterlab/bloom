#include <bloom/update/check_policy.hpp>

namespace bloom::update {

bool automaticCheckDue(const AutomaticCheckState& state, const std::int64_t nowSeconds) noexcept {
    if (!state.enabled) {
        return false;
    }
    if (!state.lastCheckSeconds.has_value()) {
        return true;
    }
    const auto last = *state.lastCheckSeconds;
    if (last > nowSeconds) {
        return true;
    }
    return nowSeconds - last >= kAutomaticCheckIntervalSeconds;
}

UpdateDecision decideUpdate(const UpdateFeed& feed, const ReleaseVersion& current,
                            const InstallKind install, const std::string_view platformKey) {
    UpdateDecision decision;
    if (install == InstallKind::Development) {
        decision.action = UpdateAction::NotInstalled;
        return decision;
    }
    // Only a newer version is ever offered: an old signed feed served again changes nothing.
    if (!(feed.version > current)) {
        decision.action = UpdateAction::UpToDate;
        return decision;
    }
    const FeedPlatform* platform = feed.platform(platformKey);
    if (platform == nullptr) {
        decision.action = UpdateAction::NotPublished;
        return decision;
    }
    const auto withFile = [&decision, platform](const UpdateAction action,
                                                const std::string_view kind) {
        const FeedFile* file = platform->file(kind);
        if (file != nullptr) {
            decision.file = *file;
        }
        decision.action = file == nullptr ? UpdateAction::NotPublished : action;
    };
    switch (install) {
    case InstallKind::MacBundle:
    case InstallKind::WindowsInstaller:
        decision.appcastUrl = platform->appcastUrl;
        decision.action = decision.appcastUrl.empty() ? UpdateAction::NotPublished
                                                      : UpdateAction::PlatformUpdater;
        break;
    case InstallKind::UserFolder:
        withFile(UpdateAction::ReplaceFolder, "tar");
        break;
    case InstallKind::AppImage:
        withFile(UpdateAction::ReplaceAppImage, "appimage");
        break;
    case InstallKind::PackageManager:
        decision.action = UpdateAction::LeaveToPackageManager;
        break;
    case InstallKind::ReadOnlyFolder:
        decision.action = UpdateAction::RunInstallerAgain;
        break;
    case InstallKind::WindowsPortable:
        withFile(UpdateAction::DownloadManually, "zip");
        break;
    case InstallKind::Development:
        decision.action = UpdateAction::NotInstalled;
        break;
    }
    return decision;
}

} // namespace bloom::update

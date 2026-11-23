#include <bloom/ui/platform_update_driver.hpp>

#include <bloom/core/version.hpp>
#include <bloom/ui/update_service.hpp>
#include <bloom/update/staging.hpp>

namespace bloom::ui {

PlatformUpdaterVersions platformUpdaterVersions() {
    return {.displayed = std::string(core::version()),
            .compared = std::string(core::buildVersion())};
}

bool startUpdateWatch(const UpdateEnvironment& environment, const std::string_view version) {
    if (environment.installedHelper.empty() || !environment.startHelper ||
        update::watchedPreviousVersion(environment.install, environment.folders).empty()) {
        return false;
    }
    const auto plan = update::makeWatchPlan(environment.install, version, environment.folders,
                                            environment.processId);
    const auto helper =
        update::writePlanAndHelper(plan, environment.installedHelper, environment.folders);
    return helper.has_value() && environment.startHelper(*helper, environment.folders.plan());
}

#if !defined(BLOOM_UI_HAS_PLATFORM_UPDATER)
// No platform updater in this build: Linux, or a build without the updater's library.
std::unique_ptr<PlatformUpdateDriver>
makePlatformUpdateDriver(const UpdateEnvironment& /*environment*/, PlatformUpdateHooks /*hooks*/) {
    return nullptr;
}
#endif

} // namespace bloom::ui

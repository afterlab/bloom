#include <bloom/ui/platform_update_driver.hpp>

#include <bloom/ui/update_service.hpp>

namespace bloom::ui {

#if !defined(BLOOM_UI_HAS_PLATFORM_UPDATER)
// No platform updater in this build: Linux, or a build without the updater's library.
std::unique_ptr<PlatformUpdateDriver>
makePlatformUpdateDriver(const UpdateEnvironment& /*environment*/) {
    return nullptr;
}
#endif

} // namespace bloom::ui

#include <bloom/update/configuration.hpp>

// The one translation unit the update settings reach (src/update/CMakeLists.txt).
#if !defined(BLOOM_UPDATE_FEED_URL) || !defined(BLOOM_UPDATE_PUBLIC_KEYS) ||                      \
    !defined(BLOOM_UPDATE_APPIMAGE_INFORMATION) || !defined(BLOOM_UPDATE_TEST_BUILD)
#error "the update settings must be defined for this file, empty when a build has none"
#endif

namespace bloom::update {

const UpdateConfiguration& builtInConfiguration() {
    static const UpdateConfiguration configuration =
        makeConfiguration(BLOOM_UPDATE_FEED_URL, BLOOM_UPDATE_PUBLIC_KEYS,
                          BLOOM_UPDATE_APPIMAGE_INFORMATION, BLOOM_UPDATE_TEST_BUILD != 0);
    return configuration;
}

} // namespace bloom::update

#include <bloom/update/configuration.hpp>

#include <bloom/update/feed.hpp>
#include <bloom/update/keys.hpp>

#include <utility>

namespace bloom::update {

UpdateConfiguration makeConfiguration(const std::string_view feedUrl,
                                      const std::string_view publicKeys,
                                      const std::string_view appImageInformation,
                                      const bool testBuild) {
    UpdateConfiguration configuration;
    configuration.testBuild = testBuild;
    configuration.appImageInformation = std::string(appImageInformation);
    auto keys = parsePublicKeys(publicKeys);
    if (feedUrl.empty() || !keys.has_value() || keys->empty() ||
        !updateAddressAllowed(feedUrl, testBuild)) {
        return configuration;
    }
    configuration.feedUrl = std::string(feedUrl);
    configuration.keys = std::move(*keys);
    return configuration;
}

UpdateConfiguration effectiveConfiguration(const UpdateConfiguration& builtIn,
                                           const EnvironmentReader& environment) {
    if (!builtIn.testBuild || !environment) {
        return builtIn;
    }
    const auto feedUrl = environment(kTestFeedUrlVariable);
    const auto publicKeys = environment(kTestPublicKeysVariable);
    if (!feedUrl.has_value() || !publicKeys.has_value()) {
        return builtIn;
    }
    return makeConfiguration(*feedUrl, *publicKeys, builtIn.appImageInformation, true);
}

} // namespace bloom::update

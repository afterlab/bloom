#pragma once

#include <bloom/update/ed25519.hpp>
#include <bloom/update/install_kind.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace bloom::update {

// Where this build looks for updates and whose signature it accepts. All of it is fixed when the
// program is built (the settings BLOOM_UPDATE_FEED_URL, BLOOM_UPDATE_PUBLIC_KEYS and
// BLOOM_UPDATE_APPIMAGE_INFORMATION); the source tree carries no address and no key, and a build
// without them never contacts anything.
struct UpdateConfiguration final {
    std::string feedUrl;
    std::vector<Ed25519PublicKey> keys;
    // The update information an AppImage of this build carries (its own delta update).
    std::string appImageInformation;
    // A test build (BLOOM_UPDATE_TEST_BUILD) may be pointed at a feed on this machine and at a
    // test key through two environment variables. It is never published: the package check
    // refuses it for a release.
    bool testBuild = false;

    [[nodiscard]] bool configured() const noexcept { return !feedUrl.empty() && !keys.empty(); }
};

inline constexpr std::string_view kTestFeedUrlVariable = "BLOOM_UPDATE_TEST_FEED_URL";
inline constexpr std::string_view kTestPublicKeysVariable = "BLOOM_UPDATE_TEST_PUBLIC_KEYS";

// What was built in. An address that is not HTTPS, or a key list with an entry that is not a
// key, leaves the build not configured.
[[nodiscard]] const UpdateConfiguration& builtInConfiguration();

// What a given build setting gives; `builtInConfiguration` is this over the built-in texts.
[[nodiscard]] UpdateConfiguration makeConfiguration(std::string_view feedUrl,
                                                    std::string_view publicKeys,
                                                    std::string_view appImageInformation,
                                                    bool testBuild);

// The configuration a run uses: the built-in one, and for a test build only, the two variables
// above in its place (an address on this machine over HTTP is then allowed). A release build
// reads no variable.
[[nodiscard]] UpdateConfiguration effectiveConfiguration(const UpdateConfiguration& builtIn,
                                                         const EnvironmentReader& environment);

// What `Bloom --update-settings` prints, for the package check of a release: four lines,
//     feed <address>
//     keys <Base64 key>[,<Base64 key>...]
//     appimage <update information>
//     test <yes|no>
// with `-` for a value the build does not have.
[[nodiscard]] std::string describeConfiguration(const UpdateConfiguration& configuration);

} // namespace bloom::update

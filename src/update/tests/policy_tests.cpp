// The automatic check's daily rule with an injected clock, the order of versions, and what a
// verified feed means for each kind of install.
#include "update_test_support.hpp"

#include <bloom/update/check_policy.hpp>
#include <bloom/update/configuration.hpp>
#include <bloom/update/keys.hpp>

#include <functional>
#include <map>
#include <vector>

int main() {
    using namespace bloom::update;
    using namespace bloom::update::testing;
    Expectations checks;

    // At most once a day, on by default.
    constexpr std::int64_t day = kAutomaticCheckIntervalSeconds;
    constexpr std::int64_t now = 1'800'000'000;
    checks.expect(AutomaticCheckState{}.enabled, "policy: the automatic check is on by default");
    checks.expect(automaticCheckDue({true, std::nullopt}, now),
                  "policy: the first run checks");
    checks.expect(!automaticCheckDue({true, now}, now) &&
                      !automaticCheckDue({true, now - 1}, now) &&
                      !automaticCheckDue({true, now - day + 1}, now),
                  "policy: no second check within a day");
    checks.expect(automaticCheckDue({true, now - day}, now) &&
                      automaticCheckDue({true, now - 30 * day}, now),
                  "policy: a check is due a day after the last");
    checks.expect(!automaticCheckDue({false, std::nullopt}, now) &&
                      !automaticCheckDue({false, now - 30 * day}, now),
                  "policy: turned off, it never checks");
    checks.expect(automaticCheckDue({true, now + 5 * day}, now),
                  "policy: a last check in the future (the clock was set back) does not stop "
                  "the checks");

    // Versions.
    const auto version = [](const std::string_view text) { return ReleaseVersion::parse(text); };
    const std::vector<std::string_view> ascending{
        "0.1.0-alpha.1", "0.1.0-alpha.2", "0.1.0-alpha.10", "0.1.0-alpha.beta", "0.1.0-beta.1",
        "0.1.0-rc.1",    "0.1.0",         "0.1.1-alpha.1",  "0.1.1",            "0.2.0",
        "1.0.0-0",       "1.0.0-alpha",   "1.0.0-alpha.1",  "1.0.0",            "10.0.0"};
    bool ordered = true;
    for (std::size_t index = 0; index + 1 < ascending.size(); ++index) {
        const auto lower = version(ascending[index]);
        const auto higher = version(ascending[index + 1]);
        ordered = ordered && lower.has_value() && higher.has_value() && *lower < *higher &&
                  *higher > *lower && !(*lower == *higher) && lower->text() == ascending[index];
    }
    checks.expect(ordered, "version: pre-releases come before their release, numbers by value");
    checks.expect(version("0.1.0+build.5") == version("0.1.0") &&
                      version("0.1.0-alpha.1+x") == version("0.1.0-alpha.1"),
                  "version: build metadata takes no part in the order");
    for (const std::string_view text :
         {"", "1", "1.0", "1.0.0.0", "01.0.0", "1.0.0-", "1.0.0-alpha..1", "1.0.0-alpha.01",
          "1.0.0-al_pha", "v1.0.0", "1.0.x", "1.0.0+", "4294967296.0.0", " 1.0.0", "1.0.0 "}) {
        checks.expect(!version(text).has_value(),
                      "version: '" + std::string(text) + "' is not a version");
    }
    checks.expect(version("0.1.0-alpha.1")->channel() == "alpha" &&
                      version("0.1.0-Beta.2")->channel() == "beta" &&
                      version("0.1.0")->channel() == "stable" &&
                      version("1.0.0-0")->channel() == "stable",
                  "version: the channel is the first pre-release word, or stable");

    // What a feed means for an install.
    const auto trusted = parsePublicKeys(trimmedFixture("trusted-key.txt"));
    const auto read = trusted.has_value()
                          ? readSignedFeed(asBytes(readFixture("feed.json")),
                                           {*trusted, "alpha", false})
                          : FeedReadResult{};
    checks.expect(read.feed.has_value(), "the fixture feed is read");
    if (!read.feed.has_value()) {
        return 1;
    }
    const auto& feed = *read.feed;
    const auto decide = [&](const InstallKind kind, const std::string_view platform,
                            const ReleaseVersion& current = *ReleaseVersion::parse(
                                "0.1.0-alpha.1")) {
        return decideUpdate(feed, current, kind, platform);
    };
    const auto folder = decide(InstallKind::UserFolder, "linux-x86_64");
    checks.expect(folder.action == UpdateAction::ReplaceFolder && folder.file.has_value() &&
                      folder.file->kind == "tar",
                  "decision: an install in the user's folder downloads the tar");
    const auto appImage = decide(InstallKind::AppImage, "linux-x86_64");
    checks.expect(appImage.action == UpdateAction::ReplaceAppImage && appImage.file.has_value() &&
                      appImage.file->kind == "appimage",
                  "decision: an AppImage downloads the AppImage");
    const auto managed = decide(InstallKind::PackageManager, "linux-x86_64");
    checks.expect(managed.action == UpdateAction::LeaveToPackageManager && !managed.file.has_value(),
                  "decision: an install owned by a package manager is left to it");
    checks.expect(decide(InstallKind::ReadOnlyFolder, "linux-x86_64").action ==
                      UpdateAction::RunInstallerAgain,
                  "decision: a folder this user cannot write needs its installer again");
    const auto mac = decide(InstallKind::MacBundle, "macos-arm64");
    checks.expect(mac.action == UpdateAction::PlatformUpdater && mac.appcastUrl.ends_with("/appcast-macos.xml"),
                  "decision: macOS hands its list to the platform's updater");
    const auto windows = decide(InstallKind::WindowsInstaller, "windows-x86_64");
    checks.expect(windows.action == UpdateAction::PlatformUpdater && windows.appcastUrl.ends_with("/appcast-windows.xml"),
                  "decision: an installed Windows copy hands its list to the platform's updater");
    const auto portable = decide(InstallKind::WindowsPortable, "windows-x86_64");
    checks.expect(portable.action == UpdateAction::DownloadManually && portable.file.has_value() &&
                      portable.file->kind == "zip",
                  "decision: an unpacked Windows zip is told where the next one is");
    checks.expect(decide(InstallKind::Development, "linux-x86_64").action ==
                      UpdateAction::NotInstalled,
                  "decision: a build folder is never updated");
    checks.expect(decide(InstallKind::UserFolder, "linux-arm64").action ==
                      UpdateAction::NotPublished,
                  "decision: a platform the feed does not list has no update");
    for (const std::string_view current : {"0.2.0-alpha.3", "0.2.0-alpha.4", "0.2.0", "1.0.0"}) {
        checks.expect(decide(InstallKind::UserFolder, "linux-x86_64", *version(current)).action ==
                          UpdateAction::UpToDate,
                      "decision: " + std::string(current) + " is not offered the older feed");
    }

    // The build's settings.
    const auto key = trimmedFixture("trusted-key.txt");
    const auto release = makeConfiguration("https://updates.example.org/alpha.json", key, "", false);
    checks.expect(release.configured() && release.keys.size() == 1 && !release.testBuild,
                  "configuration: an address and a key configure a build");
    checks.expect(!makeConfiguration("", "", "", false).configured() &&
                      !makeConfiguration("https://updates.example.org/alpha.json", "", "", false)
                           .configured() &&
                      !makeConfiguration("", key, "", false).configured() &&
                      !makeConfiguration("https://updates.example.org/alpha.json", key + ",x", "",
                                         false)
                           .configured() &&
                      !makeConfiguration("http://updates.example.org/alpha.json", key, "", false)
                           .configured() &&
                      !makeConfiguration("http://127.0.0.1:8000/alpha.json", key, "", false)
                           .configured(),
                  "configuration: without an HTTPS address and valid keys a build is not "
                  "configured");
    const std::map<std::string, std::string, std::less<>> variables{
        {std::string(kTestFeedUrlVariable), "http://127.0.0.1:8000/alpha.json"},
        {std::string(kTestPublicKeysVariable), trimmedFixture("other-key.txt")}};
    const EnvironmentReader environment =
        [&variables](const std::string_view name) -> std::optional<std::string> {
        const auto found = variables.find(name);
        return found == variables.end() ? std::nullopt : std::optional(found->second);
    };
    const auto unchanged = effectiveConfiguration(release, environment);
    checks.expect(unchanged.feedUrl == release.feedUrl && unchanged.keys == release.keys,
                  "configuration: a release build reads no variable");
    const auto testBuild = makeConfiguration("", "", "", true);
    const auto pointed = effectiveConfiguration(testBuild, environment);
    checks.expect(!testBuild.configured() && pointed.configured() && pointed.testBuild &&
                      pointed.feedUrl == "http://127.0.0.1:8000/alpha.json",
                  "configuration: a test build is pointed at a feed on this machine");
    checks.expect(!effectiveConfiguration(testBuild, [](std::string_view) {
                       return std::optional<std::string>();
                   }).configured(),
                  "configuration: a test build without the variables is not configured");
    checks.expect(!builtInConfiguration().configured() ||
                      builtInConfiguration().feedUrl.starts_with("http"),
                  "configuration: the built-in settings are read");
    return checks.result();
}

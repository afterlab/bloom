// The signed update feed: what a release publishes is read, and everything else is refused with
// its reason. The fixtures are made by the release tools (tools/release/make_update_fixtures.py).
#include "update_test_support.hpp"

#include <bloom/update/base64.hpp>
#include <bloom/update/feed.hpp>
#include <bloom/update/keys.hpp>

#include <algorithm>
#include <array>

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;

// The envelope of a fixture with its payload replaced by `change(payload)`, signatures kept.
[[nodiscard]] std::string withPayload(const std::string& envelope,
                                      const std::string_view from, const std::string_view to) {
    constexpr std::string_view opening = "\"payload\": \"";
    const auto start = envelope.find(opening) + opening.size();
    const auto end = envelope.find('"', start);
    const auto payload = decodeBase64(std::string_view(envelope).substr(start, end - start));
    std::string text(payload->begin(), payload->end());
    const auto position = text.find(from);
    if (position == std::string::npos) {
        return {};
    }
    text.replace(position, from.size(), to);
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return envelope.substr(0, start) + toBase64(bytes) + envelope.substr(end);
}

} // namespace

int main() {
    Expectations checks;
    const auto trusted = parsePublicKeys(trimmedFixture("trusted-key.txt"));
    const auto both =
        parsePublicKeys(trimmedFixture("other-key.txt") + "," + trimmedFixture("trusted-key.txt"));
    checks.expect(trusted.has_value() && both.has_value(), "the fixture keys are read");
    if (!trusted.has_value() || !both.has_value()) {
        return 1;
    }
    const FeedReadOptions alpha{*trusted, "alpha", false};
    const auto read = [&](const std::string_view name, const FeedReadOptions& options) {
        return readSignedFeed(asBytes(readFixture(name)), options);
    };
    const auto refusal = [&](const std::string_view name, const FeedReadOptions& options) {
        const auto result = read(name, options);
        return result.feed.has_value() ? UpdateRefusal::None : result.diagnostic.refusal;
    };

    // What a release publishes.
    const auto valid = read("feed.json", alpha);
    checks.expect(valid.feed.has_value() && valid.diagnostic.accepted(),
                  "feed: the published feed is read: " + valid.diagnostic.detail);
    if (valid.feed.has_value()) {
        const auto& feed = *valid.feed;
        checks.expect(feed.channel == "alpha" && feed.version.text() == "0.2.0-alpha.3" &&
                          feed.published == "2026-11-30",
                      "feed: it lists the version, the channel and the date");
        checks.expect(feed.notes.find("caf\xc3\xa9") != std::string::npos,
                      "feed: the notes are the release's text, as UTF-8");
        checks.expect(feed.platforms.size() == 3, "feed: three platforms");
        const auto* linuxFiles = feed.platform("linux-x86_64");
        const auto* mac = feed.platform("macos-arm64");
        const auto* windows = feed.platform("windows-x86_64");
        checks.expect(linuxFiles != nullptr && mac != nullptr && windows != nullptr &&
                          feed.platform("haiku-x86_64") == nullptr,
                      "feed: each platform is found by its key");
        if (linuxFiles != nullptr && mac != nullptr && windows != nullptr) {
            const auto* tar = linuxFiles->file("tar");
            checks.expect(tar != nullptr && linuxFiles->file("appimage") != nullptr &&
                              linuxFiles->file("zsync") != nullptr && linuxFiles->file("dmg") == nullptr &&
                              linuxFiles->appcastUrl.empty(),
                          "feed: the Linux files are found by kind");
            if (tar != nullptr) {
                const auto hex = tar->sha256.toLowercaseHex();
                checks.expect(
                    tar->size == 9001 &&
                        tar->url == "https://github.com/afterlab/bloom/releases/download/"
                                    "v0.2.0-alpha.3/Bloom-0.2.0-alpha.3-linux-x86_64.tar.xz" &&
                        std::string_view(hex.data(), hex.size()) ==
                            trimmedFixture("update.bin.sha256") &&
                        toBase64(tar->signature) == trimmedFixture("update.bin.sig"),
                    "feed: a file carries its address, size, SHA-256 and signature");
            }
            checks.expect(mac->appcastUrl.ends_with("/appcast-macos.xml") &&
                              mac->files.size() == 3 &&
                              windows->appcastUrl.ends_with("/appcast-windows.xml") &&
                              windows->file("installer") != nullptr,
                          "feed: macOS and Windows carry their updater's address");
        }
    }

    // Signatures.
    checks.expect(refusal("feed-other-key.json", alpha) == UpdateRefusal::FeedUnknownKey,
                  "feed: a feed signed by a key this build does not carry is refused");
    checks.expect(read("feed-two-keys.json", alpha).feed.has_value(),
                  "feed: one signature by a carried key among others is enough");
    checks.expect(read("feed-other-key.json", {*both, "alpha", false}).feed.has_value(),
                  "feed: a build that carries two keys accepts either");
    checks.expect(refusal("feed.json", {{}, "alpha", false}) == UpdateRefusal::NotConfigured,
                  "feed: a build without a key reads nothing");

    const auto published = readFixture("feed.json");
    const auto tampered = withPayload(published, "0.2.0-alpha.3", "0.2.0-alpha.9");
    checks.expect(!tampered.empty() &&
                      readSignedFeed(asBytes(tampered), alpha).diagnostic.refusal ==
                          UpdateRefusal::FeedSignatureInvalid,
                  "tampered_feed_refused: a changed version breaks the signature");
    const auto tamperedAddress = withPayload(published, "github.com/afterlab", "github.com/other");
    checks.expect(!tamperedAddress.empty() &&
                      readSignedFeed(asBytes(tamperedAddress), alpha).diagnostic.refusal ==
                          UpdateRefusal::FeedSignatureInvalid,
                  "tampered_feed_refused: a changed address breaks the signature");
    auto otherType = published;
    const std::string_view typeText = "application/vnd.bloom.update-feed+json";
    otherType.replace(otherType.find(typeText), typeText.size(),
                      "application/vnd.bloom.update-feed+text");
    checks.expect(readSignedFeed(asBytes(otherType), alpha).diagnostic.refusal ==
                      UpdateRefusal::FeedMalformed,
                  "feed: another payload type is not a feed");
    auto unsignedFeed = published;
    const auto signatures = unsignedFeed.find("\"signatures\": [");
    unsignedFeed = unsignedFeed.substr(0, signatures) + "\"signatures\": []\n}\n";
    checks.expect(readSignedFeed(asBytes(unsignedFeed), alpha).diagnostic.refusal ==
                      UpdateRefusal::FeedUnsigned,
                  "feed: an envelope without a signature is refused");
    auto wrongKeyName = readFixture("feed-other-key.json");
    const auto otherId = publicKeyId(both->front());
    wrongKeyName.replace(wrongKeyName.find(otherId), otherId.size(),
                         publicKeyId(trusted->front()));
    checks.expect(readSignedFeed(asBytes(wrongKeyName), alpha).diagnostic.refusal ==
                      UpdateRefusal::FeedSignatureInvalid,
                  "feed: another key's signature under the carried key's name is refused");

    // Shape.
    for (const std::string_view text :
         {"", "not json", "[]", "{}", "{\"payloadType\":1,\"payload\":\"\",\"signatures\":[]}",
          "{\"payloadType\":\"application/vnd.bloom.update-feed+json\",\"payload\":\"*\","
          "\"signatures\":[]}"}) {
        checks.expect(readSignedFeed(asBytes(text), alpha).diagnostic.refusal ==
                          UpdateRefusal::FeedMalformed,
                      "feed: '" + std::string(text.substr(0, 20)) + "' is not an envelope");
    }
    const std::string large(kMaximumFeedBytes + 1, ' ');
    checks.expect(readSignedFeed(asBytes(large), alpha).diagnostic.refusal ==
                      UpdateRefusal::FeedTooLarge,
                  "feed: an answer larger than a feed is not read");

    // Signed content that is still not accepted.
    checks.expect(refusal("feed-beta.json", alpha) == UpdateRefusal::FeedChannelMismatch,
                  "feed: a feed of another channel is refused");
    checks.expect(read("feed-beta.json", {*trusted, "beta", false}).feed.has_value(),
                  "feed: the same feed is read by a build of its channel");
    checks.expect(refusal("feed-no-version.json", alpha) == UpdateRefusal::FeedContentMalformed,
                  "feed: a feed without a version is refused");
    checks.expect(refusal("feed-version-of-other-channel.json", alpha) ==
                      UpdateRefusal::FeedContentMalformed,
                  "feed: a version that is not of the feed's channel is refused");
    checks.expect(refusal("feed-other-schema.json", alpha) == UpdateRefusal::FeedContentMalformed,
                  "feed: a feed of an unknown form is refused");
    checks.expect(refusal("feed-plain-http.json", alpha) == UpdateRefusal::FeedAddressRefused,
                  "feed: an address that is not HTTPS is refused");
    checks.expect(refusal("feed-loopback.json", alpha) == UpdateRefusal::FeedAddressRefused,
                  "feed: an address on this machine is refused by a release build");
    checks.expect(read("feed-loopback.json", {*trusted, "alpha", true}).feed.has_value(),
                  "feed: an address on this machine is read by a test build");
    checks.expect(refusal("feed-plain-http.json", {*trusted, "alpha", true}) ==
                      UpdateRefusal::FeedAddressRefused,
                  "feed: a test build still refuses plain HTTP to another machine");
    const auto later = read("feed-unknown-fields.json", alpha);
    const auto* laterLinux = later.feed.has_value() ? later.feed->platform("linux-x86_64") : nullptr;
    checks.expect(laterLinux != nullptr && laterLinux->files.size() == 4 &&
                      laterLinux->file("later-kind") != nullptr,
                  "feed: fields and file kinds a later version adds do not break the reader");

    // Addresses.
    checks.expect(updateAddressAllowed("https://example.org/a", false) &&
                      !updateAddressAllowed("https:///a", false) &&
                      !updateAddressAllowed("https://user@example.org/a", false) &&
                      !updateAddressAllowed("https://example.org/a b", false) &&
                      !updateAddressAllowed("http://example.org/a", true) &&
                      !updateAddressAllowed("http://127.0.0.1.example.org/a", true) &&
                      updateAddressAllowed("http://127.0.0.1:8000/a", true) &&
                      updateAddressAllowed("http://[::1]:8000/a", true) &&
                      !updateAddressAllowed("http://127.0.0.1:8000/a", false) &&
                      !updateAddressAllowed("file:///etc/passwd", true) &&
                      !updateAddressAllowed("ftp://example.org/a", true),
                  "feed: only HTTPS addresses are fetched, and this machine's in a test build");

    // The signed bytes are DSSE's.
    const std::array<std::uint8_t, 5> hello{'h', 'e', 'l', 'l', 'o'};
    const auto encoded = feedSignedBytes("http://example.com/HelloWorld", hello);
    const std::string_view expected = "DSSEv1 29 http://example.com/HelloWorld 5 hello";
    checks.expect(std::ranges::equal(encoded, asBytes(expected)),
                  "feed: the signed bytes are the envelope's pre-authentication encoding");
    return checks.result();
}

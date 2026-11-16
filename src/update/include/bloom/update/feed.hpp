#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/update/ed25519.hpp>
#include <bloom/update/refusal.hpp>
#include <bloom/update/release_version.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The update feed (ADR 0041, schemas/update/feed-1.schema.json): one small signed file a release
// publishes per channel. It names the newest version of the channel and, for each platform, the
// files of that release with their size, SHA-256 and Ed25519 signature.
//
// The file is a DSSE envelope (the Dead Simple Signing Envelope: a payload type, the payload in
// Base64, and signatures over both), so the signed bytes are exactly the bytes that are read:
// nothing is parsed before the signature has been checked against a key this build carries.
namespace bloom::update {

inline constexpr std::string_view kFeedPayloadType = "application/vnd.bloom.update-feed+json";
inline constexpr std::string_view kFeedSchema = "bloom.update-feed/1";
// A feed is a few kilobytes plus the release notes. A larger answer is not read.
inline constexpr std::size_t kMaximumFeedBytes = 1024 * 1024;

// One published file of a release.
struct FeedFile final {
    // What the file is: `tar`, `appimage`, `zsync` (Linux); `zip`, `dmg`, `pkg` (macOS);
    // `installer`, `zip` (Windows). A reader ignores kinds it does not know.
    std::string kind;
    std::string url;
    std::uint64_t size = 0;
    core::Sha256Digest sha256;
    // Plain Ed25519 over the file's bytes, the form the platform updaters check as well.
    Ed25519Signature signature{};
};

struct FeedPlatform final {
    // The address of the list the platform's own updater reads (macOS, Windows); empty where
    // Bloom installs the update itself.
    std::string appcastUrl;
    std::vector<FeedFile> files;

    [[nodiscard]] const FeedFile* file(std::string_view kind) const noexcept;
};

struct UpdateFeed final {
    std::string channel;
    ReleaseVersion version;
    // The day of the release, as YYYY-MM-DD.
    std::string published;
    // The release notes: the version's section of the changelog, as Markdown.
    std::string notes;
    // By platform key: `linux-x86_64`, `macos-arm64`, `windows-x86_64`.
    std::map<std::string, FeedPlatform, std::less<>> platforms;

    [[nodiscard]] const FeedPlatform* platform(std::string_view key) const noexcept;
};

struct FeedReadOptions final {
    std::span<const Ed25519PublicKey> trustedKeys;
    // The channel this build follows; a feed for another is refused.
    std::string_view channel;
    // A test build may be given addresses on this machine over plain HTTP. Never set otherwise.
    bool allowLoopbackHttp = false;
};

struct FeedReadResult final {
    std::optional<UpdateFeed> feed;
    UpdateDiagnostic diagnostic;
};

// Reads a published feed. The order is fixed: size, envelope shape, signature, and only then the
// content.
[[nodiscard]] FeedReadResult readSignedFeed(std::span<const std::byte> bytes,
                                            const FeedReadOptions& options);

// The bytes a feed signature covers (DSSE's pre-authentication encoding of the payload type and
// the payload).
[[nodiscard]] std::vector<std::byte> feedSignedBytes(std::string_view payloadType,
                                                     std::span<const std::uint8_t> payload);

// Whether an address may be fetched: HTTPS, or (only when allowed) plain HTTP on this machine.
[[nodiscard]] bool updateAddressAllowed(std::string_view url, bool allowLoopbackHttp) noexcept;

// The key of the platform this program was built for, or empty where no release is published.
[[nodiscard]] std::string_view currentPlatformKey() noexcept;

} // namespace bloom::update

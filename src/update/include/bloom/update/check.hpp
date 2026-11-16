#pragma once

#include <bloom/update/check_policy.hpp>
#include <bloom/update/configuration.hpp>
#include <bloom/update/feed.hpp>
#include <bloom/update/file_verification.hpp>
#include <bloom/update/install_kind.hpp>
#include <bloom/update/refusal.hpp>
#include <bloom/update/transport.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

// The two network steps of an update, Qt-free: they block, so the application runs each as a
// background job with a transport of its own.
namespace bloom::update {

enum class CheckStatus : std::uint8_t {
    // This build carries no update address or key. Nothing was contacted.
    NotConfigured,
    Cancelled,
    // The feed could not be fetched.
    Unreachable,
    // The feed was fetched and refused.
    Refused,
    // The feed was verified; `decision` says what it means for this copy.
    Decided,
};

struct CheckResult final {
    CheckStatus status = CheckStatus::NotConfigured;
    // Why the feed was refused (Refused), with its sentence.
    UpdateDiagnostic diagnostic;
    // What the transport said (Unreachable).
    TransportResult transport;
    std::optional<UpdateFeed> feed;
    UpdateDecision decision;
};

// Fetches the feed of this build's channel with one GET, verifies it, and decides.
[[nodiscard]] CheckResult checkForUpdate(const UpdateConfiguration& configuration,
                                         UpdateTransport& transport,
                                         const ReleaseVersion& current, InstallKind install,
                                         std::string_view platformKey,
                                         const std::function<bool()>& cancelled);

struct DownloadResult final {
    // Accepted only when the file is at `file`, whole and verified.
    UpdateDiagnostic diagnostic;
    TransportResult transport;
    std::filesystem::path file;

    [[nodiscard]] bool succeeded() const noexcept {
        return diagnostic.accepted() && transport.status == TransportStatus::Ok;
    }
};

// Downloads a file of a verified feed to `destination`. The bytes are checked as they arrive
// (size, SHA-256, Ed25519 signature) and are kept under another name until all three hold; a
// file that fails, or a cancelled transfer, leaves nothing behind.
[[nodiscard]] DownloadResult downloadUpdateFile(UpdateTransport& transport, const FeedFile& entry,
                                                std::span<const Ed25519PublicKey> trustedKeys,
                                                const std::filesystem::path& destination,
                                                const std::function<bool()>& cancelled,
                                                const TransportProgress& progress);

} // namespace bloom::update

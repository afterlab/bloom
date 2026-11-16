#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/update/ed25519.hpp>
#include <bloom/update/feed.hpp>
#include <bloom/update/refusal.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace bloom::update {

// Checks a downloaded file against its entry in a verified feed: the size, the SHA-256, and the
// Ed25519 signature by one of the keys this build carries. All three must hold; the file is read
// once. A file that fails is never handed to anything that installs it.
class UpdateFileVerifier final {
  public:
    UpdateFileVerifier(const FeedFile& expected, std::span<const Ed25519PublicKey> trustedKeys);

    void update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] UpdateDiagnostic finish() const;

  private:
    FeedFile expected_;
    std::uint64_t size_ = 0;
    core::Sha256Hasher sha256_;
    bool sha256Failed_ = false;
    std::vector<Ed25519Verifier> signatures_;
};

// The same check over a file on disk, read in pieces. `cancelled` is asked between pieces.
[[nodiscard]] UpdateDiagnostic verifyUpdateFile(const std::filesystem::path& path,
                                                const FeedFile& expected,
                                                std::span<const Ed25519PublicKey> trustedKeys,
                                                const std::function<bool()>& cancelled = {});

} // namespace bloom::update

#pragma once

#include <bloom/update/sha512.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bloom::update {

inline constexpr std::size_t kEd25519PublicKeyBytes = 32;
inline constexpr std::size_t kEd25519SignatureBytes = 64;

using Ed25519PublicKey = std::array<std::uint8_t, kEd25519PublicKeyBytes>;
using Ed25519Signature = std::array<std::uint8_t, kEd25519SignatureBytes>;

// Ed25519 signature verification (RFC 8032, the plain form over the whole message), and nothing
// else: Bloom holds public keys only and never signs. The same check is made by the tool that
// signs a release (`openssl pkeyutl -verify -rawin`) and by the platform updaters, so a file one
// of them accepts is accepted by all.
//
// The message is given in pieces, so a download is verified while it is read. `finish` answers
// true only for a signature this key made over exactly these bytes; a malformed key or signature
// is simply not valid. Verification reads public values only, so nothing here is secret and
// nothing needs constant time.
class Ed25519Verifier final {
  public:
    Ed25519Verifier(const Ed25519PublicKey& publicKey, const Ed25519Signature& signature) noexcept;

    void update(std::span<const std::byte> message) noexcept;
    [[nodiscard]] bool finish() const noexcept;

  private:
    Ed25519PublicKey publicKey_;
    Ed25519Signature signature_;
    Sha512 hash_;
};

[[nodiscard]] bool verifyEd25519(const Ed25519PublicKey& publicKey,
                                 std::span<const std::byte> message,
                                 const Ed25519Signature& signature) noexcept;

} // namespace bloom::update

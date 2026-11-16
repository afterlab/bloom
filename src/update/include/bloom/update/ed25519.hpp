#pragma once

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
// else: Bloom holds public keys only and never signs. The check is OpenSSL's (the locked
// libcrypto, `EVP_DigestVerify` with a raw Ed25519 public key), the same library the tool that
// signs a release uses, so a file one of them accepts is accepted by the other.
//
// True only for a signature this key made over exactly these bytes. A malformed key or
// signature is simply not valid, and so is everything when the library cannot be used
// (`cryptoLibraryReady`). The plain form is defined over the whole message at once, so the
// caller gives all of it; a large file is given as a mapping (file_verification.hpp).
[[nodiscard]] bool verifyEd25519(const Ed25519PublicKey& publicKey,
                                 std::span<const std::byte> message,
                                 const Ed25519Signature& signature) noexcept;

// Whether the library behind the checks can be used. Always true where it is linked into the
// program (Linux, macOS). On Windows it is a library file of the carried Python interpreter,
// loaded on first use from the package (or, for a program run from a build folder, from the
// dependency prefix); false when that file is missing.
[[nodiscard]] bool cryptoLibraryReady() noexcept;

} // namespace bloom::update

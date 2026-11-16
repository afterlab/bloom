#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bloom::update {

inline constexpr std::size_t kSha512DigestBytes = 64;

// SHA-512 (FIPS 180-4), streaming. Ed25519 is defined over it, and the update check is the only
// user: a file is hashed while it is read, never held in memory.
class Sha512 final {
  public:
    using Digest = std::array<std::uint8_t, kSha512DigestBytes>;

    void update(std::span<const std::byte> bytes) noexcept;
    // The digest of everything given so far. The hasher itself is left as it was.
    [[nodiscard]] Digest finalize() const noexcept;

    [[nodiscard]] static Digest hash(std::span<const std::byte> bytes) noexcept;

  private:
    void compress(const std::byte* block) noexcept;

    std::array<std::uint64_t, 8> state_{
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
        0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    std::array<std::byte, 128> buffered_{};
    std::size_t bufferedCount_ = 0;
    // A message is far below 2^64 bytes, so the upper half of the 128-bit length is its top bits.
    std::uint64_t totalBytes_ = 0;
};

} // namespace bloom::update

#pragma once

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace bloom::update {

// SHA-256 of an update's bytes, computed by OpenSSL (the locked libcrypto, `EVP_sha256`), in
// pieces, so a file is hashed while it is read. The digest is carried in Bloom's own value type.
// Every answer is empty when the library cannot be used (`cryptoLibraryReady`) or reports an
// error; an empty answer never equals a digest.
class Sha256 final {
  public:
    Sha256() noexcept;
    ~Sha256();
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(std::span<const std::byte> bytes) noexcept;
    // The digest of everything given so far. The hasher is left as it was.
    [[nodiscard]] std::optional<core::Sha256Digest> finalize() const noexcept;

    [[nodiscard]] static std::optional<core::Sha256Digest>
    hash(std::span<const std::byte> bytes) noexcept;

  private:
    struct Context;
    struct ContextDeleter final {
        void operator()(Context* context) const noexcept;
    };
    std::unique_ptr<Context, ContextDeleter> context_;
    bool failed_ = false;
};

} // namespace bloom::update

#include <bloom/update/sha256.hpp>

#include <bloom/update/ed25519.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>

#include <new>

namespace bloom::update {

// The context is OpenSSL's own type, held under a name of this module so that the header needs
// no header of the library.
struct Sha256::Context final {
    EVP_MD_CTX* evp = nullptr;
};

void Sha256::ContextDeleter::operator()(Context* const context) const noexcept {
    if (context != nullptr) {
        EVP_MD_CTX_free(context->evp);
        delete context;
    }
}

Sha256::Sha256() noexcept {
    if (!cryptoLibraryReady()) {
        failed_ = true;
        return;
    }
    context_.reset(new (std::nothrow) Context);
    if (context_ != nullptr) {
        context_->evp = EVP_MD_CTX_new();
    }
    failed_ = context_ == nullptr || context_->evp == nullptr ||
              EVP_DigestInit_ex(context_->evp, EVP_sha256(), nullptr) != 1;
}

Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::update(const std::span<const std::byte> bytes) noexcept {
    if (failed_ || bytes.empty()) {
        return;
    }
    failed_ = EVP_DigestUpdate(context_->evp, bytes.data(), bytes.size()) != 1;
}

std::optional<core::Sha256Digest> Sha256::finalize() const noexcept {
    if (failed_) {
        return std::nullopt;
    }
    // The digest is taken from a copy, so more bytes may follow.
    EVP_MD_CTX* const copy = EVP_MD_CTX_new();
    core::Sha256Digest::Bytes digest{};
    unsigned int length = 0;
    const bool done = copy != nullptr && EVP_MD_CTX_copy_ex(copy, context_->evp) == 1 &&
                      EVP_DigestFinal_ex(copy, digest.data(), &length) == 1 &&
                      length == digest.size();
    EVP_MD_CTX_free(copy);
    if (!done) {
        ERR_clear_error();
        return std::nullopt;
    }
    return core::Sha256Digest::fromBytes(digest);
}

std::optional<core::Sha256Digest> Sha256::hash(const std::span<const std::byte> bytes) noexcept {
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.finalize();
}

} // namespace bloom::update

#include <bloom/update/ed25519.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>

namespace bloom::update {

bool verifyEd25519(const Ed25519PublicKey& publicKey, const std::span<const std::byte> message,
                   const Ed25519Signature& signature) noexcept {
    if (!cryptoLibraryReady()) {
        return false;
    }
    EVP_PKEY* const key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                      publicKey.data(), publicKey.size());
    EVP_MD_CTX* const context = key != nullptr ? EVP_MD_CTX_new() : nullptr;
    bool valid = false;
    // Ed25519 names no digest of its own choosing: the digest argument stays empty.
    if (context != nullptr && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1) {
        static constexpr unsigned char kNothing = 0;
        const auto* const bytes = message.empty()
                                      ? &kNothing
                                      : reinterpret_cast<const unsigned char*>(message.data());
        valid = EVP_DigestVerify(context, signature.data(), signature.size(), bytes,
                                 message.size()) == 1;
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    // A refused signature leaves its reason in the thread's error queue; nothing reads it.
    ERR_clear_error();
    return valid;
}

} // namespace bloom::update

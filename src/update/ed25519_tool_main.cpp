// bloom-ed25519-tool: Ed25519 with the library Bloom verifies updates with (the locked OpenSSL's
// libcrypto), for the release tools on a machine whose `openssl` program is not OpenSSL 3
// (tools/release/feed.py, `signing_route`). A build tool: no package carries it, and a release
// is signed with the `openssl` program.
//
//   bloom-ed25519-tool public <key file>
//       prints the public half of a private key as Base64 (32 bytes).
//   bloom-ed25519-tool sign <key file> <message file> <signature file>
//       writes the signature of the message file's bytes as 64 raw bytes.
//   bloom-ed25519-tool verify <public key in Base64> <message file> <signature file>
//       exit status 0 when the signature is the key's over the file's bytes, 1 when it is not.
//
// The key file is an Ed25519 private key as PKCS#8 without a passphrase, PEM or DER. Exit status
// 2 with one line on the standard error stream for anything that could not be done.
#include "mapped_file.hpp"

#include <bloom/update/base64.hpp>
#include <bloom/update/ed25519.hpp>
#include <bloom/update/keys.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::update::detail::MappedFile;

int fail(const std::string& text) {
    std::cerr << "bloom-ed25519-tool: " << text << '\n';
    return 2;
}

// A key file never has a passphrase: a key that asks for one is refused, never prompted for.
int noPassphrase(char*, int, int, void*) {
    return 0;
}

// The private key of a PKCS#8 file, PEM or DER; null when the file holds no Ed25519 key.
EVP_PKEY* readKey(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return nullptr;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 16 * 1024) {
        return nullptr;
    }
    EVP_PKEY* key = nullptr;
    if (std::string_view(bytes.data(), bytes.size()).starts_with("-----")) {
        BIO* const input = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
        if (input != nullptr) {
            key = PEM_read_bio_PrivateKey(input, nullptr, noPassphrase, nullptr);
            BIO_free(input);
        }
    } else {
        const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.data());
        PKCS8_PRIV_KEY_INFO* const info =
            d2i_PKCS8_PRIV_KEY_INFO(nullptr, &cursor, static_cast<long>(bytes.size()));
        if (info != nullptr) {
            key = EVP_PKCS82PKEY(info);
            PKCS8_PRIV_KEY_INFO_free(info);
        }
    }
    if (key != nullptr && EVP_PKEY_get_base_id(key) != EVP_PKEY_ED25519) {
        EVP_PKEY_free(key);
        key = nullptr;
    }
    ERR_clear_error();
    return key;
}

int printPublic(const std::filesystem::path& keyFile) {
    EVP_PKEY* const key = readKey(keyFile);
    if (key == nullptr) {
        return fail("the key file holds no Ed25519 private key");
    }
    bloom::update::Ed25519PublicKey bytes{};
    std::size_t length = bytes.size();
    const bool read = EVP_PKEY_get_raw_public_key(key, bytes.data(), &length) == 1 &&
                      length == bytes.size();
    EVP_PKEY_free(key);
    if (!read) {
        return fail("the key's public half could not be read");
    }
    std::cout << bloom::update::encodeBase64(bytes) << '\n';
    return 0;
}

int sign(const std::filesystem::path& keyFile, const std::filesystem::path& messageFile,
         const std::filesystem::path& signatureFile) {
    const auto message = MappedFile::open(messageFile);
    if (!message.has_value()) {
        return fail("the message file cannot be read");
    }
    EVP_PKEY* const key = readKey(keyFile);
    if (key == nullptr) {
        return fail("the key file holds no Ed25519 private key");
    }
    bloom::update::Ed25519Signature signature{};
    std::size_t length = signature.size();
    EVP_MD_CTX* const context = EVP_MD_CTX_new();
    static constexpr unsigned char kNothing = 0;
    const auto bytes = message->bytes();
    const bool made =
        context != nullptr && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestSign(context, signature.data(), &length,
                       bytes.empty() ? &kNothing
                                     : reinterpret_cast<const unsigned char*>(bytes.data()),
                       bytes.size()) == 1 &&
        length == signature.size();
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!made) {
        return fail("the message could not be signed");
    }
    std::ofstream out(signatureFile, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(signature.data()),
              static_cast<std::streamsize>(signature.size()));
    out.flush();
    return out ? 0 : fail("the signature file cannot be written");
}

int verify(const std::string& keyText, const std::filesystem::path& messageFile,
           const std::filesystem::path& signatureFile) {
    const auto keys = bloom::update::parsePublicKeys(keyText);
    if (!keys.has_value() || keys->size() != 1) {
        return fail("the public key is not the Base64 of 32 bytes");
    }
    const auto message = MappedFile::open(messageFile);
    const auto signatureBytes = MappedFile::open(signatureFile);
    if (!message.has_value() || !signatureBytes.has_value()) {
        return fail("the message file or the signature file cannot be read");
    }
    bloom::update::Ed25519Signature signature{};
    if (signatureBytes->bytes().size() != signature.size()) {
        return 1;
    }
    for (std::size_t index = 0; index < signature.size(); ++index) {
        signature[index] = static_cast<std::uint8_t>(signatureBytes->bytes()[index]);
    }
    return bloom::update::verifyEd25519(keys->front(), message->bytes(), signature) ? 0 : 1;
}

} // namespace

int main(const int count, const char* const* const values) {
    const std::vector<std::string> arguments(values + (count > 0 ? 1 : 0), values + count);
    if (!bloom::update::cryptoLibraryReady()) {
        return fail("the libcrypto library file was not found");
    }
    try {
        if (arguments.size() == 2 && arguments[0] == "public") {
            return printPublic(std::filesystem::path(arguments[1]));
        }
        if (arguments.size() == 4 && arguments[0] == "sign") {
            return sign(std::filesystem::path(arguments[1]),
                        std::filesystem::path(arguments[2]),
                        std::filesystem::path(arguments[3]));
        }
        if (arguments.size() == 4 && arguments[0] == "verify") {
            return verify(arguments[1], std::filesystem::path(arguments[2]),
                          std::filesystem::path(arguments[3]));
        }
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    return fail("usage: public <key file> | sign <key file> <message file> <signature file> | "
                "verify <public key> <message file> <signature file>");
}

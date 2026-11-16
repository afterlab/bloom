#include <bloom/update/file_verification.hpp>

#include "mapped_file.hpp"

#include <bloom/update/sha256.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace bloom::update {

UpdateDiagnostic verifyUpdateFile(const std::filesystem::path& path, const FeedFile& expected,
                                  const std::span<const Ed25519PublicKey> trustedKeys,
                                  const std::function<bool()>& cancelled) {
    if (trustedKeys.empty()) {
        return {UpdateRefusal::NotConfigured, "this build carries no update key"};
    }
    if (!cryptoLibraryReady()) {
        return {UpdateRefusal::VerificationUnavailable,
                "this copy of Bloom cannot check signatures: a file of its package is missing"};
    }
    std::uint64_t size = 0;
    Sha256 sha256;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return {UpdateRefusal::FileUnreadable, "the downloaded file cannot be opened"};
        }
        std::vector<char> buffer(1024 * 1024);
        while (file) {
            if (cancelled && cancelled()) {
                return {UpdateRefusal::Cancelled, "the check was cancelled"};
            }
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = file.gcount();
            if (count > 0) {
                const auto bytes =
                    std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(count)));
                size += bytes.size();
                sha256.update(bytes);
            }
        }
        if (file.bad() || !file.eof()) {
            return {UpdateRefusal::FileUnreadable, "the downloaded file cannot be read"};
        }
    }
    if (size != expected.size) {
        return {UpdateRefusal::FileSizeMismatch,
                "the file is " + std::to_string(size) + " bytes; the feed gives " +
                    std::to_string(expected.size)};
    }
    const auto digest = sha256.finalize();
    if (!digest.has_value() || *digest != expected.sha256) {
        return {UpdateRefusal::FileChecksumMismatch,
                "the file's SHA-256 is not the one the feed gives"};
    }
    if (cancelled && cancelled()) {
        return {UpdateRefusal::Cancelled, "the check was cancelled"};
    }
    const auto mapped = detail::MappedFile::open(path);
    if (!mapped.has_value() || mapped->bytes().size() != size) {
        return {UpdateRefusal::FileUnreadable, "the downloaded file cannot be read"};
    }
    const bool signedByCarriedKey =
        std::ranges::any_of(trustedKeys, [&](const Ed25519PublicKey& key) {
            return verifyEd25519(key, mapped->bytes(), expected.signature);
        });
    if (!signedByCarriedKey) {
        return {UpdateRefusal::FileSignatureInvalid,
                "the feed's signature for the file does not match it"};
    }
    return {};
}

} // namespace bloom::update

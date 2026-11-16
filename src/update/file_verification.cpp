#include <bloom/update/file_verification.hpp>

#include <algorithm>
#include <array>
#include <fstream>

namespace bloom::update {

UpdateFileVerifier::UpdateFileVerifier(const FeedFile& expected,
                                       const std::span<const Ed25519PublicKey> trustedKeys)
    : expected_(expected) {
    signatures_.reserve(trustedKeys.size());
    for (const auto& key : trustedKeys) {
        signatures_.emplace_back(key, expected_.signature);
    }
}

void UpdateFileVerifier::update(const std::span<const std::byte> bytes) noexcept {
    size_ += bytes.size();
    sha256Failed_ = !sha256_.update(bytes) || sha256Failed_;
    for (auto& signature : signatures_) {
        signature.update(bytes);
    }
}

UpdateDiagnostic UpdateFileVerifier::finish() const {
    if (signatures_.empty()) {
        return {UpdateRefusal::NotConfigured, "this build carries no update key"};
    }
    if (size_ != expected_.size) {
        return {UpdateRefusal::FileSizeMismatch,
                "the file is " + std::to_string(size_) + " bytes; the feed gives " +
                    std::to_string(expected_.size)};
    }
    if (sha256Failed_ || sha256_.finalize() != expected_.sha256) {
        return {UpdateRefusal::FileChecksumMismatch,
                "the file's SHA-256 is not the one the feed gives"};
    }
    const bool signedByCarriedKey = std::ranges::any_of(
        signatures_, [](const Ed25519Verifier& signature) { return signature.finish(); });
    if (!signedByCarriedKey) {
        return {UpdateRefusal::FileSignatureInvalid,
                "the feed's signature for the file does not match it"};
    }
    return {};
}

UpdateDiagnostic verifyUpdateFile(const std::filesystem::path& path, const FeedFile& expected,
                                  const std::span<const Ed25519PublicKey> trustedKeys,
                                  const std::function<bool()>& cancelled) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {UpdateRefusal::FileUnreadable, "the downloaded file cannot be opened"};
    }
    UpdateFileVerifier verifier(expected, trustedKeys);
    std::vector<char> buffer(1024 * 1024);
    while (file) {
        if (cancelled && cancelled()) {
            return {UpdateRefusal::Cancelled, "the check was cancelled"};
        }
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count > 0) {
            verifier.update(
                std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(count))));
        }
    }
    if (file.bad() || !file.eof()) {
        return {UpdateRefusal::FileUnreadable, "the downloaded file cannot be read"};
    }
    return verifier.finish();
}

} // namespace bloom::update

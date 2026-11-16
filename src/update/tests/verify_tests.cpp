// A downloaded file is accepted only when its size, its SHA-256 and its Ed25519 signature are
// the ones the verified feed gives. The three cases issue #734 names are the first three.
#include "update_test_support.hpp"

#include <bloom/update/base64.hpp>
#include <bloom/update/feed.hpp>
#include <bloom/update/file_verification.hpp>
#include <bloom/update/keys.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main() {
    Expectations checks;
    const auto trusted = parsePublicKeys(trimmedFixture("trusted-key.txt"));
    const auto other = parsePublicKeys(trimmedFixture("other-key.txt"));
    const auto feed =
        trusted.has_value()
            ? readSignedFeed(asBytes(readFixture("feed.json")), {*trusted, "alpha", false})
            : FeedReadResult{};
    const auto* platform = feed.feed.has_value() ? feed.feed->platform("linux-x86_64") : nullptr;
    const auto* entry = platform != nullptr ? platform->file("tar") : nullptr;
    checks.expect(entry != nullptr && other.has_value(), "the fixture feed and keys are read");
    if (entry == nullptr || !other.has_value()) {
        return 1;
    }
    const auto released = readFixture("update.bin");
    std::error_code error;
    const auto folder = std::filesystem::temp_directory_path(error) /
                        ("bloom-update-verify-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(folder, error);
    const auto path = folder / "update.bin";
    const auto verdict = [&](const std::string& bytes, const FeedFile& expected,
                             const std::vector<Ed25519PublicKey>& keys) {
        writeFile(path, bytes);
        return verifyUpdateFile(path, expected, keys).refusal;
    };

    checks.expect(verdict(released, *entry, *trusted) == UpdateRefusal::None,
                  "the released file is accepted");

    // tampered_file_refused: one changed byte, the size kept.
    auto tampered = released;
    tampered[4000] = static_cast<char>(tampered[4000] ^ 0x01);
    checks.expect(verdict(tampered, *entry, *trusted) == UpdateRefusal::FileChecksumMismatch,
                  "tampered_file_refused: a changed byte is refused");
    checks.expect(verdict(released + "x", *entry, *trusted) == UpdateRefusal::FileSizeMismatch &&
                      verdict(released.substr(0, released.size() - 1), *entry, *trusted) ==
                          UpdateRefusal::FileSizeMismatch &&
                      verdict({}, *entry, *trusted) == UpdateRefusal::FileSizeMismatch,
                  "tampered_file_refused: a longer, shorter or empty file is refused");

    // wrong_signature_refused: the checksum is the file's, the signature is not this key's.
    auto wrongSignature = *entry;
    const auto otherSignature = decodeBase64(trimmedFixture("update.bin.other-key.sig"));
    checks.expect(otherSignature.has_value() && otherSignature->size() == 64,
                  "the other key's signature is read");
    if (otherSignature.has_value() && otherSignature->size() == 64) {
        std::ranges::copy(*otherSignature, wrongSignature.signature.begin());
        checks.expect(verdict(released, wrongSignature, *trusted) ==
                          UpdateRefusal::FileSignatureInvalid,
                      "wrong_signature_refused: another key's signature is refused");
        checks.expect(verdict(released, wrongSignature, *other) == UpdateRefusal::None,
                      "wrong_signature_refused: the same signature is that key's own");
    }
    auto changedSignature = *entry;
    changedSignature.signature[10] ^= 0x04U;
    checks.expect(verdict(released, changedSignature, *trusted) ==
                      UpdateRefusal::FileSignatureInvalid,
                  "wrong_signature_refused: a changed signature is refused");
    checks.expect(verdict(released, *entry, *other) == UpdateRefusal::FileSignatureInvalid,
                  "wrong_signature_refused: a build with another key refuses the file");
    // A tampered file whose entry was changed to match it still fails the signature.
    auto matchingEntry = *entry;
    matchingEntry.sha256 =
        *bloom::core::Sha256Hasher::hash(asBytes(tampered));
    checks.expect(verdict(tampered, matchingEntry, *trusted) ==
                      UpdateRefusal::FileSignatureInvalid,
                  "wrong_signature_refused: a matching checksum does not excuse the signature");

    // wrong_checksum_refused: the file and its signature are right, the checksum is not.
    auto wrongChecksum = *entry;
    auto digest = std::array<std::uint8_t, 32>{};
    std::ranges::copy(entry->sha256.bytes(), digest.begin());
    digest[0] ^= 0x80U;
    wrongChecksum.sha256 = bloom::core::Sha256Digest::fromBytes(digest);
    checks.expect(verdict(released, wrongChecksum, *trusted) ==
                      UpdateRefusal::FileChecksumMismatch,
                  "wrong_checksum_refused: a checksum that is not the file's is refused");

    // Everything else that is not an accepted file.
    checks.expect(verdict(released, *entry, {}) == UpdateRefusal::NotConfigured,
                  "a build without a key accepts nothing");
    std::vector<Ed25519PublicKey> both = *other;
    both.push_back(trusted->front());
    checks.expect(verdict(released, *entry, both) == UpdateRefusal::None,
                  "a build with two keys accepts a file signed by either");
    checks.expect(verifyUpdateFile(folder / "missing.bin", *entry, *trusted).refusal ==
                      UpdateRefusal::FileUnreadable,
                  "a file that cannot be opened is refused");
    writeFile(path, released);
    checks.expect(verifyUpdateFile(path, *entry, *trusted, [] { return true; }).refusal ==
                      UpdateRefusal::Cancelled,
                  "a cancelled check accepts nothing");

    // The same check while the bytes arrive.
    UpdateFileVerifier pieces(*entry, *trusted);
    for (std::size_t offset = 0; offset < released.size(); offset += 777) {
        pieces.update(asBytes(std::string_view(released).substr(offset, 777)));
    }
    checks.expect(pieces.finish().accepted(), "a file given in pieces is accepted");

    std::filesystem::remove_all(folder, error);
    return checks.result();
}

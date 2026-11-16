// Ed25519 verification and SHA-512 against cases made by OpenSSL (tools/release/make_update_fixtures.py):
// the three test signatures RFC 8032 prints, a longer message, and every way a signature must
// fail.
#include "update_test_support.hpp"

#include <bloom/update/base64.hpp>
#include <bloom/update/ed25519.hpp>
#include <bloom/update/keys.hpp>
#include <bloom/update/sha512.hpp>

#include <algorithm>
#include <sstream>

int main() {
    using namespace bloom::update;
    using namespace bloom::update::testing;
    Expectations checks;

    int digests = 0;
    std::istringstream sha512(readFixture("sha512-cases.txt"));
    for (std::string data, expected; sha512 >> data >> expected;) {
        const auto bytes = fromHex(data);
        const auto digest = fromHex(expected);
        checks.expect(bytes.has_value() && digest.has_value(), "sha512: a case is readable");
        if (!bytes.has_value() || !digest.has_value()) {
            continue;
        }
        ++digests;
        const auto whole = Sha512::hash(std::as_bytes(std::span(*bytes)));
        checks.expect(std::ranges::equal(whole, *digest),
                      "sha512: the digest of " + std::to_string(bytes->size()) + " bytes");
        // The same bytes in uneven pieces.
        Sha512 pieces;
        for (std::size_t offset = 0, step = 1; offset < bytes->size(); offset += step, step += 7) {
            const auto count = std::min(step, bytes->size() - offset);
            pieces.update(std::as_bytes(std::span(*bytes).subspan(offset, count)));
        }
        checks.expect(std::ranges::equal(pieces.finalize(), *digest),
                      "sha512: the digest of the same bytes given in pieces");
    }
    checks.expect(digests == 6, "sha512: six cases were read");

    int accepted = 0;
    int refused = 0;
    std::istringstream cases(readFixture("ed25519-cases.txt"));
    for (std::string line; std::getline(cases, line);) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string name, keyText, messageText, signatureText, expected;
        fields >> name >> keyText >> messageText >> signatureText >> expected;
        const auto keyBytes = fromHex(keyText);
        const auto message = fromHex(messageText);
        const auto signatureBytes = fromHex(signatureText);
        const bool readable = keyBytes.has_value() && message.has_value() &&
                              signatureBytes.has_value() && keyBytes->size() == 32 &&
                              signatureBytes->size() == 64;
        checks.expect(readable, "ed25519 " + name + ": the case is readable");
        if (!readable) {
            continue;
        }
        Ed25519PublicKey key{};
        Ed25519Signature signature{};
        std::ranges::copy(*keyBytes, key.begin());
        std::ranges::copy(*signatureBytes, signature.begin());
        const bool valid = verifyEd25519(key, std::as_bytes(std::span(*message)), signature);
        checks.expect(valid == (expected == "yes"),
                      "ed25519 " + name + ": " +
                          (expected == "yes" ? "the signature is accepted"
                                             : "the signature is refused"));
        (expected == "yes" ? accepted : refused) += 1;

        // In pieces, as a download is checked.
        Ed25519Verifier pieces(key, signature);
        for (std::size_t offset = 0; offset < message->size(); offset += 100) {
            const auto count = std::min<std::size_t>(100, message->size() - offset);
            pieces.update(std::as_bytes(std::span(*message).subspan(offset, count)));
        }
        checks.expect(pieces.finish() == valid,
                      "ed25519 " + name + ": the answer is the same for the message in pieces");
    }
    checks.expect(accepted == 4 && refused == 9, "ed25519: four accepted and nine refused cases");

    // Base64, one spelling per value.
    const auto abc = decodeBase64("YWJj");
    checks.expect(abc.has_value() && *abc == std::vector<std::uint8_t>{'a', 'b', 'c'},
                  "base64: a full group");
    const auto ab = decodeBase64("YWI=");
    checks.expect(ab.has_value() && ab->size() == 2, "base64: one padding character");
    const auto a = decodeBase64("YQ==");
    checks.expect(a.has_value() && a->size() == 1, "base64: two padding characters");
    for (const std::string_view refusedText :
         {"YWJ", "YWJj\n", "YW Jj", "YWI", "Y===", "=WJj", "YQ=a", "YR==", "YWJ=", "YWJj====",
          "YWJ-", "YWJ_"}) {
        checks.expect(!decodeBase64(refusedText).has_value(),
                      "base64: '" + std::string(refusedText) + "' is refused");
    }
    checks.expect(decodeBase64("").has_value() && decodeBase64("")->empty(), "base64: empty");

    // The key list of a build.
    const auto trusted = trimmedFixture("trusted-key.txt");
    const auto other = trimmedFixture("other-key.txt");
    const auto one = parsePublicKeys(trusted);
    const auto two = parsePublicKeys(trusted + "," + other);
    checks.expect(one.has_value() && one->size() == 1, "keys: one key");
    checks.expect(two.has_value() && two->size() == 2, "keys: two keys");
    const auto repeated = parsePublicKeys(trusted + "," + trusted);
    checks.expect(repeated.has_value() && repeated->size() == 1,
                  "keys: a repeated key is one key");
    const auto none = parsePublicKeys("");
    checks.expect(none.has_value() && none->empty(), "keys: an empty list has no key");
    for (const std::string& refusedText :
         {trusted + ",", "," + trusted, trusted.substr(4), std::string("YWJj"),
          trusted + ";" + other}) {
        checks.expect(!parsePublicKeys(refusedText).has_value(),
                      "keys: a list with an entry that is no key is refused");
    }
    checks.expect(one.has_value() &&
                      publicKeyId(one->front()) ==
                          "21fe31dfa154a261626bf854046fd2271b7bed4b6abe45aa58877ef47f9721b9",
                  "keys: the key's name is the SHA-256 of its bytes");
    return checks.result();
}

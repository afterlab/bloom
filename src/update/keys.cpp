#include <bloom/update/keys.hpp>

#include <bloom/update/base64.hpp>
#include <bloom/update/sha256.hpp>

#include <algorithm>

namespace bloom::update {

std::optional<std::vector<Ed25519PublicKey>> parsePublicKeys(std::string_view text) {
    std::vector<Ed25519PublicKey> keys;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto entry = text.substr(0, comma);
        const auto bytes = decodeBase64(entry);
        if (!bytes.has_value() || bytes->size() != kEd25519PublicKeyBytes) {
            return std::nullopt;
        }
        Ed25519PublicKey key{};
        std::ranges::copy(*bytes, key.begin());
        if (std::ranges::find(keys, key) == keys.end()) {
            keys.push_back(key);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
        if (text.empty()) {
            return std::nullopt;
        }
    }
    return keys;
}

std::string publicKeyId(const Ed25519PublicKey& key) {
    const auto digest = Sha256::hash(std::as_bytes(std::span(key)));
    if (!digest.has_value()) {
        return {};
    }
    const auto hex = digest->toLowercaseHex();
    return std::string(hex.data(), hex.size());
}

} // namespace bloom::update

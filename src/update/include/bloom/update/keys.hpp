#pragma once

#include <bloom/update/ed25519.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::update {

// The public keys a build accepts updates from: one today, several while a key is being replaced.
// Written as the Base64 of each key's 32 bytes, separated by commas (the build setting
// BLOOM_UPDATE_PUBLIC_KEYS). Nothing is returned when any entry is not a key, so a mistyped
// setting never leaves a build that accepts fewer keys than its maker meant, or none.
[[nodiscard]] std::optional<std::vector<Ed25519PublicKey>> parsePublicKeys(std::string_view text);

// The name a signature uses for the key that made it: the lowercase hexadecimal SHA-256 of the
// key's 32 bytes.
[[nodiscard]] std::string publicKeyId(const Ed25519PublicKey& key);

} // namespace bloom::update

#pragma once

#include <bloom/update/ed25519.hpp>
#include <bloom/update/feed.hpp>
#include <bloom/update/refusal.hpp>

#include <filesystem>
#include <functional>
#include <span>

namespace bloom::update {

// Checks a downloaded file against its entry in a verified feed: the size, the SHA-256, and the
// Ed25519 signature by one of the keys this build carries. All three must hold. The file is read
// once in pieces for its size and checksum (`cancelled` is asked between pieces), and only a
// file that passes both is then given whole, as a mapping, to the signature check. A file that
// fails is never handed to anything that installs it.
[[nodiscard]] UpdateDiagnostic verifyUpdateFile(const std::filesystem::path& path,
                                                const FeedFile& expected,
                                                std::span<const Ed25519PublicKey> trustedKeys,
                                                const std::function<bool()>& cancelled = {});

} // namespace bloom::update

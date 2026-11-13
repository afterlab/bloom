#pragma once
#include <bloom/core/sha256.hpp>
#include <bloom/platform/source_identity.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bloom::platform {
// The encoded bytes of one source file as a local store returned them, with the SHA-256 the
// store checked them against on this read.
struct StoredSourceBytes final {
    std::vector<std::byte> bytes;
    core::Sha256Digest digest;
};
// A local store of source files' encoded bytes, consulted before a source is read (#718). The
// media readers take one through this interface so they do not depend on the cache module that
// implements it (`bloom::media::cache::SourceMirror`). Both calls are worker-thread calls.
class SourceStore {
  public:
    SourceStore() = default;
    SourceStore(const SourceStore&) = delete;
    SourceStore& operator=(const SourceStore&) = delete;
    virtual ~SourceStore() = default;
    // The stored bytes of `source` when the store holds them for exactly this identity (size,
    // modification time and file id); nullopt otherwise. Never reads the source.
    [[nodiscard]] virtual std::optional<StoredSourceBytes>
    find(const std::filesystem::path& source, const FileIdentity& identity,
         const std::function<bool()>& cancel) = 0;
    // Write-through: asks the store to keep `bytes`, which were just read from `source` while it
    // had `identity`. Returns at once; the write happens in the background and may be dropped.
    // `owner` keeps `bytes` alive until then.
    virtual void keep(const std::filesystem::path& source, const FileIdentity& identity,
                      std::shared_ptr<const void> owner, std::span<const std::byte> bytes) = 0;
};
} // namespace bloom::platform

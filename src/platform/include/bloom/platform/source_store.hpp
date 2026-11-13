#pragma once
#include <bloom/core/sha256.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <bloom/platform/source_identity.hpp>

#include <cstddef>
#include <cstdint>
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
// What one asset says about keeping a local copy of its source (#718): nothing (the folder
// rules, the preference and the automatic rule decide), always, or never.
enum class LocalCopyOverride : std::uint8_t { Inherit, Always, Never };

// A local store of source files' encoded bytes, consulted before a source is read (#718). The
// media readers take one through this interface so they do not depend on the cache module that
// implements it (`bloom::media::cache::SourceMirror`). Both calls are worker-thread calls.
class SourceStore {
  public:
    SourceStore() = default;
    SourceStore(const SourceStore&) = delete;
    SourceStore& operator=(const SourceStore&) = delete;
    virtual ~SourceStore() = default;
    // Whether this store keeps a local copy of `source` at all: the asset's override, then the
    // user's folder rules and preference, then the automatic rule for the volume the identity
    // names. A reader consults it once and then neither looks up nor keeps a source it refuses.
    [[nodiscard]] virtual bool keeps(const std::filesystem::path& source,
                                     const FileIdentity& identity,
                                     LocalCopyOverride asset) const = 0;
    // #718: the byte-range store for one large source the store keeps (a video container), for
    // a progressive snapshot to read local ranges from and keep new ones in. Null when the store
    // keeps no ranges (the caller then reads every range from the source).
    [[nodiscard]] virtual std::shared_ptr<SourceChunkStore>
    chunks(const std::filesystem::path& source, const FileIdentity& identity) {
        static_cast<void>(source);
        static_cast<void>(identity);
        return nullptr;
    }
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
    // Drops whatever the store holds of `source`: its bytes turned out not to be the recorded
    // source (a range-filled copy that failed its hash). Only that source's entries go.
    virtual void forget(const std::filesystem::path& source) { static_cast<void>(source); }
    // True while the store has a backlog of writes: a background fill (the read-ahead) holds off
    // until it clears, so the copy never competes with the frames being shown or exported.
    [[nodiscard]] virtual bool underPressure() const = 0;
};
// The process's own store of source media (#718): the one a host opened for its user. An
// evaluator's cache made while it is set reads through it without being handed it, so the
// evaluators that live for one measurement or one export (layer alignment, tracking, scripted
// queries, an export's CPU fallback) use the same local copies as the Viewer. Null until a host
// sets it; a host that owns the store clears it before the store goes away.
void setProcessSourceStore(std::shared_ptr<SourceStore> store);
[[nodiscard]] std::shared_ptr<SourceStore> processSourceStore();
// How one read may use a store: which store (none: read the source directly) and what the asset
// being read says about local copies.
struct SourceStoreAccess final {
    SourceStore* store = nullptr;
    LocalCopyOverride asset = LocalCopyOverride::Inherit;
};
// Process-wide counters of where source bytes came from, for the status bar and for tests that
// prove a source is read once: reuses of a revision still held in memory, reads served by a
// local store, and (in sourceReadWork()) whole reads of the source itself. Counters saturate.
struct SourceTierWork final {
    std::uint64_t memoryHits = 0;
    std::uint64_t storeHits = 0, storeHitBytes = 0;
};
[[nodiscard]] SourceTierWork sourceTierWork() noexcept;
void recordSourceMemoryHit() noexcept;
void recordSourceStoreHit(std::uint64_t bytes) noexcept;
} // namespace bloom::platform

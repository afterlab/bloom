#pragma once

#include <bloom/core/worker_thread.hpp>
#include <bloom/platform/source_store.hpp>
#include <bloom/platform/storage_class.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// The local mirror of source media that sits on slow storage (#718). See
// docs/architecture/media-io.md "Disk cache" and ADR 0040 for the contract this implements.
// Qt-free: usable from the runtime evaluator, the headless tools and the Qt UI layer.
namespace bloom::media::cache {

// Physical disk-free-derived default, capped so one machine's huge drive cannot silently claim
// dozens of gigabytes: min(10% of free space on the cache volume, capBytes).
inline constexpr std::uint64_t kDefaultSourceMirrorCapBytes = 32ULL * 1024 * 1024 * 1024;
// Independent of the byte budget: a mirror of many tiny files stops growing its index and
// directory-entry count past this many entries.
inline constexpr std::uint64_t kDefaultSourceMirrorMaxEntries = 200'000;
// Encoded bytes waiting for the background writer. A pending write shares the bytes of the source
// revision a frame is using, so the bound is on what the queue alone may keep alive.
inline constexpr std::uint64_t kSourceMirrorWriteQueueByteCapacity = 256ULL * 1024 * 1024;
// A write is skipped while the cache volume has less free space than the write plus this reserve.
inline constexpr std::uint64_t kSourceMirrorFreeSpaceReserveBytes = 1ULL * 1024 * 1024 * 1024;
// The index file is rewritten after this many changes, or when the writer has been idle for
// kSourceMirrorIndexFlushIdle with changes pending, and at shutdown.
inline constexpr std::size_t kSourceMirrorIndexBatch = 64;
inline constexpr std::chrono::milliseconds kSourceMirrorIndexFlushIdle{2000};

// "Keep local copies of source media": the user's preference, and the override a folder rule or
// an asset's interpretation carries.
enum class LocalCopyPreference : std::uint8_t { Automatic, Always, Never };
using platform::LocalCopyOverride;
struct LocalCopyFolderRule final {
    // An absolute folder; the rule covers every source under it. The longest matching folder wins.
    std::filesystem::path folder;
    bool keep = true;
    friend bool operator==(const LocalCopyFolderRule&, const LocalCopyFolderRule&) = default;
};
struct LocalCopyPolicy final {
    LocalCopyPreference preference = LocalCopyPreference::Automatic;
    std::vector<LocalCopyFolderRule> folders;
    friend bool operator==(const LocalCopyPolicy&, const LocalCopyPolicy&) = default;
};
// Whether a source gets a local copy. Resolution order: the asset's override, the longest folder
// rule that holds the source, the preference, and then the automatic rule: Network, Slow attached
// and Unknown storage is copied; Fast local storage and a source on the cache's own volume never.
[[nodiscard]] bool keepsLocalCopy(const LocalCopyPolicy& policy, LocalCopyOverride asset,
                                  const std::filesystem::path& source,
                                  platform::StorageClass storage, bool onCacheVolume);

struct SourceMirrorConfig final {
    // The mirror's private root directory (typically bloom::platform::userCacheDirectory() plus a
    // "media" leaf). Created lazily on first write.
    std::filesystem::path rootDirectory;
    // 0 defers to kDefaultSourceMirrorCapBytes; a settings override passes its own byte count.
    std::uint64_t byteBudget = 0;
    std::uint64_t maxEntryCount = kDefaultSourceMirrorMaxEntries;
    // 0 defers to kSourceMirrorWriteQueueByteCapacity. Tests set a small value.
    std::uint64_t writeQueueByteCapacity = 0;
    // 0 defers to kSourceMirrorFreeSpaceReserveBytes. Tests set a huge value to see a refusal.
    std::uint64_t freeSpaceReserveBytes = 0;
    bool enabled = true;
    LocalCopyPolicy policy{};
};

struct SourceMirrorStatistics final {
    // Sources served from the local copy, and the lookups that found none.
    std::uint64_t diskHits = 0, diskHitBytes = 0, diskMisses = 0;
    // Byte ranges of large sources (video containers) served from local chunks, and the ranges
    // kept after they were read from the source. A served range is also a disk hit, and a kept
    // one a network (or other slow) read, like a whole file's.
    std::uint64_t rangeHits = 0, rangeHitBytes = 0, rangeWrites = 0;
    // Whole reads of a source that were handed to the mirror afterwards, by where the source
    // lives: a network volume, or other slow (or always-copied) storage.
    std::uint64_t networkReads = 0, networkReadBytes = 0;
    std::uint64_t otherSourceReads = 0, otherSourceReadBytes = 0;
    // Entries removed because their bytes no longer matched their recorded digest, or because the
    // source they mirror changed.
    std::uint64_t corruptDropped = 0, staleDropped = 0;
    std::uint64_t evictions = 0;
    std::uint64_t writes = 0;
    // Pending writes dropped because the queue was full (the oldest goes first), and writes
    // skipped because the cache volume was short of free space.
    std::uint64_t droppedWrites = 0, refusedForDiskSpace = 0;
    std::uint64_t pendingWrites = 0, pendingWriteBytes = 0, peakPendingWriteBytes = 0;
    std::uint64_t entryCount = 0, storedBytes = 0;
    // False until the index has been read at startup (on the mirror's own thread).
    bool indexLoaded = false;
    // The last write was skipped because the cache volume is short of free space.
    bool diskPressure = false;
};

enum class LocalCopyState : std::uint8_t {
    // The index has not been read yet.
    Unknown,
    NotCached,
    // Part of the source is local (a container's chunks).
    Partial,
    Cached,
};

// A bounded, corruption-checked local mirror of source files' encoded bytes, keyed by the
// source's path and checked against its metadata identity (size, modification time, file id).
// A hit is the exact bytes the source held under that identity, so decoding them is decoding the
// source. Writes are write-through and never wait: the bytes a read brought from the source go to
// their caller at once and reach the disk on the mirror's own low-priority thread.
//
// Thread-safety: every public member function may be called concurrently from any thread. No
// function holds the mirror's lock across file I/O or hashing, and none blocks on the writer
// except flush() and the destructor. Two instances rooted at one directory (two processes) are
// safe with each other: entries land by temp file and rename, and each entry file carries its own
// identity and digest, so a reader never serves a torn or foreign file.
class SourceMirror final : public platform::SourceStore {
  public:
    explicit SourceMirror(SourceMirrorConfig config, runtime::MemoryBudgetLedger& ledger =
                                                         runtime::processMemoryBudgetLedger());
    ~SourceMirror() override;

    // platform::SourceStore. keeps() is keepsLocalCopy() for the identity's volume. find() reads
    // and verifies the local file on the calling thread; keep() queues the write. A disabled mirror
    // misses and keeps nothing.
    [[nodiscard]] bool keeps(const std::filesystem::path& source,
                             const platform::FileIdentity& identity,
                             LocalCopyOverride asset) const override;
    [[nodiscard]] std::optional<platform::StoredSourceBytes>
    find(const std::filesystem::path& source, const platform::FileIdentity& identity,
         const std::function<bool()>& cancel) override;
    void keep(const std::filesystem::path& source, const platform::FileIdentity& identity,
              std::shared_ptr<const void> owner, std::span<const std::byte> bytes) override;
    // The byte ranges of one large source: a sparse local file of 1 MiB chunks, each checked
    // against its own hash when it is read back, under the same identity check as a whole entry.
    // Opening it is local file I/O on the calling worker. Null when the mirror is disabled or
    // the source could never fit the byte budget. The returned store outlives the mirror safely
    // (it then holds nothing and keeps nothing).
    [[nodiscard]] std::shared_ptr<platform::SourceChunkStore>
    chunks(const std::filesystem::path& source, const platform::FileIdentity& identity) override;

    // Whether a source on this volume gets a local copy under the current policy. `volume` is the
    // source's FileIdentity::volume. Cheap after the first look at a volume.
    [[nodiscard]] bool keepsLocalCopy(const std::filesystem::path& source, std::uint64_t volume,
                                      LocalCopyOverride asset = LocalCopyOverride::Inherit) const;
    void setPolicy(LocalCopyPolicy policy);
    [[nodiscard]] LocalCopyPolicy policy() const;

    // Whether the mirror holds `source`, from the index alone (no file I/O): for the Assets panel
    // and the status bar.
    [[nodiscard]] LocalCopyState localCopyState(const std::filesystem::path& source) const;
    // Drops the entries of one source (a changed or removed file purges only its own).
    void forget(const std::filesystem::path& source) override;
    // Removes every entry and resets statistics ("Purge media cache").
    void clear();
    // Blocks until the index is loaded and every write requested so far has been applied or
    // dropped, and the index file is current. Tests and shutdown paths only.
    void flush();
    // True while the write queue is more than half full: background fills (the read-ahead) hold
    // off until the writer catches up. A cache volume short of space is not a backlog: writes
    // are then skipped (SourceMirrorStatistics::diskPressure) and reads go on from the source.
    [[nodiscard]] bool underPressure() const override;

    [[nodiscard]] SourceMirrorStatistics statistics() const;
    // The write queue's byte capacity as it stands (memory pressure lowers it).
    [[nodiscard]] std::uint64_t writeQueueByteCapacity() const noexcept {
        return writeQueueByteCapacity_.load(std::memory_order_relaxed);
    }
    void setByteBudget(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t byteBudget() const;
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    void setEnabled(bool value) { enabled_.store(value, std::memory_order_release); }
    [[nodiscard]] const std::filesystem::path& rootDirectory() const noexcept { return root_; }

    // Test-only: the on-disk path of `source`'s entry, so a corruption test can flip a byte.
    [[nodiscard]] std::filesystem::path entryPathForTest(const std::filesystem::path& source) const;
    // Test-only: the on-disk path of `source`'s chunk file and where chunk `index` starts in it.
    [[nodiscard]] std::filesystem::path chunkPathForTest(const std::filesystem::path& source) const;
    [[nodiscard]] static std::uint64_t chunkDataOffsetForTest(std::uint64_t sourceSize,
                                                              std::uint64_t index);
    // Test-only: holds the background writer before its next write, so a test can fill the queue
    // and observe its policy without racing the disk.
    void setWriterPausedForTest(bool paused);

  private:
    // Keyed by the source key for a whole entry and by the source key plus "c" for a chunk file.
    struct IndexEntry final {
        std::uint64_t bytes = 0;
        std::int64_t lastUseNanos = 0;
        // A chunk file's size once every chunk is local; 0 for a whole entry.
        std::uint64_t wholeBytes = 0;
    };
    class ChunkFile;
    // What a chunk file reaches the mirror through: cleared when the mirror goes away.
    struct Hub;
    struct PendingWrite final {
        std::string key;
        platform::FileIdentity identity;
        std::shared_ptr<const void> owner;
        std::span<const std::byte> bytes;
        std::uint64_t generation = 0;
    };

    // The file of an index key: "<key>.src", or "<key>.chunks" for a chunk file's key.
    [[nodiscard]] std::filesystem::path entryPath(const std::string& key) const;
    // Caller holds mutex_: a chunk file that is open stops reading and keeping.
    void retireOpenChunks(const std::string& indexKey);
    // Accounting for a chunk file, called by it: bytes that became local (`kept` when they were
    // just read from the source) or were dropped as corrupt, and a read served from it.
    void chunksGrew(const std::string& indexKey, std::uint64_t wholeBytes, std::uint64_t bytes,
                    bool kept, bool network);
    void chunksCorrupt(const std::string& indexKey, std::uint64_t bytes);
    void chunksServed(const std::string& indexKey, std::uint64_t bytes);
    void chunksRefusedForDiskSpace();
    [[nodiscard]] std::filesystem::path indexPath() const;
    // Where the verified source identities persist (platform::VerifiedSourceDigests).
    [[nodiscard]] std::filesystem::path verifiedSourcesPath() const;
    void writerLoop();
    void loadIndex();
    void writeEntry(const PendingWrite& write);
    void writeIndex();
    void evict(std::vector<std::string>& victims);
    void removeFiles(const std::vector<std::string>& keys) const;
    void noteUse(const std::string& key, std::uint64_t bytes);
    void dropEntry(const std::string& key);
    [[nodiscard]] bool diskHasRoomFor(std::uint64_t bytes);

    runtime::MemoryBudgetLedger& ledger_;
    const std::shared_ptr<Hub> hub_;
    const std::filesystem::path root_;
    const std::uint64_t maxEntryCount_;
    const std::uint64_t freeSpaceReserve_;
    std::atomic<bool> enabled_;
    std::atomic<std::uint64_t> writeQueueByteCapacity_;
    std::atomic<bool> diskPressure_{false};
    // The cache volume's id, resolved once on the writer thread (nullopt until then or when the
    // root cannot be observed).
    std::atomic<std::uint64_t> cacheVolume_{0};
    std::atomic<bool> cacheVolumeKnown_{false};

    // Index, policy and statistics. Never held across file I/O.
    mutable std::mutex mutex_;
    std::unordered_map<std::string, IndexEntry> index_;
    std::unordered_map<std::string, std::weak_ptr<ChunkFile>> openChunks_;
    std::uint64_t storedBytes_ = 0;
    std::uint64_t byteBudget_;
    std::size_t indexChanges_ = 0;
    bool indexLoaded_ = false;
    std::uint64_t generation_ = 0; // bumped by clear(): writes queued before it are abandoned
    LocalCopyPolicy policy_;
    SourceMirrorStatistics statistics_;
    runtime::MemoryPoolCounters counters_;

    // The write queue. Queue accounting never takes the index mutex: a memory-pressure callback
    // cannot wait on it.
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::condition_variable idleCv_;
    std::deque<PendingWrite> queue_;
    std::uint64_t queuedBytes_ = 0;
    std::uint64_t peakQueuedBytes_ = 0;
    std::uint64_t droppedWrites_ = 0;
    std::size_t inFlight_ = 0;
    bool indexRequested_ = false; // flush() asked for the index to be written now
    bool loaded_ = false;         // the startup load finished (queueMutex_ side of indexLoaded_)
    std::atomic<bool> stopping_{false};
    std::atomic<bool> writerPaused_{false};
    bloom::core::WorkerThread writer_;
};

// Free-space-derived default budget for a mirror rooted at `root` (or its nearest existing
// ancestor): min(10% of free space, capBytes). Returns capBytes when free space is unknown.
[[nodiscard]] std::uint64_t
defaultSourceMirrorByteBudget(const std::filesystem::path& root,
                              std::uint64_t capBytes = kDefaultSourceMirrorCapBytes);

} // namespace bloom::media::cache

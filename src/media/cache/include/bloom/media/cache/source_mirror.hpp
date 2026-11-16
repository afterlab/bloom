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

// The disk headroom (#732): the free space the mirror leaves on the cache volume. The mirror has
// no byte budget of its own: it may hold what it stores now plus the free space beyond the
// headroom, measured when it writes. The default headroom is the smaller of a tenth of the
// volume's capacity and this cap.
inline constexpr std::uint64_t kDefaultSourceMirrorHeadroomCapBytes = 32ULL * 1024 * 1024 * 1024;
[[nodiscard]] constexpr std::uint64_t
defaultSourceMirrorHeadroom(const std::uint64_t capacityBytes) noexcept {
    return capacityBytes / 10 < kDefaultSourceMirrorHeadroomCapBytes
               ? capacityBytes / 10
               : kDefaultSourceMirrorHeadroomCapBytes;
}
// A cache volume whose free space cannot be read gives nothing to measure: the mirror then holds
// at most this much.
inline constexpr std::uint64_t kSourceMirrorUnmeasuredByteLimit = 32ULL * 1024 * 1024 * 1024;
// Independent of the bytes: a mirror of many tiny files stops growing its index and
// directory-entry count past this many entries.
inline constexpr std::uint64_t kDefaultSourceMirrorMaxEntries = 200'000;
// Encoded bytes waiting for the background writer. A pending write shares the bytes of the source
// revision a frame is using, so the bound is on what the queue alone may keep alive.
inline constexpr std::uint64_t kSourceMirrorWriteQueueByteCapacity = 256ULL * 1024 * 1024;
// The floor under every headroom: no write may leave the cache volume with less free space than
// this, whatever the headroom setting says.
inline constexpr std::uint64_t kSourceMirrorFreeSpaceReserveBytes = 1ULL * 1024 * 1024 * 1024;
// An entry used within this window is in use: a write that needs its room is refused instead.
inline constexpr std::chrono::minutes kSourceMirrorHoldWindow{10};
// One measurement of the cache volume serves the writes of this long; the mirror's own writes and
// removals are applied to it meanwhile.
inline constexpr std::chrono::seconds kSourceMirrorDriveSampleLife{1};
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

// What the cache volume reports: its size and the bytes free on it.
struct SourceMirrorDriveSpace final {
    std::uint64_t capacityBytes = 0;
    std::uint64_t freeBytes = 0;
};

struct SourceMirrorConfig final {
    // The mirror's private root directory (typically bloom::platform::userCacheDirectory() plus a
    // "media" leaf). Created lazily on first write.
    std::filesystem::path rootDirectory;
    // The space to keep free on the cache volume. 0 is the default,
    // defaultSourceMirrorHeadroom() of the volume's capacity.
    std::uint64_t headroomBytes = 0;
    std::uint64_t maxEntryCount = kDefaultSourceMirrorMaxEntries;
    // 0 defers to kSourceMirrorWriteQueueByteCapacity. Tests set a small value.
    std::uint64_t writeQueueByteCapacity = 0;
    // 0 defers to kSourceMirrorFreeSpaceReserveBytes. Tests set a small value so that a headroom
    // of a few bytes is not raised to the floor.
    std::uint64_t freeSpaceReserveBytes = 0;
    bool enabled = true;
    LocalCopyPolicy policy{};
    // Test seams; both unset in production. `clock` is the time, in nanoseconds, an entry is
    // stamped with when it is used (unset: the wall clock). The mirror keeps the stamps strictly
    // increasing itself. It may be called under the mirror's lock, so it must not call the mirror.
    std::function<std::int64_t()> clock{};
    // The cache volume's capacity and free space, or nothing when they cannot be read (unset: the
    // file system is asked). Called with no lock held, never on the UI thread.
    std::function<std::optional<SourceMirrorDriveSpace>()> driveSpace{};
};

// For tests that are not about space: a cache volume of a fixed size and free space, so that a
// mirror never meets its headroom whatever the machine running the test has free.
[[nodiscard]] inline std::function<std::optional<SourceMirrorDriveSpace>()>
roomyDriveSpaceForTest() {
    return [] {
        return std::optional(SourceMirrorDriveSpace{std::uint64_t{1} << 40U, std::uint64_t{1} << 39U});
    };
}

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
    // refused because there was no room for them: the space beyond the headroom was used up and
    // what could have made room is still in use (or the entry limit was reached the same way).
    std::uint64_t droppedWrites = 0, refusedWrites = 0;
    std::uint64_t pendingWrites = 0, pendingWriteBytes = 0, peakPendingWriteBytes = 0;
    std::uint64_t entryCount = 0, storedBytes = 0;
    // The cache volume as last measured (on the mirror's own thread or a worker, never here):
    // its capacity, its free space, the headroom in effect (the setting or the default, never
    // under the floor) and what the mirror may hold now: what it stores plus the room it may
    // still grow into. All 0 until the first measurement; `driveMeasured` is false while the
    // volume cannot be read, and then `availableBytes` is the fixed limit that applies instead.
    std::uint64_t capacityBytes = 0, freeBytes = 0, headroomBytes = 0, availableBytes = 0;
    bool driveMeasured = false;
    // False until the index has been read at startup (on the mirror's own thread).
    bool indexLoaded = false;
    // The last write was refused for lack of room.
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
// Space (#732). The mirror grows into the cache volume's free space beyond the headroom and
// gives space back, least recently used first, when other programs bring the free space under
// it. A write that needs room takes the least recently used entries as victims, and is refused
// (counted, nothing evicted) when a victim is in use: used within kSourceMirrorHoldWindow, or
// used since this same source was last asked for and found no place (it was evicted or refused
// before). The second rule keeps a loop larger than the space on a stable set of entries however
// long one pass takes, and still lets new work replace entries nothing has used for the window.
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
    // Opening it is local file I/O on the calling worker. Null when the mirror is disabled, the
    // source is larger than the space the mirror may hold now, or there is no room to start its
    // file. The returned store outlives the mirror safely (it then holds nothing and keeps nothing).
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
    // off until the writer catches up. A mirror with no room is not a backlog: writes are then
    // refused (SourceMirrorStatistics::diskPressure) and reads go on from the source.
    [[nodiscard]] bool underPressure() const override;

    [[nodiscard]] SourceMirrorStatistics statistics() const;
    // The write queue's byte capacity as it stands (memory pressure lowers it).
    [[nodiscard]] std::uint64_t writeQueueByteCapacity() const noexcept {
        return writeQueueByteCapacity_.load(std::memory_order_relaxed);
    }
    // The headroom setting (0: the default). A new value applies to the next write, and the
    // mirror's own thread gives space back at once when the volume is now short of it; nothing
    // is measured or removed on the calling thread.
    void setHeadroom(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t headroom() const;
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
    // The cache volume as last measured, kept current by the mirror's own writes and removals.
    struct DriveSample final {
        bool taken = false;    // a measurement was attempted
        bool measured = false; // and the volume answered
        std::uint64_t capacityBytes = 0, freeBytes = 0;
        std::int64_t atNanos = 0;
    };
    // Room held for a write that was admitted and has not reached the index yet.
    struct Reservation final {
        std::uint64_t bytes = 0;
        bool entry = false; // the write adds an index entry
    };
    // One entry in least-recently-used order, as it was when the order was taken.
    struct Aged final {
        std::int64_t usedNanos = 0;
        std::string key;
    };
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
    // just read from the source, under a reservation admit() gave) or were dropped as corrupt,
    // and a read served from it.
    void chunksGrew(const std::string& indexKey, std::uint64_t wholeBytes, std::uint64_t bytes,
                    bool kept, bool network);
    void chunksCorrupt(const std::string& indexKey, std::uint64_t bytes);
    void chunksServed(const std::string& indexKey, std::uint64_t bytes);
    [[nodiscard]] std::filesystem::path indexPath() const;
    // Where the verified source identities persist (platform::VerifiedSourceDigests).
    [[nodiscard]] std::filesystem::path verifiedSourcesPath() const;
    void writerLoop();
    void loadIndex();
    void writeEntry(const PendingWrite& write);
    void writeIndex();
    void removeFiles(const std::vector<std::string>& keys) const;
    void noteUse(const std::string& key, std::uint64_t bytes);
    void dropEntry(const std::string& key);

    // Space. None of these is called with a lock held unless its name ends in Locked, and those
    // need mutex_.
    // Measures the cache volume when the last measurement is older than
    // kSourceMirrorDriveSampleLife, or always when `fresh`. Never on the UI thread.
    void measureDrive(bool fresh);
    // Asks for room for `bytes` more under `key`: gives back what the headroom is short of,
    // evicts victims that are not in use, and reserves the room. Empty (and counted as a refused
    // write) when the room cannot be made without evicting an entry in use.
    [[nodiscard]] std::optional<Reservation> admit(const std::string& key, std::uint64_t bytes);
    // The same decision without evicting, reserving or counting.
    [[nodiscard]] bool wouldAdmit(const std::string& key, std::uint64_t bytes);
    // Returns a reservation whose write did not happen.
    void release(const Reservation& reservation);
    // Measures the volume and gives back what the headroom is short of: the writer's idle tick.
    void maintain();
    [[nodiscard]] std::uint64_t keepFreeLocked() const;
    [[nodiscard]] std::uint64_t roomLocked() const;
    [[nodiscard]] std::uint64_t deficitLocked() const;
    // Chooses the victims that make room for `bytes` more under `key` (and for one more entry
    // when `entry`). False when an entry in use would have to go; `victims` is then untouched.
    [[nodiscard]] bool planRoomLocked(const std::string& key, std::uint64_t bytes, bool entry,
                                      std::vector<std::string>& victims);
    // Evicts, least recently used first and whether in use or not, until the volume has its
    // headroom back and the index is inside its entry limit.
    void giveBackLocked(std::vector<std::string>& victims);
    // Removes `key` from the index as an eviction and remembers when it was last used.
    void evictLocked(const std::string& key);
    void correctFreeLocked(std::uint64_t added, std::uint64_t removed);
    void rememberDepartedLocked(const std::string& key, std::int64_t lastAskedNanos);
    // Calls `visit(key, entry)` from the least recently used entry on, leaving out `except`,
    // until it returns false.
    template <typename Visit>
    void forEachOldestLocked(const std::string& except, const Visit& visit);
    // The time a use is stamped with: the configured clock, strictly increasing.
    [[nodiscard]] std::int64_t now() const;
    // The cache volume's capacity and free space as the configured probe (or the file system)
    // reports them now. Never called with a lock held.
    [[nodiscard]] std::optional<SourceMirrorDriveSpace> driveSpace() const;

    runtime::MemoryBudgetLedger& ledger_;
    const std::shared_ptr<Hub> hub_;
    const std::filesystem::path root_;
    const std::uint64_t maxEntryCount_;
    const std::uint64_t freeSpaceReserve_;
    const std::function<std::int64_t()> clock_;
    const std::function<std::optional<SourceMirrorDriveSpace>()> driveSpace_;
    mutable std::atomic<std::int64_t> lastStamp_{0};
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
    std::uint64_t headroom_; // the setting; 0 is the default
    DriveSample drive_;
    // Counts the changes this mirror made to what the volume holds, so that a measurement taken
    // while one happened can be told apart and taken again.
    std::uint64_t spaceChanges_ = 0;
    // Room admitted to writes that have not reached the index yet.
    std::uint64_t reservedBytes_ = 0;
    std::uint64_t reservedEntries_ = 0;
    // The sources that found no place: evicted (with the time of their last use) or refused (with
    // the time of the refusal), by a hash of their key. Bounded by the entry limit; a purge
    // clears it. Kept in memory only.
    std::unordered_map<std::uint64_t, std::int64_t> departed_;
    // The oldest entries in order, consumed from `oldestNext_`: an entry that was used or removed
    // since no longer matches its record and is passed over. Everything not listed is newer.
    std::vector<Aged> oldest_;
    std::size_t oldestNext_ = 0;
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

} // namespace bloom::media::cache

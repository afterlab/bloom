#include <bloom/media/cache/source_mirror.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/platform/thread_priority.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bloom::media::cache {

namespace {

constexpr std::array<char, 4> kMagic = {'B', 'L', 'S', '2'};
constexpr std::uint32_t kFormatVersion = 1;
// magic(4) + version(4) + sourceSize(8) + modified(8) + fileIdHigh(8) + fileIdLow(8) +
// payloadBytes(8) + payloadDigest(32)
constexpr std::size_t kHeaderBytes = 4 + 4 + 8 + 8 + 8 + 8 + 8 + 32;
constexpr std::size_t kIoChunkBytes = std::size_t{4} << 20;
constexpr std::string_view kEntrySuffix = ".src";
constexpr std::string_view kIndexHeader = "bloom-source-mirror-index 2";

void putU32(std::span<std::byte> out, const std::size_t offset, const std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
}
void putU64(std::span<std::byte> out, const std::size_t offset, const std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
}
[[nodiscard]] std::uint32_t getU32(std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
                 << (8U * i);
    return value;
}
[[nodiscard]] std::uint64_t getU64(std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
                 << (8U * i);
    return value;
}

[[nodiscard]] std::int64_t nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct EntryHeader final {
    std::uint64_t sourceSize = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::uint64_t fileIdHigh = 0, fileIdLow = 0;
    std::uint64_t payloadBytes = 0;
    core::Sha256Digest digest;
    // The identity check (#718): size, modification time and file id. The device number and the
    // status-change time are not part of it: a network mount gets a new device number each time
    // it is mounted, and a rename or permission change moves the status-change time.
    [[nodiscard]] bool matches(const platform::FileIdentity& identity) const noexcept {
        return sourceSize == identity.size && modifiedNanoseconds == identity.modifiedNanoseconds &&
               fileIdHigh == identity.fileIdHigh && fileIdLow == identity.fileIdLow;
    }
};
[[nodiscard]] std::array<std::byte, kHeaderBytes>
serializeHeader(const platform::FileIdentity& identity, const std::uint64_t payloadBytes,
                const core::Sha256Digest& digest) {
    std::array<std::byte, kHeaderBytes> out{};
    for (std::size_t i = 0; i < kMagic.size(); ++i)
        out[i] = static_cast<std::byte>(kMagic[i]);
    putU32(out, 4, kFormatVersion);
    putU64(out, 8, identity.size);
    putU64(out, 16, static_cast<std::uint64_t>(identity.modifiedNanoseconds));
    putU64(out, 24, identity.fileIdHigh);
    putU64(out, 32, identity.fileIdLow);
    putU64(out, 40, payloadBytes);
    const auto& digestBytes = digest.bytes();
    for (std::size_t i = 0; i < digestBytes.size(); ++i)
        out[48 + i] = static_cast<std::byte>(digestBytes[i]);
    return out;
}
[[nodiscard]] std::optional<EntryHeader> parseHeader(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderBytes)
        return std::nullopt;
    for (std::size_t i = 0; i < kMagic.size(); ++i)
        if (bytes[i] != static_cast<std::byte>(kMagic[i]))
            return std::nullopt;
    if (getU32(bytes, 4) != kFormatVersion)
        return std::nullopt;
    EntryHeader header;
    header.sourceSize = getU64(bytes, 8);
    header.modifiedNanoseconds = static_cast<std::int64_t>(getU64(bytes, 16));
    header.fileIdHigh = getU64(bytes, 24);
    header.fileIdLow = getU64(bytes, 32);
    header.payloadBytes = getU64(bytes, 40);
    std::array<std::uint8_t, core::kSha256DigestBytes> digestBytes{};
    for (std::size_t i = 0; i < digestBytes.size(); ++i)
        digestBytes[i] = std::to_integer<std::uint8_t>(bytes[48 + i]);
    header.digest = core::Sha256Digest::fromBytes(digestBytes);
    return header;
}

[[nodiscard]] std::string normalizedText(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
        absolute = path;
    const auto text = absolute.lexically_normal().generic_u8string();
    std::string result(reinterpret_cast<const char*>(text.data()), text.size());
#if defined(_WIN32) || defined(__APPLE__)
    // File names on these systems' default volumes do not differ by case.
    std::ranges::transform(result, result.begin(), [](const char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
#endif
    while (result.size() > 1 && result.back() == '/')
        result.pop_back();
    return result;
}
// The entry key of a source: the SHA-256 of its normalized absolute path, in lowercase hex.
[[nodiscard]] std::string sourceKey(const std::filesystem::path& source) {
    const auto text = normalizedText(source);
    const auto digest =
        core::Sha256Hasher::hash(std::as_bytes(std::span(text.data(), text.size())));
    if (!digest)
        return {};
    const auto hex = digest->toLowercaseHex();
    return {hex.data(), hex.size()};
}
[[nodiscard]] bool isKey(const std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](const char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           });
}

[[nodiscard]] std::string uniqueSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    std::ostringstream out;
    out << std::hex << std::this_thread::get_id() << '-' << nowNanos() << '-'
        << counter.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}
void syncFile(const std::filesystem::path& path) {
#if !defined(_WIN32)
    const int descriptor = ::open(path.c_str(), O_WRONLY); // NOLINT(*-vararg)
    if (descriptor >= 0) {
        static_cast<void>(::fsync(descriptor));
        static_cast<void>(::close(descriptor));
    }
#else
    static_cast<void>(path);
#endif
}
void removeQuietly(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

bool keepsLocalCopy(const LocalCopyPolicy& policy, const LocalCopyOverride asset,
                    const std::filesystem::path& source, const platform::StorageClass storage,
                    const bool onCacheVolume) {
    if (asset != LocalCopyOverride::Inherit)
        return asset == LocalCopyOverride::Always;
    if (!policy.folders.empty()) {
        const auto text = normalizedText(source);
        const LocalCopyFolderRule* best = nullptr;
        std::size_t bestLength = 0;
        for (const auto& rule : policy.folders) {
            if (rule.folder.empty())
                continue;
            const auto folder = normalizedText(rule.folder);
            const bool holds = folder == "/" || text == folder ||
                               (text.size() > folder.size() && text.starts_with(folder) &&
                                (text[folder.size()] == '/' || folder.back() == '/'));
            if (holds && (best == nullptr || folder.size() > bestLength)) {
                best = &rule;
                bestLength = folder.size();
            }
        }
        if (best != nullptr)
            return best->keep;
    }
    switch (policy.preference) {
    case LocalCopyPreference::Always:
        return true;
    case LocalCopyPreference::Never:
        return false;
    case LocalCopyPreference::Automatic:
        break;
    }
    return platform::isSlowStorage(storage) && !onCacheVolume;
}

SourceMirror::SourceMirror(SourceMirrorConfig config, runtime::MemoryBudgetLedger& ledger)
    : ledger_(ledger), root_(std::move(config.rootDirectory)), maxEntryCount_(config.maxEntryCount),
      freeSpaceReserve_(config.freeSpaceReserveBytes != 0 ? config.freeSpaceReserveBytes
                                                          : kSourceMirrorFreeSpaceReserveBytes),
      enabled_(config.enabled), writeQueueByteCapacity_(config.writeQueueByteCapacity != 0
                                                            ? config.writeQueueByteCapacity
                                                            : kSourceMirrorWriteQueueByteCapacity),
      byteBudget_(config.byteBudget != 0 ? config.byteBudget : kDefaultSourceMirrorCapBytes),
      policy_(std::move(config.policy)) {
    // One identity query of the cache folder (local storage), so the automatic rule can tell a
    // source on the cache's own volume from the first frame on.
    if (!root_.empty())
        if (const auto volume = platform::storageVolume(root_).volume) {
            cacheVolume_.store(*volume, std::memory_order_relaxed);
            cacheVolumeKnown_.store(true, std::memory_order_release);
        }
    writer_ = bloom::core::WorkerThread([this] { writerLoop(); });
    try {
        ledger_.registerCache(
            this, static_cast<std::size_t>(writeQueueByteCapacity_.load()),
            [this] {
                const std::lock_guard lock(queueMutex_);
                return static_cast<std::size_t>(queuedBytes_);
            },
            [this](const std::size_t bytes) {
                const std::lock_guard lock(queueMutex_);
                writeQueueByteCapacity_.store(bytes, std::memory_order_relaxed);
                // Memory pressure: the oldest pending writes go first. A write in flight keeps
                // its charge until it finishes; no I/O or wait happens here.
                while (!queue_.empty() && queuedBytes_ > bytes) {
                    queuedBytes_ -= queue_.front().bytes.size();
                    queue_.pop_front();
                    ++droppedWrites_;
                }
                if (queue_.empty() && inFlight_ == 0)
                    idleCv_.notify_all();
            });
        // Host memory is the write queue (scratch); the counters are the mirror's lookups,
        // writes and evictions (bytes on disk) and the writes the queue refused.
        ledger_.setPoolProfile(this, {.name = "Source mirror write queue",
                                      .kind = runtime::ProfilerPoolKind::Scratch,
                                      .counters = &counters_,
                                      .rows = {},
                                      .entries = [this] {
                                          const std::lock_guard lock(queueMutex_);
                                          return queue_.size();
                                      }});
    } catch (...) {
        stopping_.store(true);
        {
            const std::lock_guard lock(queueMutex_);
        }
        queueCv_.notify_all();
        writer_.join();
        throw;
    }
}

SourceMirror::~SourceMirror() {
    ledger_.unregisterCache(this);
    stopping_.store(true);
    {
        const std::lock_guard lock(queueMutex_);
    }
    queueCv_.notify_all();
    if (writer_.joinable())
        writer_.join();
}

std::filesystem::path SourceMirror::entryPath(const std::string& key) const {
    return root_ / "sources" / key.substr(0, 2) / (key + std::string(kEntrySuffix));
}
std::filesystem::path SourceMirror::indexPath() const { return root_ / "sources" / "index.v2"; }
std::filesystem::path SourceMirror::entryPathForTest(const std::filesystem::path& source) const {
    return entryPath(sourceKey(source));
}

std::optional<platform::StoredSourceBytes>
SourceMirror::find(const std::filesystem::path& source, const platform::FileIdentity& identity,
                   const std::function<bool()>& cancel) {
    if (!enabled() || root_.empty())
        return std::nullopt;
    const auto key = sourceKey(source);
    if (key.empty())
        return std::nullopt;
    const auto path = entryPath(key);
    const auto miss = [&] {
        const std::lock_guard lock(mutex_);
        ++statistics_.diskMisses;
        counters_.miss();
        return std::nullopt;
    };
    // Everything below is local file I/O and hashing on the calling worker, outside the lock.
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return miss();
    std::array<std::byte, kHeaderBytes> head{};
    input.read(reinterpret_cast<char*>(head.data()), // NOLINT(*-reinterpret-cast)
               static_cast<std::streamsize>(head.size()));
    const auto header = input.gcount() == static_cast<std::streamsize>(head.size())
                            ? parseHeader(head)
                            : std::nullopt;
    const auto drop = [&](const bool corrupt) {
        input.close();
        removeQuietly(path);
        dropEntry(key);
        const std::lock_guard lock(mutex_);
        ++(corrupt ? statistics_.corruptDropped : statistics_.staleDropped);
        ++statistics_.diskMisses;
        counters_.miss();
        return std::nullopt;
    };
    if (!header || header->payloadBytes != header->sourceSize)
        return drop(true);
    // The source changed since it was mirrored: its entry is its own to purge, nothing else.
    if (!header->matches(identity))
        return drop(false);
    platform::StoredSourceBytes result;
    result.bytes.resize(static_cast<std::size_t>(header->payloadBytes));
    core::Sha256Hasher hasher;
    for (std::size_t offset = 0; offset < result.bytes.size();) {
        if (cancel && cancel())
            return std::nullopt;
        const auto count = std::min(kIoChunkBytes, result.bytes.size() - offset);
        input.read(reinterpret_cast<char*>(result.bytes.data() + offset), // NOLINT
                   static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count))
            return drop(true);
        if (!hasher.update(std::span<const std::byte>(result.bytes.data() + offset, count)))
            return drop(true);
        offset += count;
    }
    if (input.peek() != std::char_traits<char>::eof() || hasher.finalize() != header->digest)
        return drop(true);
    result.digest = header->digest;
    noteUse(key, kHeaderBytes + result.bytes.size());
    {
        const std::lock_guard lock(mutex_);
        ++statistics_.diskHits;
        statistics_.diskHitBytes += result.bytes.size();
        counters_.hit();
    }
    return result;
}

void SourceMirror::keep(const std::filesystem::path& source, const platform::FileIdentity& identity,
                        std::shared_ptr<const void> owner, const std::span<const std::byte> bytes) {
    if (!enabled() || root_.empty() || bytes.size() != identity.size)
        return;
    auto key = sourceKey(source);
    if (key.empty())
        return;
    // Remembered per volume id, so this asks the file system nothing for a volume already seen.
    const bool network =
        platform::storageClass(source, identity.volume) == platform::StorageClass::Network;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        ++(network ? statistics_.networkReads : statistics_.otherSourceReads);
        (network ? statistics_.networkReadBytes : statistics_.otherSourceReadBytes) += bytes.size();
        generation = generation_;
    }
    {
        const std::lock_guard lock(queueMutex_);
        if (stopping_.load())
            return;
        if (std::ranges::any_of(queue_,
                                [&](const PendingWrite& pending) { return pending.key == key; }))
            return;
        // A full queue drops its oldest pending write (never the frame's own read, which already
        // returned): the source it belonged to is simply read once more later.
        const auto capacity = writeQueueByteCapacity_.load(std::memory_order_relaxed);
        while (!queue_.empty() && queuedBytes_ + bytes.size() > capacity) {
            queuedBytes_ -= queue_.front().bytes.size();
            queue_.pop_front();
            ++droppedWrites_;
            counters_.refused();
        }
        queue_.push_back({std::move(key), identity, std::move(owner), bytes, generation});
        queuedBytes_ += bytes.size();
        peakQueuedBytes_ = std::max(peakQueuedBytes_, queuedBytes_);
    }
    queueCv_.notify_one();
}

bool SourceMirror::keepsLocalCopy(const std::filesystem::path& source, const std::uint64_t volume,
                                  const LocalCopyOverride asset) const {
    LocalCopyPolicy policy;
    {
        const std::lock_guard lock(mutex_);
        policy = policy_;
    }
    const bool onCacheVolume = cacheVolumeKnown_.load(std::memory_order_acquire) &&
                               cacheVolume_.load(std::memory_order_relaxed) == volume;
    return cache::keepsLocalCopy(policy, asset, source, platform::storageClass(source, volume),
                                 onCacheVolume);
}
void SourceMirror::setPolicy(LocalCopyPolicy policy) {
    const std::lock_guard lock(mutex_);
    policy_ = std::move(policy);
}
LocalCopyPolicy SourceMirror::policy() const {
    const std::lock_guard lock(mutex_);
    return policy_;
}

LocalCopyState SourceMirror::localCopyState(const std::filesystem::path& source) const {
    const auto key = sourceKey(source);
    const std::lock_guard lock(mutex_);
    if (!indexLoaded_)
        return LocalCopyState::Unknown;
    return index_.contains(key) ? LocalCopyState::Cached : LocalCopyState::NotCached;
}

void SourceMirror::forget(const std::filesystem::path& source) {
    const auto key = sourceKey(source);
    if (key.empty() || root_.empty())
        return;
    dropEntry(key);
    removeQuietly(entryPath(key));
}

void SourceMirror::clear() {
    {
        const std::lock_guard lock(queueMutex_);
        for (const auto& pending : queue_)
            queuedBytes_ -= pending.bytes.size();
        queue_.clear();
    }
    {
        const std::lock_guard lock(mutex_);
        ++generation_;
        index_.clear();
        storedBytes_ = 0;
        indexChanges_ = 0;
        const bool loaded = statistics_.indexLoaded;
        statistics_ = {};
        statistics_.indexLoaded = loaded;
    }
    if (root_.empty())
        return;
    std::error_code ignored;
    std::filesystem::remove_all(root_ / "sources", ignored);
}

void SourceMirror::flush() {
    std::unique_lock lock(queueMutex_);
    indexRequested_ = true;
    queueCv_.notify_all();
    idleCv_.wait(lock, [this] {
        return stopping_.load() ||
               (loaded_ && queue_.empty() && inFlight_ == 0 && !indexRequested_);
    });
}

void SourceMirror::setWriterPausedForTest(const bool paused) {
    writerPaused_.store(paused);
    {
        const std::lock_guard lock(queueMutex_);
    }
    queueCv_.notify_all();
}

bool SourceMirror::underPressure() const {
    if (diskPressure_.load(std::memory_order_relaxed))
        return true;
    const std::lock_guard lock(queueMutex_);
    return queuedBytes_ * 2 > writeQueueByteCapacity_.load(std::memory_order_relaxed);
}

SourceMirrorStatistics SourceMirror::statistics() const {
    SourceMirrorStatistics result;
    {
        const std::lock_guard lock(mutex_);
        result = statistics_;
        result.entryCount = index_.size();
        result.storedBytes = storedBytes_;
        result.indexLoaded = indexLoaded_;
    }
    const std::lock_guard lock(queueMutex_);
    result.droppedWrites = droppedWrites_;
    result.pendingWrites = queue_.size() + inFlight_;
    result.pendingWriteBytes = queuedBytes_;
    result.peakPendingWriteBytes = peakQueuedBytes_;
    return result;
}

void SourceMirror::setByteBudget(const std::uint64_t bytes) {
    std::vector<std::string> victims;
    {
        const std::lock_guard lock(mutex_);
        byteBudget_ = bytes != 0 ? bytes : kDefaultSourceMirrorCapBytes;
        evict(victims);
    }
    removeFiles(victims);
}
std::uint64_t SourceMirror::byteBudget() const {
    const std::lock_guard lock(mutex_);
    return byteBudget_;
}

void SourceMirror::noteUse(const std::string& key, const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    auto [entry, inserted] = index_.try_emplace(key, IndexEntry{bytes, nowNanos()});
    if (inserted)
        storedBytes_ += bytes;
    else
        entry->second.lastUseNanos = nowNanos();
    ++indexChanges_;
}
void SourceMirror::dropEntry(const std::string& key) {
    const std::lock_guard lock(mutex_);
    if (const auto found = index_.find(key); found != index_.end()) {
        storedBytes_ -= std::min(storedBytes_, found->second.bytes);
        index_.erase(found);
        ++indexChanges_;
    }
}

// Caller holds mutex_. Chooses the least recently used entries until the mirror is back under
// its budgets (with a little slack, so a full mirror does not sort its index on every write) and
// removes them from the index; the caller deletes the files outside the lock.
void SourceMirror::evict(std::vector<std::string>& victims) {
    if (storedBytes_ <= byteBudget_ && index_.size() <= maxEntryCount_)
        return;
    const auto byteGoal = byteBudget_ - byteBudget_ / 20;
    const auto countGoal = maxEntryCount_ - maxEntryCount_ / 20;
    std::vector<std::pair<std::int64_t, const std::string*>> order;
    order.reserve(index_.size());
    for (const auto& [key, entry] : index_)
        order.emplace_back(entry.lastUseNanos, &key);
    std::ranges::sort(order);
    for (const auto& [used, key] : order) {
        if (storedBytes_ <= byteGoal && index_.size() - victims.size() <= countGoal)
            break;
        const auto& entry = index_.at(*key);
        storedBytes_ -= std::min(storedBytes_, entry.bytes);
        counters_.released(entry.bytes);
        ++statistics_.evictions;
        victims.push_back(*key);
    }
    for (const auto& key : victims)
        index_.erase(key);
    indexChanges_ += victims.size();
}
void SourceMirror::removeFiles(const std::vector<std::string>& keys) const {
    for (const auto& key : keys)
        removeQuietly(entryPath(key));
}

bool SourceMirror::diskHasRoomFor(const std::uint64_t bytes) {
    std::error_code error;
    auto probe = root_;
    while (!probe.empty() && !std::filesystem::exists(probe, error)) {
        const auto parent = probe.parent_path();
        if (parent == probe)
            break;
        probe = parent;
    }
    const auto space = std::filesystem::space(probe, error);
    // Unknown free space does not stop the mirror: the byte budget still bounds it.
    const bool room = error || space.available == static_cast<std::uintmax_t>(-1) ||
                      space.available >= bytes + freeSpaceReserve_;
    diskPressure_.store(!room, std::memory_order_relaxed);
    return room;
}

void SourceMirror::writeEntry(const PendingWrite& write) {
    const auto abandoned = [&] {
        if (stopping_.load(std::memory_order_relaxed))
            return true;
        const std::lock_guard lock(mutex_);
        return generation_ != write.generation;
    };
    if (abandoned())
        return;
    const auto total = kHeaderBytes + write.bytes.size();
    if (!diskHasRoomFor(total)) {
        const std::lock_guard lock(mutex_);
        ++statistics_.refusedForDiskSpace;
        counters_.refused();
        return;
    }
    // The digest of exactly the bytes the frame decoded. It is the content digest the asset
    // records, so a later hit is a verified revision and corruption on disk is always caught.
    core::Sha256Hasher hasher;
    for (std::size_t offset = 0; offset < write.bytes.size();) {
        if (stopping_.load(std::memory_order_relaxed))
            return;
        const auto count = std::min(kIoChunkBytes, write.bytes.size() - offset);
        if (!hasher.update(write.bytes.subspan(offset, count)))
            return;
        offset += count;
    }
    const auto header = serializeHeader(write.identity, write.bytes.size(), hasher.finalize());
    const auto path = entryPath(write.key);
    std::error_code createError;
    std::filesystem::create_directories(path.parent_path(), createError);
    const auto temporary =
        path.parent_path() / (path.filename().string() + ".tmp-" + uniqueSuffix());
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        bool good = static_cast<bool>(out);
        if (good) {
            out.write(reinterpret_cast<const char*>(header.data()), // NOLINT(*-reinterpret-cast)
                      static_cast<std::streamsize>(header.size()));
            for (std::size_t offset = 0; good && offset < write.bytes.size();) {
                // Shutdown and "Purge media cache" stop a write between chunks; the temporary
                // file is removed and nothing partial is ever visible under the entry's name.
                if (abandoned()) {
                    good = false;
                    break;
                }
                const auto count = std::min(kIoChunkBytes, write.bytes.size() - offset);
                out.write(reinterpret_cast<const char*>(write.bytes.data() + offset), // NOLINT
                          static_cast<std::streamsize>(count));
                good = static_cast<bool>(out);
                offset += count;
            }
            if (good) {
                out.flush();
                good = static_cast<bool>(out);
            }
        }
        out.close();
        if (!good) {
            removeQuietly(temporary);
            return;
        }
    }
    syncFile(temporary);
    std::error_code renameError;
    std::filesystem::rename(temporary, path, renameError);
    if (renameError) {
        removeQuietly(temporary);
        return;
    }
    std::vector<std::string> victims;
    bool stale = false;
    {
        const std::lock_guard lock(mutex_);
        if (generation_ != write.generation) {
            stale = true;
        } else {
            auto& entry = index_[write.key];
            storedBytes_ -= std::min(storedBytes_, entry.bytes);
            entry = IndexEntry{total, nowNanos()};
            storedBytes_ += total;
            ++indexChanges_;
            ++statistics_.writes;
            counters_.store();
            evict(victims);
        }
    }
    if (stale)
        removeQuietly(path);
    removeFiles(victims);
}

// Runs on the writer thread at startup, off the UI thread: reads the index for last-use times,
// then lists the entry files that are really there, so a write that reached the disk without
// reaching the index (a crash) is still counted and evicted.
void SourceMirror::loadIndex() {
    if (root_.empty())
        return;
    std::error_code error;
    // Entries of the decoded-frame cache this mirror replaced: a two-hex-digit folder per shard
    // and "index.v1" directly under the root. They are derived data nothing reads any more.
    if (std::filesystem::exists(root_ / "index.v1", error)) {
        for (std::filesystem::directory_iterator shards(root_, error), end; !error && shards != end;
             shards.increment(error)) {
            const auto name = shards->path().filename().string();
            if (name.size() == 2 && std::isxdigit(static_cast<unsigned char>(name[0])) &&
                std::isxdigit(static_cast<unsigned char>(name[1]))) {
                std::error_code ignored;
                std::filesystem::remove_all(shards->path(), ignored);
            }
            if (stopping_.load(std::memory_order_relaxed))
                return;
        }
        removeQuietly(root_ / "index.v1");
    }
    std::unordered_map<std::string, std::int64_t> used;
    {
        std::ifstream in(indexPath());
        std::string line;
        if (in && std::getline(in, line) && line == kIndexHeader)
            while (std::getline(in, line)) {
                std::istringstream row(line);
                std::string key;
                std::int64_t lastUse = 0;
                if (row >> key >> lastUse && isKey(key))
                    used.emplace(std::move(key), lastUse);
            }
    }
    std::unordered_map<std::string, IndexEntry> found;
    const auto staleBefore = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    error.clear();
    for (std::filesystem::directory_iterator shards(root_ / "sources", error), end;
         !error && shards != end; shards.increment(error)) {
        if (stopping_.load(std::memory_order_relaxed))
            return;
        std::error_code inner;
        if (!shards->is_directory(inner))
            continue;
        for (std::filesystem::directory_iterator files(shards->path(), inner), last;
             !inner && files != last; files.increment(inner)) {
            const auto name = files->path().filename().string();
            std::error_code ignored;
            if (name.ends_with(kEntrySuffix) && isKey(name.substr(0, name.size() - 4))) {
                const auto key = name.substr(0, name.size() - 4);
                const auto bytes = files->file_size(ignored);
                if (ignored)
                    continue;
                const auto known = used.find(key);
                found.emplace(key, IndexEntry{bytes, known != used.end() ? known->second : 0});
            } else if (name.find(".tmp-") != std::string::npos &&
                       files->last_write_time(ignored) < staleBefore && !ignored) {
                // A write another run never finished. A fresh one may belong to a live process.
                removeQuietly(files->path());
            }
        }
    }
    std::vector<std::string> victims;
    {
        const std::lock_guard lock(mutex_);
        for (auto& [key, entry] : found)
            if (index_.try_emplace(key, entry).second)
                storedBytes_ += entry.bytes;
        indexLoaded_ = true;
        evict(victims);
    }
    removeFiles(victims);
}

void SourceMirror::writeIndex() {
    if (root_.empty())
        return;
    std::ostringstream text;
    {
        const std::lock_guard lock(mutex_);
        if (indexChanges_ == 0)
            return;
        indexChanges_ = 0;
        text << kIndexHeader << '\n';
        for (const auto& [key, entry] : index_)
            text << key << ' ' << entry.lastUseNanos << '\n';
    }
    const auto path = indexPath();
    std::error_code createError;
    std::filesystem::create_directories(path.parent_path(), createError);
    const auto temporary = path.parent_path() / ("index.v2.tmp-" + uniqueSuffix());
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const auto data = text.str();
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            out.close();
            removeQuietly(temporary);
            return;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(temporary, path, renameError);
    if (renameError)
        removeQuietly(temporary);
}

void SourceMirror::writerLoop() {
    // Mirror writes and index maintenance yield to everything else: the thread runs at the
    // system's idle CPU and I/O priority for its whole life.
    const platform::IdleThreadPriorityScope idle(/*threadEndsWithScope=*/true);
    loadIndex();
    std::unique_lock lock(queueMutex_);
    loaded_ = true;
    idleCv_.notify_all();
    while (!stopping_.load()) {
        const bool woken = queueCv_.wait_for(lock, kSourceMirrorIndexFlushIdle, [this] {
            return stopping_.load() || (!queue_.empty() && !writerPaused_.load()) ||
                   indexRequested_;
        });
        if (stopping_.load())
            break;
        if (!queue_.empty() && !writerPaused_.load()) {
            auto write = std::move(queue_.front());
            queue_.pop_front();
            inFlight_ = 1;
            lock.unlock();
            writeEntry(write);
            bool batchFull = false;
            {
                const std::lock_guard index(mutex_);
                batchFull = indexChanges_ >= kSourceMirrorIndexBatch;
            }
            if (batchFull)
                writeIndex();
            const auto bytes = write.bytes.size();
            write = {}; // releases the source bytes before the queue reports itself idle
            lock.lock();
            queuedBytes_ -= std::min<std::uint64_t>(queuedBytes_, bytes);
            inFlight_ = 0;
        }
        if (queue_.empty() && (indexRequested_ || !woken)) {
            // Idle with changes pending, or a flush asked for it: bring the index file up to date.
            lock.unlock();
            writeIndex();
            lock.lock();
            if (queue_.empty())
                indexRequested_ = false;
        }
        if (queue_.empty() && inFlight_ == 0)
            idleCv_.notify_all();
    }
    // Shutdown: pending writes are abandoned (their sources are read again another day), the
    // index is left current, and anyone in flush() is released.
    for (const auto& pending : queue_)
        queuedBytes_ -= std::min<std::uint64_t>(queuedBytes_, pending.bytes.size());
    queue_.clear();
    lock.unlock();
    writeIndex();
    idleCv_.notify_all();
}

std::uint64_t defaultSourceMirrorByteBudget(const std::filesystem::path& root,
                                            const std::uint64_t capBytes) {
    std::error_code error;
    auto probe = root;
    while (!probe.empty() && !std::filesystem::exists(probe, error)) {
        const auto parent = probe.parent_path();
        if (parent == probe)
            break;
        probe = parent;
    }
    const auto space = std::filesystem::space(probe, error);
    if (error || space.available == static_cast<std::uintmax_t>(-1))
        return capBytes;
    return std::min<std::uint64_t>(space.available / 10, capBytes);
}

} // namespace bloom::media::cache

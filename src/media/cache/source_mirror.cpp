#include <bloom/media/cache/source_mirror.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/platform/positional_file.hpp>
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
// A chunk file: magic(4) + version(4) + sourceSize(8) + modified(8) + fileIdHigh(8) +
// fileIdLow(8) + chunkBytes(8), padded to kChunkHeaderBytes; then one 8-byte slot per chunk (the
// chunk's hash, 0 while the chunk is missing); then, from the next 4 KiB boundary, the chunks at
// their offsets in the source. The file is sparse: a missing chunk takes no room.
constexpr std::array<char, 4> kChunkMagic = {'B', 'L', 'C', '1'};
constexpr std::size_t kChunkHeaderBytes = 64;
constexpr std::uint64_t kChunkBytes = platform::ReadOnlyFileSnapshot::kProgressiveChunkBytes;
constexpr std::string_view kChunkSuffix = ".chunks";
constexpr char kChunkKeyMark = 'c';
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

// The wall clock, made strictly increasing within the process: two uses never share a time, so
// the least recently used order does not depend on the clock's resolution.
[[nodiscard]] std::int64_t nowNanos() {
    static std::atomic<std::int64_t> last{0};
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    auto previous = last.load(std::memory_order_relaxed);
    std::int64_t next = 0;
    do {
        next = std::max(now, previous + 1);
    } while (!last.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    return next;
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

[[nodiscard]] bool isChunkKey(const std::string_view text) {
    return text.size() == 65 && text.back() == kChunkKeyMark && isKey(text.substr(0, 64));
}

struct ChunkLayout final {
    std::uint64_t sourceSize = 0, count = 0, dataOffset = 0;
    explicit ChunkLayout(const std::uint64_t size)
        : sourceSize(size), count((size + kChunkBytes - 1) / kChunkBytes),
          dataOffset((kChunkHeaderBytes + count * 8 + 4095) / 4096 * 4096) {}
    [[nodiscard]] std::uint64_t length(const std::uint64_t index) const {
        return std::min(kChunkBytes, sourceSize - index * kChunkBytes);
    }
    [[nodiscard]] std::uint64_t wholeBytes() const { return dataOffset + sourceSize; }
};
// A chunk's slot value: the first eight bytes of its SHA-256, never 0 (0 marks a missing chunk).
[[nodiscard]] std::uint64_t chunkHash(const std::span<const std::byte> bytes) {
    const auto digest = core::Sha256Hasher::hash(bytes);
    if (!digest)
        return 0;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(digest->bytes()[i]) << (8U * i);
    return value != 0 ? value : 1;
}
[[nodiscard]] std::array<std::byte, kChunkHeaderBytes>
serializeChunkHeader(const platform::FileIdentity& identity) {
    std::array<std::byte, kChunkHeaderBytes> out{};
    for (std::size_t i = 0; i < kChunkMagic.size(); ++i)
        out[i] = static_cast<std::byte>(kChunkMagic[i]);
    putU32(out, 4, kFormatVersion);
    putU64(out, 8, identity.size);
    putU64(out, 16, static_cast<std::uint64_t>(identity.modifiedNanoseconds));
    putU64(out, 24, identity.fileIdHigh);
    putU64(out, 32, identity.fileIdLow);
    putU64(out, 40, kChunkBytes);
    return out;
}
// The identity a chunk file was written for, when the file is one this build can read.
[[nodiscard]] std::optional<EntryHeader> readChunkHeader(const platform::PositionalFile& file) {
    std::array<std::byte, kChunkHeaderBytes> head{};
    if (!file.readAt(0, head))
        return std::nullopt;
    for (std::size_t i = 0; i < kChunkMagic.size(); ++i)
        if (head[i] != static_cast<std::byte>(kChunkMagic[i]))
            return std::nullopt;
    if (getU32(head, 4) != kFormatVersion || getU64(head, 40) != kChunkBytes)
        return std::nullopt;
    EntryHeader header;
    header.sourceSize = getU64(head, 8);
    header.modifiedNanoseconds = static_cast<std::int64_t>(getU64(head, 16));
    header.fileIdHigh = getU64(head, 24);
    header.fileIdLow = getU64(head, 32);
    if (header.sourceSize == 0)
        return std::nullopt;
    return header;
}
// Every slot of a chunk file; nullopt when the file is too short to hold its table.
[[nodiscard]] std::optional<std::vector<std::uint64_t>>
readChunkSlots(const platform::PositionalFile& file, const ChunkLayout& layout) {
    std::vector<std::byte> table(static_cast<std::size_t>(layout.count * 8));
    if (!file.readAt(kChunkHeaderBytes, table))
        return std::nullopt;
    std::vector<std::uint64_t> slots(static_cast<std::size_t>(layout.count));
    for (std::size_t i = 0; i < slots.size(); ++i)
        slots[i] = getU64(table, i * 8);
    return slots;
}
[[nodiscard]] std::uint64_t localChunkBytes(const std::vector<std::uint64_t>& slots,
                                            const ChunkLayout& layout) {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < slots.size(); ++i)
        if (slots[i] != 0)
            bytes += layout.length(i);
    return bytes;
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

struct SourceMirror::Hub final {
    // Shared by every chunk file call into the mirror; exclusive only in the mirror's destructor.
    std::shared_mutex mutex;
    SourceMirror* mirror = nullptr;
};

// One source's chunk file. Reads and writes are positional, so any number of threads fill and
// read at once; no lock is held across file I/O or hashing. A chunk's bytes are written before
// its slot, so a write that was cut short leaves the chunk missing, and a slot whose bytes did
// not reach the disk fails its hash when read and is cleared.
class SourceMirror::ChunkFile final : public platform::SourceChunkStore {
  public:
    ChunkFile(std::shared_ptr<Hub> hub, std::string indexKey, const EntryHeader& identity,
              std::unique_ptr<platform::PositionalFile> file,
              const std::vector<std::uint64_t>& slots, const bool network)
        : hub_(std::move(hub)), indexKey_(std::move(indexKey)), identity_(identity),
          layout_(identity.sourceSize), file_(std::move(file)),
          slots_(std::make_unique<std::atomic<std::uint64_t>[]>(slots.size())), network_(network) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            slots_[i].store(slots[i], std::memory_order_relaxed);
    }

    [[nodiscard]] bool read(const std::uint64_t offset, const std::span<std::byte> bytes) override {
        if (retired() || !wholeChunks(offset, bytes.size()))
            return false;
        std::uint64_t adopted = 0;
        std::size_t done = 0;
        for (auto index = offset / kChunkBytes; done < bytes.size(); ++index) {
            const auto length = static_cast<std::size_t>(layout_.length(index));
            auto slot = slots_[index].load(std::memory_order_acquire);
            if (slot == 0) {
                // Another process sharing this cache folder may have kept the chunk since.
                std::array<std::byte, 8> stored{};
                if (!file_->readAt(kChunkHeaderBytes + index * 8, stored) ||
                    (slot = getU64(stored, 0)) == 0)
                    return false;
                adopted += length;
            }
            const auto chunk = bytes.subspan(done, length);
            if (!file_->readAt(layout_.dataOffset + index * kChunkBytes, chunk) ||
                chunkHash(chunk) != slot) {
                clearSlot(index, slot, adopted != 0 ? 0 : length);
                return false;
            }
            if (adopted != 0)
                slots_[index].store(slot, std::memory_order_release);
            done += length;
        }
        withMirror([&](SourceMirror& mirror) {
            if (adopted != 0)
                mirror.chunksGrew(indexKey_, layout_.wholeBytes(), adopted, false, network_);
            mirror.chunksServed(indexKey_, bytes.size());
        });
        return true;
    }

    void keep(const std::uint64_t offset, const std::span<const std::byte> bytes) override {
        if (retired() || !wholeChunks(offset, bytes.size()))
            return;
        std::size_t done = 0;
        for (auto index = offset / kChunkBytes; done < bytes.size(); ++index) {
            const auto length = static_cast<std::size_t>(layout_.length(index));
            const auto chunk = bytes.subspan(done, length);
            done += length;
            if (slots_[index].load(std::memory_order_acquire) != 0)
                continue;
            bool room = true;
            withMirror([&](SourceMirror& mirror) {
                room = mirror.diskHasRoomFor(length);
                if (!room)
                    mirror.chunksRefusedForDiskSpace();
            });
            if (!room)
                return;
            const auto slot = chunkHash(chunk);
            std::array<std::byte, 8> stored{};
            putU64(stored, 0, slot);
            if (slot == 0 || !file_->writeAt(layout_.dataOffset + index * kChunkBytes, chunk) ||
                !file_->writeAt(kChunkHeaderBytes + index * 8, stored))
                return;
            std::uint64_t missing = 0;
            if (!slots_[index].compare_exchange_strong(missing, slot, std::memory_order_acq_rel))
                continue; // another thread kept the same chunk
            withMirror([&](SourceMirror& mirror) {
                mirror.chunksGrew(indexKey_, layout_.wholeBytes(), length, true, network_);
            });
            if (retired())
                return;
        }
    }

    [[nodiscard]] bool underPressure() const override {
        bool pressure = false;
        withMirror([&](SourceMirror& mirror) {
            pressure = mirror.underPressure() || !mirror.diskHasRoomFor(kChunkBytes);
        });
        return pressure;
    }

    void retire() noexcept { retired_.store(true, std::memory_order_release); }
    [[nodiscard]] bool retired() const noexcept {
        return retired_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool serves(const platform::FileIdentity& identity) const noexcept {
        return !retired() && identity_.matches(identity);
    }

  private:
    // The snapshot asks for and keeps whole chunks at chunk boundaries; nothing else is served.
    [[nodiscard]] bool wholeChunks(const std::uint64_t offset, const std::uint64_t size) const {
        return size != 0 && offset % kChunkBytes == 0 && offset < layout_.sourceSize &&
               size <= layout_.sourceSize - offset &&
               (size % kChunkBytes == 0 || offset + size == layout_.sourceSize);
    }
    template <typename Call> void withMirror(const Call& call) const {
        const std::shared_lock lock(hub_->mutex);
        if (hub_->mirror != nullptr)
            call(*hub_->mirror);
    }
    // A chunk whose bytes do not match its slot: it is missing again, here and on disk, and the
    // caller reads it from the source. `counted` is what the index had counted for it.
    void clearSlot(const std::uint64_t index, std::uint64_t slot, const std::uint64_t counted) {
        slots_[index].compare_exchange_strong(slot, 0, std::memory_order_acq_rel);
        const std::array<std::byte, 8> empty{};
        static_cast<void>(file_->writeAt(kChunkHeaderBytes + index * 8, empty));
        withMirror([&](SourceMirror& mirror) { mirror.chunksCorrupt(indexKey_, counted); });
    }

    const std::shared_ptr<Hub> hub_;
    const std::string indexKey_;
    const EntryHeader identity_;
    const ChunkLayout layout_;
    const std::unique_ptr<platform::PositionalFile> file_;
    const std::unique_ptr<std::atomic<std::uint64_t>[]> slots_;
    const bool network_;
    std::atomic<bool> retired_{false};
};

SourceMirror::SourceMirror(SourceMirrorConfig config, runtime::MemoryBudgetLedger& ledger)
    : ledger_(ledger), hub_(std::make_shared<Hub>()), root_(std::move(config.rootDirectory)), maxEntryCount_(config.maxEntryCount),
      freeSpaceReserve_(config.freeSpaceReserveBytes != 0 ? config.freeSpaceReserveBytes
                                                          : kSourceMirrorFreeSpaceReserveBytes),
      enabled_(config.enabled), writeQueueByteCapacity_(config.writeQueueByteCapacity != 0
                                                            ? config.writeQueueByteCapacity
                                                            : kSourceMirrorWriteQueueByteCapacity),
      byteBudget_(config.byteBudget != 0 ? config.byteBudget : kDefaultSourceMirrorCapBytes),
      policy_(std::move(config.policy)) {
    hub_->mirror = this;
    // The verified source identities persist beside the mirror, read and written on its thread.
    if (!root_.empty())
        platform::verifiedSourceDigests().persistTo(verifiedSourcesPath());
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
    {
        // Chunk files that outlive the mirror hold and keep nothing from here on. A call that is
        // inside the mirror finishes first.
        const std::unique_lock lock(hub_->mutex);
        hub_->mirror = nullptr;
    }
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [indexKey, open] : openChunks_)
            if (const auto live = open.lock())
                live->retire();
        openChunks_.clear();
    }
    ledger_.unregisterCache(this);
    stopping_.store(true);
    {
        const std::lock_guard lock(queueMutex_);
    }
    queueCv_.notify_all();
    if (writer_.joinable())
        writer_.join();
    if (!root_.empty() && platform::verifiedSourceDigests().persistedFile() == verifiedSourcesPath())
        platform::verifiedSourceDigests().persistTo({});
}

std::filesystem::path SourceMirror::entryPath(const std::string& key) const {
    if (isChunkKey(key))
        return root_ / "sources" / key.substr(0, 2) / (key.substr(0, 64) + std::string(kChunkSuffix));
    return root_ / "sources" / key.substr(0, 2) / (key + std::string(kEntrySuffix));
}
std::filesystem::path SourceMirror::chunkPathForTest(const std::filesystem::path& source) const {
    return entryPath(sourceKey(source) + kChunkKeyMark);
}
std::uint64_t SourceMirror::chunkDataOffsetForTest(const std::uint64_t sourceSize,
                                                   const std::uint64_t index) {
    return ChunkLayout(sourceSize).dataOffset + index * kChunkBytes;
}

std::shared_ptr<platform::SourceChunkStore>
SourceMirror::chunks(const std::filesystem::path& source, const platform::FileIdentity& identity) {
    if (!enabled() || root_.empty() || identity.size == 0)
        return nullptr;
    const auto key = sourceKey(source);
    if (key.empty())
        return nullptr;
    const auto indexKey = key + kChunkKeyMark;
    const ChunkLayout layout(identity.size);
    {
        const std::lock_guard lock(mutex_);
        // A source that could never fit is not copied piece by piece only to be evicted.
        if (layout.wholeBytes() > byteBudget_) {
            counters_.refused();
            return nullptr;
        }
        if (const auto open = openChunks_.find(indexKey); open != openChunks_.end()) {
            if (auto live = open->second.lock(); live && live->serves(identity))
                return live;
            retireOpenChunks(indexKey);
        }
    }
    // Local file I/O on the calling worker, outside the lock.
    const auto path = entryPath(indexKey);
    EntryHeader header;
    header.sourceSize = identity.size;
    header.modifiedNanoseconds = identity.modifiedNanoseconds;
    header.fileIdHigh = identity.fileIdHigh;
    header.fileIdLow = identity.fileIdLow;
    std::optional<std::vector<std::uint64_t>> slots;
    auto file = platform::PositionalFile::open(path, false);
    if (file) {
        const auto stored = readChunkHeader(*file);
        const bool current = stored && stored->matches(identity);
        if (current)
            slots = readChunkSlots(*file, layout);
        if (!slots) {
            // The source changed since its ranges were kept (or the file is not one this build
            // wrote): its chunk file is its own to purge, nothing else.
            file.reset();
            removeQuietly(path);
            dropEntry(indexKey);
            const std::lock_guard lock(mutex_);
            ++(stored && !current ? statistics_.staleDropped : statistics_.corruptDropped);
        }
    }
    if (!file) {
        if (!diskHasRoomFor(layout.dataOffset + kChunkBytes)) {
            chunksRefusedForDiskSpace();
            return nullptr;
        }
        file = platform::PositionalFile::open(path, true);
        const auto head = serializeChunkHeader(identity);
        const std::vector<std::byte> table(static_cast<std::size_t>(layout.count * 8));
        if (!file || !file->writeAt(0, head) || !file->writeAt(kChunkHeaderBytes, table)) {
            file.reset();
            removeQuietly(path);
            return nullptr;
        }
        slots.emplace(static_cast<std::size_t>(layout.count), 0);
    }
    const bool network =
        platform::storageClass(source, identity.volume) == platform::StorageClass::Network;
    auto result = std::make_shared<ChunkFile>(hub_, indexKey, header, std::move(file), *slots,
                                              network);
    const auto bytes = layout.dataOffset + localChunkBytes(*slots, layout);
    std::vector<std::string> victims;
    {
        const std::lock_guard lock(mutex_);
        if (const auto open = openChunks_.find(indexKey); open != openChunks_.end())
            if (auto live = open->second.lock(); live && live->serves(identity))
                return live; // another worker opened it meanwhile: one object per source
        openChunks_[indexKey] = result;
        auto& entry = index_[indexKey];
        storedBytes_ -= std::min(storedBytes_, entry.bytes);
        entry = IndexEntry{bytes, nowNanos(), layout.wholeBytes()};
        storedBytes_ += bytes;
        ++indexChanges_;
        evict(victims);
    }
    removeFiles(victims);
    return result;
}

void SourceMirror::retireOpenChunks(const std::string& indexKey) {
    if (const auto open = openChunks_.find(indexKey); open != openChunks_.end()) {
        if (const auto live = open->second.lock())
            live->retire();
        openChunks_.erase(open);
    }
}
void SourceMirror::chunksGrew(const std::string& indexKey, const std::uint64_t wholeBytes,
                              const std::uint64_t bytes, const bool kept, const bool network) {
    std::vector<std::string> victims;
    {
        const std::lock_guard lock(mutex_);
        if (kept) {
            // One range read from the source counts as one read of it, like a whole file's.
            ++(network ? statistics_.networkReads : statistics_.otherSourceReads);
            (network ? statistics_.networkReadBytes : statistics_.otherSourceReadBytes) += bytes;
            ++statistics_.rangeWrites;
            counters_.store();
        }
        // An entry the index no longer has was evicted or purged: its file is gone and the
        // chunk file object is retired, so there is nothing to count.
        const auto found = index_.find(indexKey);
        if (found == index_.end())
            return;
        found->second.bytes = std::min(found->second.bytes + bytes, wholeBytes);
        found->second.lastUseNanos = nowNanos();
        storedBytes_ += bytes;
        ++indexChanges_;
        evict(victims);
    }
    removeFiles(victims);
}
void SourceMirror::chunksCorrupt(const std::string& indexKey, const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    ++statistics_.corruptDropped;
    if (const auto found = index_.find(indexKey); found != index_.end()) {
        const auto counted = std::min(found->second.bytes, bytes);
        found->second.bytes -= counted;
        storedBytes_ -= std::min(storedBytes_, counted);
        ++indexChanges_;
    }
}
void SourceMirror::chunksServed(const std::string& indexKey, const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    ++statistics_.rangeHits;
    statistics_.rangeHitBytes += bytes;
    ++statistics_.diskHits;
    statistics_.diskHitBytes += bytes;
    counters_.hit();
    if (const auto found = index_.find(indexKey); found != index_.end())
        found->second.lastUseNanos = nowNanos();
}
void SourceMirror::chunksRefusedForDiskSpace() {
    const std::lock_guard lock(mutex_);
    ++statistics_.refusedForDiskSpace;
    counters_.refused();
}
std::filesystem::path SourceMirror::indexPath() const { return root_ / "sources" / "index.v2"; }
std::filesystem::path SourceMirror::verifiedSourcesPath() const {
    return root_ / "verified-sources.v1";
}
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
    platform::recordSourceStoreHit(result.bytes.size());
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
        // returned): the source it belonged to is simply read once more later. A write in flight
        // cannot be dropped, and a source larger than the whole capacity is taken only while the
        // queue holds nothing at all, so the queue never keeps more than its capacity or one
        // such source alive.
        const auto capacity = writeQueueByteCapacity_.load(std::memory_order_relaxed);
        if (bytes.size() <= capacity)
            while (!queue_.empty() && queuedBytes_ + bytes.size() > capacity) {
                queuedBytes_ -= queue_.front().bytes.size();
                queue_.pop_front();
                ++droppedWrites_;
                counters_.refused();
            }
        if (queuedBytes_ != 0 && queuedBytes_ + bytes.size() > capacity) {
            ++droppedWrites_;
            counters_.refused();
            return;
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
bool SourceMirror::keeps(const std::filesystem::path& source,
                         const platform::FileIdentity& identity,
                         const LocalCopyOverride asset) const {
    return enabled() && !root_.empty() && keepsLocalCopy(source, identity.volume, asset);
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
    if (index_.contains(key))
        return LocalCopyState::Cached;
    if (const auto ranges = index_.find(key + kChunkKeyMark); ranges != index_.end())
        return ranges->second.bytes >= ranges->second.wholeBytes ? LocalCopyState::Cached
                                                                 : LocalCopyState::Partial;
    return LocalCopyState::NotCached;
}

void SourceMirror::forget(const std::filesystem::path& source) {
    const auto key = sourceKey(source);
    if (key.empty() || root_.empty())
        return;
    for (const auto& indexKey : {key, key + kChunkKeyMark}) {
        dropEntry(indexKey);
        removeQuietly(entryPath(indexKey));
    }
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
        for (const auto& [indexKey, open] : openChunks_)
            if (const auto live = open.lock())
                live->retire();
        openChunks_.clear();
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
        result.diskPressure = diskPressure_.load(std::memory_order_relaxed);
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
    retireOpenChunks(key);
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
    for (const auto& key : victims) {
        index_.erase(key);
        retireOpenChunks(key);
    }
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
                if (row >> key >> lastUse && (isKey(key) || isChunkKey(key)))
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
            } else if (name.ends_with(kChunkSuffix) &&
                       isKey(name.substr(0, name.size() - kChunkSuffix.size()))) {
                // What a chunk file holds is in its own slot table (it is sparse: its length
                // says nothing). One that cannot be read is removed.
                const auto key =
                    name.substr(0, name.size() - kChunkSuffix.size()) + kChunkKeyMark;
                const auto file = platform::PositionalFile::open(files->path(), false);
                const auto header = file ? readChunkHeader(*file) : std::nullopt;
                const auto slots =
                    header ? readChunkSlots(*file, ChunkLayout(header->sourceSize)) : std::nullopt;
                if (!slots) {
                    if (file)
                        removeQuietly(files->path());
                    continue;
                }
                const ChunkLayout layout(header->sourceSize);
                const auto known = used.find(key);
                found.emplace(key, IndexEntry{layout.dataOffset + localChunkBytes(*slots, layout),
                                              known != used.end() ? known->second : 0,
                                              layout.wholeBytes()});
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
    // First, and small: a lookup that arrives before it has been read waits for it briefly.
    const bool persistsIdentities =
        !root_.empty() && platform::verifiedSourceDigests().persistedFile() == verifiedSourcesPath();
    if (persistsIdentities)
        platform::verifiedSourceDigests().load();
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
            if (persistsIdentities)
                platform::verifiedSourceDigests().save();
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
    if (persistsIdentities)
        platform::verifiedSourceDigests().save();
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

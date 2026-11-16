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
// The capacity and free space of the volume that holds `root` (or its nearest existing ancestor);
// nothing when the file system does not report them.
[[nodiscard]] std::optional<SourceMirrorDriveSpace> volumeSpace(const std::filesystem::path& root) {
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
        return std::nullopt;
    return SourceMirrorDriveSpace{space.capacity, space.available};
}
// The id a departed key is remembered by: the first 64 bits of its hash, complemented for a
// chunk file's key so the two kinds of entry of one source stay apart.
[[nodiscard]] std::uint64_t departedId(const std::string_view key) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 16 && i < key.size(); ++i) {
        const auto digit = key[i];
        value = (value << 4U) | static_cast<std::uint64_t>(digit <= '9' ? digit - '0'
                                                                        : digit - 'a' + 10);
    }
    return key.size() == 65 ? ~value : value;
}
[[nodiscard]] std::uint64_t saturatingSubtract(const std::uint64_t from,
                                               const std::uint64_t amount) noexcept {
    return from > amount ? from - amount : 0;
}
// How many of the oldest entries one pass over the index lists in order.
constexpr std::size_t kOldestBatch = 1024;
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
            // The room is reserved before the chunk is written: a refused chunk is not written
            // at all, and the read it belongs to simply goes on from the source.
            std::optional<Reservation> room;
            withMirror([&](SourceMirror& mirror) { room = mirror.admit(indexKey_, length); });
            if (!room)
                return;
            const auto unused = [&] {
                withMirror([&](SourceMirror& mirror) { mirror.release(*room); });
            };
            const auto slot = chunkHash(chunk);
            std::array<std::byte, 8> stored{};
            putU64(stored, 0, slot);
            if (retired() || slot == 0 ||
                !file_->writeAt(layout_.dataOffset + index * kChunkBytes, chunk) ||
                !file_->writeAt(kChunkHeaderBytes + index * 8, stored)) {
                unused();
                return;
            }
            std::uint64_t missing = 0;
            if (!slots_[index].compare_exchange_strong(missing, slot, std::memory_order_acq_rel)) {
                unused();
                continue; // another thread kept the same chunk
            }
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
            pressure = mirror.underPressure() || !mirror.wouldAdmit(indexKey_, kChunkBytes);
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
      clock_(std::move(config.clock)), driveSpace_(std::move(config.driveSpace)),
      enabled_(config.enabled), writeQueueByteCapacity_(config.writeQueueByteCapacity != 0
                                                            ? config.writeQueueByteCapacity
                                                            : kSourceMirrorWriteQueueByteCapacity),
      headroom_(config.headroomBytes),
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
        // The write queue is live work (#732): bytes on their way to the disk, not a cache that
        // can be dropped and read again for free. Its ceiling is fixed, so the ledger neither
        // divides it among the reusable pools nor asks it to give anything back; the budget it
        // is told is always that ceiling and nothing happens here.
        ledger_.registerCache(
            this, static_cast<std::size_t>(writeQueueByteCapacity_),
            [this] {
                const std::lock_guard lock(queueMutex_);
                return static_cast<std::size_t>(queuedBytes_);
            },
            [](const std::size_t) {}, runtime::MemoryBudgetOwnerKind::LiveWork);
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
    beginShutdown();
    {
        // A read still waiting for room in keep() leaves before the queue it waits on is gone.
        std::unique_lock lock(queueMutex_);
        roomCv_.wait(lock, [this] { return waitingKeepers_ == 0; });
    }
    if (writer_.joinable())
        writer_.join();
    if (!root_.empty() && platform::verifiedSourceDigests().persistedFile() == verifiedSourcesPath())
        platform::verifiedSourceDigests().persistTo({});
}

void SourceMirror::beginShutdown() noexcept {
    stopping_.store(true);
    {
        const std::lock_guard lock(queueMutex_);
    }
    queueCv_.notify_all();
    roomCv_.notify_all();
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
    measureDrive(false);
    {
        const std::lock_guard lock(mutex_);
        // A source larger than everything the mirror may hold now is not copied piece by piece
        // only to be refused or evicted.
        if (layout.wholeBytes() > storedBytes_ + roomLocked()) {
            ++statistics_.refusedWrites;
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
    // A new chunk file starts with its header and slot table: that room is reserved first.
    std::optional<Reservation> started;
    if (!file) {
        started = admit(indexKey, layout.dataOffset);
        if (!started)
            return nullptr;
        file = platform::PositionalFile::open(path, true);
        const auto head = serializeChunkHeader(identity);
        const std::vector<std::byte> table(static_cast<std::size_t>(layout.count * 8));
        if (!file || !file->writeAt(0, head) || !file->writeAt(kChunkHeaderBytes, table)) {
            file.reset();
            removeQuietly(path);
            release(*started);
            return nullptr;
        }
        slots.emplace(static_cast<std::size_t>(layout.count), 0);
    }
    const bool network =
        platform::storageClass(source, identity.volume) == platform::StorageClass::Network;
    auto result = std::make_shared<ChunkFile>(hub_, indexKey, header, std::move(file), *slots,
                                              network);
    const auto bytes = layout.dataOffset + localChunkBytes(*slots, layout);
    {
        const std::lock_guard lock(mutex_);
        if (started) {
            reservedBytes_ = saturatingSubtract(reservedBytes_, started->bytes);
            reservedEntries_ = saturatingSubtract(reservedEntries_, started->entry ? 1 : 0);
        }
        if (const auto open = openChunks_.find(indexKey); open != openChunks_.end())
            if (auto live = open->second.lock(); live && live->serves(identity))
                return live; // another worker opened it meanwhile: one object per source
        openChunks_[indexKey] = result;
        auto& entry = index_[indexKey];
        storedBytes_ -= std::min(storedBytes_, entry.bytes);
        entry = IndexEntry{bytes, now(), layout.wholeBytes()};
        storedBytes_ += bytes;
        // Only a file this call started took new room on the volume; one that was already there
        // (another process's, or one this mirror had lost track of) is in the measurement.
        if (started)
            correctFreeLocked(started->bytes, 0);
        departed_.erase(departedId(indexKey));
        ++indexChanges_;
    }
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
    const std::lock_guard lock(mutex_);
    if (kept) {
        // One range read from the source counts as one read of it, like a whole file's.
        ++(network ? statistics_.networkReads : statistics_.otherSourceReads);
        (network ? statistics_.networkReadBytes : statistics_.otherSourceReadBytes) += bytes;
        ++statistics_.rangeWrites;
        counters_.store();
        // The chunk was written under a reservation: it is on the volume now.
        reservedBytes_ = saturatingSubtract(reservedBytes_, bytes);
        correctFreeLocked(bytes, 0);
    }
    // An entry the index no longer has was evicted or purged: its file is gone and the
    // chunk file object is retired, so there is nothing to count.
    const auto found = index_.find(indexKey);
    if (found == index_.end())
        return;
    found->second.bytes = std::min(found->second.bytes + bytes, wholeBytes);
    found->second.lastUseNanos = now();
    storedBytes_ += bytes;
    ++indexChanges_;
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
        found->second.lastUseNanos = now();
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
    // Asked before any lock of the mirror is taken: the ledger's callbacks take the queue's lock.
    bool filling = ledger_.previewFillActive();
    {
        std::unique_lock lock(queueMutex_);
        const auto queued = [&] {
            return std::ranges::any_of(
                queue_, [&](const PendingWrite& pending) { return pending.key == key; });
        };
        if (stopping_.load() || queued())
            return;
        const auto capacity = writeQueueByteCapacity_;
        // A source larger than the whole capacity is taken only while the queue holds nothing
        // at all, so the queue never keeps more than its capacity or one such source alive.
        const auto fits = [&] {
            return bytes.size() <= capacity ? queuedBytes_ + bytes.size() <= capacity
                                            : queuedBytes_ == 0;
        };
        if (filling && !fits()) {
            // A RAM Preview fill has no frame deadline and reads faster than an idle-priority
            // writer writes: the read waits here for the writer instead of losing a write, so
            // one pass leaves everything it read on disk. The fill's end is the ledger's to
            // say, and it is asked with no lock of the mirror held.
            const auto purges = queuePurges_;
            ++waitingKeepers_;
            while (filling && !fits() && !stopping_.load() && queuePurges_ == purges) {
                roomCv_.wait_for(lock, std::chrono::milliseconds(50));
                if (fits() || stopping_.load() || queuePurges_ != purges)
                    break;
                lock.unlock();
                filling = ledger_.previewFillActive();
                lock.lock();
            }
            --waitingKeepers_;
            // Shutdown and "Purge media cache" abandon the write like any pending one; another
            // read may have queued the same source meanwhile. A write abandoned at shutdown is
            // counted as dropped; a purge resets the counts with everything else.
            if (stopping_.load()) {
                ++droppedWrites_;
                counters_.refused();
                roomCv_.notify_all(); // the destructor waits for the last waiting read
                return;
            }
            if (queuePurges_ != purges || queued())
                return;
        }
        // Outside a fill a full queue drops its oldest pending write (never the frame's own
        // read, which already returned): the source it belonged to is simply read once more
        // later. A write in flight cannot be dropped.
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
        ++queuePurges_;
        droppedWrites_ = 0; // the counts start again with a purge, this one with the others
    }
    roomCv_.notify_all();
    {
        const std::lock_guard lock(mutex_);
        ++generation_;
        for (const auto& [indexKey, open] : openChunks_)
            if (const auto live = open.lock())
                live->retire();
        openChunks_.clear();
        index_.clear();
        // The files go below: their room is free again.
        correctFreeLocked(0, storedBytes_);
        storedBytes_ = 0;
        indexChanges_ = 0;
        departed_.clear();
        oldest_.clear();
        oldestNext_ = 0;
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
    // After a shutdown began there is nothing left to apply: the call returns when the writer
    // has left the index current and stopped.
    idleCv_.wait(lock, [this] {
        return stopping_.load() ? writerStopped_
                                : loaded_ && queue_.empty() && inFlight_ == 0 && !indexRequested_;
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
    return queuedBytes_ * 2 > writeQueueByteCapacity_;
}

SourceMirrorStatistics SourceMirror::statistics() const {
    SourceMirrorStatistics result;
    {
        const std::lock_guard lock(mutex_);
        result = statistics_;
        result.entryCount = index_.size();
        result.storedBytes = storedBytes_;
        if (drive_.taken) {
            result.driveMeasured = drive_.measured;
            result.capacityBytes = drive_.measured ? drive_.capacityBytes : 0;
            result.freeBytes = drive_.measured ? drive_.freeBytes : 0;
            result.headroomBytes = drive_.measured ? keepFreeLocked() : 0;
            result.availableBytes = storedBytes_ + roomLocked();
        }
        result.indexLoaded = indexLoaded_;
        result.diskPressure = diskPressure_.load(std::memory_order_relaxed);
    }
    const std::lock_guard lock(queueMutex_);
    result.droppedWrites = droppedWrites_;
    result.pendingWrites = queue_.size() + inFlight_;
    result.pendingWriteBytes = queuedBytes_;
    result.waitingWrites = waitingKeepers_;
    result.peakPendingWriteBytes = peakQueuedBytes_;
    return result;
}

void SourceMirror::setHeadroom(const std::uint64_t bytes) {
    {
        const std::lock_guard lock(mutex_);
        if (headroom_ == bytes)
            return;
        headroom_ = bytes;
    }
    // The writer measures the volume and gives space back; nothing is removed on this thread.
    {
        const std::lock_guard lock(queueMutex_);
        indexRequested_ = true;
    }
    queueCv_.notify_all();
}
std::uint64_t SourceMirror::headroom() const {
    const std::lock_guard lock(mutex_);
    return headroom_;
}

void SourceMirror::noteUse(const std::string& key, const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    auto [entry, inserted] = index_.try_emplace(key, IndexEntry{bytes, now()});
    if (inserted)
        storedBytes_ += bytes;
    else
        entry->second.lastUseNanos = now();
    ++indexChanges_;
}
void SourceMirror::dropEntry(const std::string& key) {
    const std::lock_guard lock(mutex_);
    retireOpenChunks(key);
    if (const auto found = index_.find(key); found != index_.end()) {
        storedBytes_ -= std::min(storedBytes_, found->second.bytes);
        // The caller removes the file: its room is free again.
        correctFreeLocked(0, found->second.bytes);
        index_.erase(found);
        ++indexChanges_;
    }
}

std::uint64_t SourceMirror::keepFreeLocked() const {
    const auto headroom =
        headroom_ != 0 ? headroom_ : defaultSourceMirrorHeadroom(drive_.capacityBytes);
    return std::max(headroom, freeSpaceReserve_);
}
std::uint64_t SourceMirror::roomLocked() const {
    // Not measured yet counts as no room: every path that writes measures first.
    if (!drive_.taken)
        return 0;
    if (!drive_.measured)
        return saturatingSubtract(kSourceMirrorUnmeasuredByteLimit, storedBytes_ + reservedBytes_);
    return saturatingSubtract(drive_.freeBytes, keepFreeLocked() + reservedBytes_);
}
std::uint64_t SourceMirror::deficitLocked() const {
    if (!drive_.taken)
        return 0;
    if (!drive_.measured)
        return saturatingSubtract(storedBytes_, kSourceMirrorUnmeasuredByteLimit);
    return std::min(storedBytes_, saturatingSubtract(keepFreeLocked(), drive_.freeBytes));
}
void SourceMirror::correctFreeLocked(const std::uint64_t added, const std::uint64_t removed) {
    ++spaceChanges_;
    if (!drive_.taken || !drive_.measured)
        return;
    drive_.freeBytes = std::min(saturatingSubtract(drive_.freeBytes, added) + removed,
                                drive_.capacityBytes);
}

void SourceMirror::measureDrive(const bool fresh) {
    const auto time = now();
    std::uint64_t changes = 0, storedBefore = 0;
    {
        const std::lock_guard lock(mutex_);
        if (!fresh && drive_.taken &&
            time - drive_.atNanos < std::chrono::nanoseconds(kSourceMirrorDriveSampleLife).count())
            return;
        changes = spaceChanges_;
        storedBefore = storedBytes_;
    }
    // The volume is asked with no lock held, so this mirror may store or remove something
    // meanwhile, and the answer cannot say whether it saw that. Such an answer is asked for
    // again; when the mirror keeps changing, what it stored meanwhile is taken as not seen,
    // which can only understate the free space until the next measurement.
    for (int attempt = 0;; ++attempt) {
        const auto space = driveSpace();
        const std::lock_guard lock(mutex_);
        if (space && (spaceChanges_ != changes || storedBytes_ != storedBefore) && attempt < 3) {
            changes = spaceChanges_;
            storedBefore = storedBytes_;
            continue;
        }
        drive_.taken = true;
        drive_.measured = space.has_value();
        drive_.atNanos = time;
        if (space) {
            drive_.capacityBytes = space->capacityBytes;
            drive_.freeBytes = saturatingSubtract(space->freeBytes,
                                                  saturatingSubtract(storedBytes_, storedBefore));
        }
        return;
    }
}

template <typename Visit>
void SourceMirror::forEachOldestLocked(const std::string& except, const Visit& visit) {
    const auto current = [this](const Aged& aged) -> const IndexEntry* {
        const auto found = index_.find(aged.key);
        return found != index_.end() && found->second.lastUseNanos == aged.usedNanos
                   ? &found->second
                   : nullptr;
    };
    while (oldestNext_ < oldest_.size() && current(oldest_[oldestNext_]) == nullptr)
        ++oldestNext_;
    if (oldestNext_ >= oldest_.size()) {
        // One pass over the index lists its oldest entries in order. Everything else is newer
        // and stays newer, because a use only ever moves an entry to now.
        oldest_.clear();
        oldestNext_ = 0;
        std::vector<std::pair<std::int64_t, const std::string*>> order;
        order.reserve(index_.size());
        for (const auto& [key, entry] : index_)
            order.emplace_back(entry.lastUseNanos, &key);
        const auto byAge = [](const auto& left, const auto& right) {
            return left.first != right.first ? left.first < right.first
                                             : *left.second < *right.second;
        };
        const auto listed = std::min(order.size(), kOldestBatch);
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(listed),
                          order.end(), byAge);
        oldest_.reserve(listed);
        for (std::size_t i = 0; i < listed; ++i)
            oldest_.push_back({order[i].first, *order[i].second});
    }
    for (auto at = oldestNext_; at < oldest_.size(); ++at) {
        const auto* const entry = current(oldest_[at]);
        if (entry == nullptr || oldest_[at].key == except)
            continue;
        if (!visit(oldest_[at].key, *entry))
            return;
    }
    if (oldest_.empty())
        return;
    // More is wanted than the list still holds (rare: a write or a give-back that needs the room
    // of many entries at once): the rest of the index, in order, after the last one listed.
    const Aged last = oldest_.back();
    std::vector<std::pair<std::int64_t, const std::string*>> rest;
    for (const auto& [key, entry] : index_)
        if (entry.lastUseNanos > last.usedNanos ||
            (entry.lastUseNanos == last.usedNanos && key > last.key))
            rest.emplace_back(entry.lastUseNanos, &key);
    std::ranges::sort(rest, [](const auto& left, const auto& right) {
        return left.first != right.first ? left.first < right.first : *left.second < *right.second;
    });
    for (const auto& [used, key] : rest)
        if (*key != except && !visit(*key, index_.at(*key)))
            return;
}

void SourceMirror::rememberDepartedLocked(const std::string& key,
                                          const std::int64_t lastAskedNanos) {
    if (departed_.size() >= maxEntryCount_ && !departed_.contains(departedId(key))) {
        // Full: the older half goes. A source forgotten this way is treated as new work.
        std::vector<std::int64_t> times;
        times.reserve(departed_.size());
        for (const auto& [id, time] : departed_)
            times.push_back(time);
        const auto middle = times.begin() + static_cast<std::ptrdiff_t>(times.size() / 2);
        std::nth_element(times.begin(), middle, times.end());
        std::erase_if(departed_, [&](const auto& record) { return record.second <= *middle; });
    }
    departed_[departedId(key)] = lastAskedNanos;
}

void SourceMirror::evictLocked(const std::string& key) {
    const auto found = index_.find(key);
    if (found == index_.end())
        return;
    storedBytes_ -= std::min(storedBytes_, found->second.bytes);
    // The caller removes the file once the lock is released: its room is free again.
    correctFreeLocked(0, found->second.bytes);
    counters_.released(found->second.bytes);
    ++statistics_.evictions;
    rememberDepartedLocked(key, found->second.lastUseNanos);
    index_.erase(found);
    retireOpenChunks(key);
    ++indexChanges_;
}

bool SourceMirror::planRoomLocked(const std::string& key, const std::uint64_t bytes,
                                  const bool entry, std::vector<std::string>& victims) {
    const auto room = roomLocked();
    const auto needBytes = saturatingSubtract(bytes, room);
    const std::uint64_t needEntries =
        entry && index_.size() + reservedEntries_ >= maxEntryCount_ ? 1 : 0;
    if (needBytes == 0 && needEntries == 0)
        return true;
    const auto time = now();
    const auto asked = departed_.find(departedId(key));
    const auto hold = std::chrono::nanoseconds(kSourceMirrorHoldWindow).count();
    std::vector<std::string> chosen;
    std::uint64_t freedBytes = 0;
    bool held = false;
    forEachOldestLocked(key, [&](const std::string& victim, const IndexEntry& aged) {
        if (freedBytes >= needBytes && chosen.size() >= needEntries)
            return false;
        // In use: used within the hold window, or used since this same source last found no
        // place. A loop larger than the space uses every entry between two requests for a
        // source that does not fit, so its entries stay however long a pass takes.
        if (time - aged.lastUseNanos < hold ||
            (asked != departed_.end() && aged.lastUseNanos > asked->second)) {
            held = true;
            return false;
        }
        chosen.push_back(victim);
        freedBytes += aged.bytes;
        return true;
    });
    if (held || freedBytes < needBytes || chosen.size() < needEntries)
        return false;
    victims.insert(victims.end(), chosen.begin(), chosen.end());
    return true;
}

void SourceMirror::giveBackLocked(std::vector<std::string>& victims) {
    const auto deficit = deficitLocked();
    const auto excess = saturatingSubtract(index_.size(), maxEntryCount_);
    if (deficit == 0 && excess == 0)
        return;
    std::vector<std::string> chosen;
    std::uint64_t freedBytes = 0;
    forEachOldestLocked({}, [&](const std::string& victim, const IndexEntry& aged) {
        if (freedBytes >= deficit && chosen.size() >= excess)
            return false;
        chosen.push_back(victim);
        freedBytes += aged.bytes;
        return true;
    });
    for (const auto& key : chosen)
        evictLocked(key);
    victims.insert(victims.end(), chosen.begin(), chosen.end());
}

std::optional<SourceMirror::Reservation> SourceMirror::admit(const std::string& key,
                                                             const std::uint64_t bytes) {
    measureDrive(false);
    std::vector<std::string> victims;
    std::optional<Reservation> reservation;
    {
        const std::lock_guard lock(mutex_);
        // Space other programs took from the headroom comes back first, in use or not.
        giveBackLocked(victims);
        const bool entry = !index_.contains(key);
        std::vector<std::string> chosen;
        if (planRoomLocked(key, bytes, entry, chosen)) {
            for (const auto& victim : chosen)
                evictLocked(victim);
            victims.insert(victims.end(), chosen.begin(), chosen.end());
            reservedBytes_ += bytes;
            reservedEntries_ += entry ? 1 : 0;
            reservation = Reservation{bytes, entry};
        } else {
            ++statistics_.refusedWrites;
            counters_.refused();
            rememberDepartedLocked(key, now());
        }
        diskPressure_.store(!reservation, std::memory_order_relaxed);
    }
    removeFiles(victims);
    return reservation;
}

bool SourceMirror::wouldAdmit(const std::string& key, const std::uint64_t bytes) {
    measureDrive(false);
    const std::lock_guard lock(mutex_);
    std::vector<std::string> chosen;
    if (planRoomLocked(key, bytes, !index_.contains(key), chosen))
        return true;
    // Asked, and found no place: remembered like a refused write, so that the entries in the way
    // count as idle from now on. Not counted as refused: nothing was to be written yet.
    rememberDepartedLocked(key, now());
    return false;
}

void SourceMirror::release(const Reservation& reservation) {
    const std::lock_guard lock(mutex_);
    reservedBytes_ = saturatingSubtract(reservedBytes_, reservation.bytes);
    reservedEntries_ = saturatingSubtract(reservedEntries_, reservation.entry ? 1 : 0);
}

void SourceMirror::maintain() {
    measureDrive(true);
    std::vector<std::string> victims;
    {
        const std::lock_guard lock(mutex_);
        giveBackLocked(victims);
    }
    removeFiles(victims);
}

void SourceMirror::removeFiles(const std::vector<std::string>& keys) const {
    for (const auto& key : keys)
        removeQuietly(entryPath(key));
}

std::int64_t SourceMirror::now() const {
    if (!clock_)
        return nowNanos();
    // An injected clock is made strictly increasing the same way the wall clock is.
    const auto time = clock_();
    auto previous = lastStamp_.load(std::memory_order_relaxed);
    std::int64_t next = 0;
    do {
        next = std::max(time, previous + 1);
    } while (!lastStamp_.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    return next;
}

std::optional<SourceMirrorDriveSpace> SourceMirror::driveSpace() const {
    if (driveSpace_)
        return driveSpace_();
    return volumeSpace(root_);
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
    // The room is reserved before anything is written; a refused write is counted there.
    const auto reservation = admit(write.key, total);
    if (!reservation)
        return;
    // Every way out below that does not reach the index gives the room back.
    bool stored = false;
    struct Unused final {
        SourceMirror& mirror;
        const Reservation& reservation;
        const bool& stored;
        ~Unused() {
            if (!stored)
                mirror.release(reservation);
        }
    } const unused{*this, *reservation, stored};
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
    bool stale = false;
    {
        const std::lock_guard lock(mutex_);
        if (generation_ != write.generation) {
            stale = true;
        } else {
            stored = true;
            reservedBytes_ = saturatingSubtract(reservedBytes_, reservation->bytes);
            reservedEntries_ = saturatingSubtract(reservedEntries_, reservation->entry ? 1 : 0);
            auto& entry = index_[write.key];
            storedBytes_ -= std::min(storedBytes_, entry.bytes);
            // The new file replaced the old one (if any) on the volume.
            correctFreeLocked(total, entry.bytes);
            entry = IndexEntry{total, now()};
            storedBytes_ += total;
            departed_.erase(departedId(write.key));
            ++indexChanges_;
            ++statistics_.writes;
            counters_.store();
        }
    }
    if (stale)
        removeQuietly(path);
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
    {
        const std::lock_guard lock(mutex_);
        for (auto& [key, entry] : found)
            if (index_.try_emplace(key, entry).second)
                storedBytes_ += entry.bytes;
        indexLoaded_ = true;
        // Entries came in with the times of earlier runs: the order is taken again.
        oldest_.clear();
        oldestNext_ = 0;
    }
    // The first measurement of the volume, and what the headroom is short of given back.
    maintain();
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
            roomCv_.notify_all();
        }
        if (queue_.empty() && (indexRequested_ || !woken)) {
            // Idle, or a flush or a new headroom asked for it: measure the volume, give back what
            // the headroom is short of, and bring the index file up to date.
            lock.unlock();
            maintain();
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
    for (const auto& pending : queue_) {
        queuedBytes_ -= std::min<std::uint64_t>(queuedBytes_, pending.bytes.size());
        ++droppedWrites_;
        counters_.refused();
    }
    queue_.clear();
    lock.unlock();
    roomCv_.notify_all();
    writeIndex();
    if (persistsIdentities)
        platform::verifiedSourceDigests().save();
    lock.lock();
    writerStopped_ = true;
    lock.unlock();
    idleCv_.notify_all();
}

} // namespace bloom::media::cache

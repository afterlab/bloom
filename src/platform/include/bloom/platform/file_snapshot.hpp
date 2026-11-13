#pragma once
#include <bloom/core/sha256.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <bloom/platform/source_identity.hpp>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace bloom::platform {
struct FileSnapshotStatistics final {
    std::uint64_t bytesRead = 0, bytesHashed = 0;
    std::uint64_t ioNanoseconds = 0, hashNanoseconds = 0;
};
// Disk quota includes captures in flight and unlinked files waiting for their last handle to close.
class FileSnapshotQuota final {
  public:
    FileSnapshotQuota(std::uint64_t bytes, std::size_t files) : ceiling_(bytes), files_(files) {}
    [[nodiscard]] std::uint64_t residentBytes() const;
    [[nodiscard]] std::size_t residentFiles() const;

  private:
    friend class ReadOnlyFileSnapshot;
    bool reserve(std::uint64_t bytes);
    void release(std::uint64_t bytes);
    mutable std::mutex mutex_;
    std::uint64_t ceiling_ = 0, resident_ = 0;
    std::size_t files_ = 0, count_ = 0;
};
// #718: where a progressive snapshot finds bytes of its source that are already local, and where
// it keeps the bytes it reads from the source (the local mirror's byte ranges of one file). Called
// from whichever thread is filling; an implementation is thread-safe.
class SourceChunkStore {
  public:
    SourceChunkStore() = default;
    SourceChunkStore(const SourceChunkStore&) = delete;
    SourceChunkStore& operator=(const SourceChunkStore&) = delete;
    virtual ~SourceChunkStore() = default;
    // Fills `bytes` with [offset, offset + bytes.size()) of the source when the store holds all
    // of that range; false, leaving `bytes` unspecified, otherwise. Never reads the source.
    [[nodiscard]] virtual bool read(std::uint64_t offset, std::span<std::byte> bytes) = 0;
    // Keeps a range that was just read from the source. May drop it (a full store).
    virtual void keep(std::uint64_t offset, std::span<const std::byte> bytes) = 0;
    // True while background filling should hold off (a write backlog, a short cache volume).
    [[nodiscard]] virtual bool underPressure() const = 0;
};
// What ensureRange() made local: [begin, end) holds the asked range and whatever was already
// local around it; `complete` once the whole source is local.
struct SnapshotRange final {
    std::uint64_t begin = 0, end = 0;
    bool complete = false;
    friend bool operator==(const SnapshotRange&, const SnapshotRange&) = default;
};
// Whether a snapshot's bytes have been hashed to its digest. An ordinary capture is Verified
// from the start. A progressive one is Pending until its fill completes and the hash of the
// whole local file equals the digest it was opened with, and Failed when it does not or the
// source changed while it was read.
enum class SnapshotVerification : unsigned char { Verified, Pending, Failed };
struct ProgressiveSnapshotRequest final {
    std::filesystem::path source, spoolDirectory;
    std::shared_ptr<FileSnapshotQuota> quota;
    std::uint64_t sourceLimit = 0;
    // The digest recorded for the source: the snapshot's digest() until proved otherwise.
    core::Sha256Digest claimed{};
    // Null reads every range from the source and keeps nothing beyond the snapshot itself.
    std::shared_ptr<SourceChunkStore> chunks;
    // False fills only what ensureRange() asks for (tests); true also fills the rest on an
    // idle-priority thread of the snapshot's own.
    bool backgroundFill = true;
};
// A host-owned anonymous regular file (on Windows: a private file nothing else can open, removed
// with its last handle). There is no public native-handle/path constructor: only
// capture() can establish the digest/length authority accepted by ProcessSupervisor. Capture and
// final release are blocking filesystem work; a UI host supplies its existing retirement owner.
class ReadOnlyFileSnapshot final {
  public:
    using Progress = std::function<void(std::uint64_t, std::uint64_t)>;
    [[nodiscard]] static ProcessResult<std::unique_ptr<ReadOnlyFileSnapshot>>
    capture(const std::filesystem::path& source, const std::filesystem::path& spoolDirectory,
            const std::shared_ptr<FileSnapshotQuota>& quota, std::uint64_t sourceLimit,
            const ProcessCancellation& cancel = {}, const Progress& progress = {},
            std::optional<core::Sha256Digest> expected = {},
            std::size_t hostByteBudget = std::numeric_limits<std::size_t>::max());
    // #718: a snapshot that is usable at once and fills by byte range: nothing is copied before
    // this returns. The file has the source's full length; a range is local once ensureRange()
    // returned for it. The digest is the request's claimed one until verification() says more.
    // The source stays open until the fill completes. Blocking filesystem work, like capture().
    [[nodiscard]] static ProcessResult<std::unique_ptr<ReadOnlyFileSnapshot>>
    captureProgressive(const ProgressiveSnapshotRequest& request);
    // The byte range granularity of a progressive snapshot.
    static constexpr std::uint64_t kProgressiveChunkBytes = 1024 * 1024;
    [[nodiscard]] bool progressive() const;
    // Makes [offset, offset + length) local now, ahead of the background fill, reading from the
    // chunk store or the source. Returns at once with the whole file for an ordinary snapshot or
    // a range that is already local. Thread-safe; blocking I/O; a failed read leaves the range
    // missing and can be asked again.
    [[nodiscard]] ProcessResult<SnapshotRange>
    ensureRange(std::uint64_t offset, std::uint64_t length,
                const ProcessCancellation& cancel = {}) const;
    [[nodiscard]] SnapshotVerification verification() const;
    // Brings the whole source in on the calling thread's behalf and waits for the hash: the
    // final verification(), or a failure when a read failed or `cancel` asked to stop.
    [[nodiscard]] ProcessResult<SnapshotVerification>
    waitVerified(const ProcessCancellation& cancel = {}, const Progress& progress = {}) const;
    // Bytes of the source that are local so far (size() for an ordinary snapshot).
    [[nodiscard]] std::uint64_t localBytes() const;
    ~ReadOnlyFileSnapshot();
    ReadOnlyFileSnapshot(const ReadOnlyFileSnapshot&) = delete;
    ReadOnlyFileSnapshot& operator=(const ReadOnlyFileSnapshot&) = delete;
    [[nodiscard]] const core::Sha256Digest& digest() const;
    [[nodiscard]] std::uint64_t size() const;
    [[nodiscard]] const FileSnapshotStatistics& statistics() const;
    // Observation of the original pathname that bracketed this capture, when it was stable across
    // the copy. A later identical observation means the original still holds these bytes.
    [[nodiscard]] const std::optional<FileIdentity>& sourceIdentity() const;

  private:
    friend class ProcessSupervisor;
    struct State;
    struct Progressive;
    explicit ReadOnlyFileSnapshot(std::unique_ptr<State> state);
    [[nodiscard]] int descriptor() const;
    // Windows: the read-only HANDLE to the private copy. Null elsewhere.
    [[nodiscard]] void* nativeHandle() const;
    std::unique_ptr<State> state_;
};
// Child-only typed view of the descriptor installed by launch(), never a serialized pathname.
// Admission checks read-only access, anonymous regular-file identity and exact bounded length.
class WorkerSnapshotInput final {
  public:
    [[nodiscard]] static ProcessResult<std::unique_ptr<WorkerSnapshotInput>>
    open(std::uint64_t size);
    ~WorkerSnapshotInput();
    WorkerSnapshotInput(const WorkerSnapshotInput&) = delete;
    WorkerSnapshotInput& operator=(const WorkerSnapshotInput&) = delete;
    [[nodiscard]] ProcessResult<std::size_t> read(std::uint64_t offset,
                                                  std::span<std::byte> bytes) const;
    [[nodiscard]] std::uint64_t size() const { return size_; }
    // #718: the worker's one gate for a progressive source, consulted by every read() before it
    // touches the file: it returns true once [offset, offset + length) is local (asking the host
    // for it when it has to), false when the range cannot be had. Null (the default) reads at
    // once. Set by the worker's main thread before provider code runs.
    using RangeGate = std::function<bool(std::uint64_t offset, std::uint64_t length)>;
    static void setRangeGate(RangeGate gate);

  private:
    WorkerSnapshotInput(std::uint64_t size, int descriptor)
        : size_(size), descriptor_(descriptor) {}
    // Windows: an owned HANDLE in place of a descriptor.
    WorkerSnapshotInput(std::uint64_t size, void* handle) : size_(size), handle_(handle) {}
    std::uint64_t size_ = 0;
    int descriptor_ = -1;
    [[maybe_unused]] void* handle_ = nullptr;
};
} // namespace bloom::platform

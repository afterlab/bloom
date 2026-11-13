#include <algorithm>
#include <array>
#include <atomic>
#include <bloom/platform/file_snapshot.hpp>
#include <bloom/platform/thread_priority.hpp>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <thread>
#include <utility>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <random>
#include <string>
#include <windows.h>
#include <winioctl.h>
#endif

namespace bloom::platform {
std::uint64_t FileSnapshotQuota::residentBytes() const {
    const std::lock_guard lock(mutex_);
    return resident_;
}
std::size_t FileSnapshotQuota::residentFiles() const {
    const std::lock_guard lock(mutex_);
    return count_;
}
bool FileSnapshotQuota::reserve(const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    if (count_ >= files_ || bytes > ceiling_ || resident_ > ceiling_ - bytes)
        return false;
    resident_ += bytes;
    ++count_;
    return true;
}
void FileSnapshotQuota::release(const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    resident_ -= bytes;
    --count_;
}
// #718: the fill state of a snapshot that is usable before it is whole. The file has the source's
// full length from the start; a chunk is Present once its bytes were written. The holder of this
// state owns the source handle and the only writable handle to the private file.
struct ReadOnlyFileSnapshot::Progressive {
#if defined(_WIN32)
    using File = HANDLE;
    static constexpr File kNoFile = nullptr;
#else
    using File = int;
    static constexpr File kNoFile = -1;
#endif
    enum Chunk : std::uint8_t { Missing, Filling, Present };
    File source = kNoFile, writer = kNoFile;
    // The snapshot's own read-only handle (State owns it); the hash reads through it.
    File reader = kNoFile;
    std::filesystem::path sourcePath;
    std::uint64_t size = 0;
    core::Sha256Digest claimed{};
    std::optional<FileIdentity> observedBefore;
    std::shared_ptr<SourceChunkStore> chunks;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::uint8_t> state;
    std::uint64_t presentChunks = 0, localBytes = 0;
    // Callers inside ensureRange(): the background fill stands aside while there are any.
    std::size_t foreground = 0;
    SnapshotVerification verification = SnapshotVerification::Pending;
    bool hashing = false;
    std::atomic_bool stop{false};
    std::thread thread;

    ~Progressive();
    [[nodiscard]] std::uint64_t chunkCount() const {
        return (size + kProgressiveChunkBytes - 1) / kProgressiveChunkBytes;
    }
    [[nodiscard]] std::uint64_t chunkBytes(const std::uint64_t index) const {
        return std::min<std::uint64_t>(kProgressiveChunkBytes,
                                       size - index * kProgressiveChunkBytes);
    }
    // Makes one chunk Present. False when it could not be read or written, or on cancel/stop.
    [[nodiscard]] bool fill(std::uint64_t index, const ProcessCancellation& cancel, int& error);
    // Hashes the whole local file once every chunk is Present and settles `verification`.
    void verify();
    void background();
};
struct ReadOnlyFileSnapshot::State {
    int fd = -1;
#if defined(_WIN32)
    // The read-only handle to the private copy. The copy is removed when its last handle closes.
    HANDLE handle = nullptr;
#endif
    std::uint64_t bytes = 0;
    core::Sha256Digest digest;
    FileSnapshotStatistics statistics;
    std::optional<FileIdentity> sourceIdentity;
    std::shared_ptr<FileSnapshotQuota> quota;
    // #718: set for a snapshot that fills by byte range. Ends its thread and closes its own
    // handles before the reader below closes.
    std::unique_ptr<Progressive> progressive;
    ~State() {
        progressive.reset();
#if defined(__linux__) || defined(__APPLE__)
        if (fd >= 0)
            ::close(fd);
#endif
#if defined(_WIN32)
        if (handle != nullptr)
            CloseHandle(handle);
#endif
        if (quota)
            quota->release(bytes);
    }
};
ReadOnlyFileSnapshot::ReadOnlyFileSnapshot(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
ReadOnlyFileSnapshot::~ReadOnlyFileSnapshot() = default;
const core::Sha256Digest& ReadOnlyFileSnapshot::digest() const { return state_->digest; }
std::uint64_t ReadOnlyFileSnapshot::size() const { return state_->bytes; }
const FileSnapshotStatistics& ReadOnlyFileSnapshot::statistics() const {
    return state_->statistics;
}
const std::optional<FileIdentity>& ReadOnlyFileSnapshot::sourceIdentity() const {
    return state_->sourceIdentity;
}
int ReadOnlyFileSnapshot::descriptor() const { return state_->fd; }
void* ReadOnlyFileSnapshot::nativeHandle() const {
#if defined(_WIN32)
    return state_->handle;
#else
    return nullptr;
#endif
}
#if defined(_WIN32)
namespace {
constexpr wchar_t kSnapshotVariable[] = L"BLOOM_PROCESS_SNAPSHOT";
using Clock = std::chrono::steady_clock;
void elapsed(std::uint64_t& total, const Clock::time_point begin) {
    const auto value =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
    if (value > 0) {
        const auto amount = static_cast<std::uint64_t>(value);
        total = amount > std::numeric_limits<std::uint64_t>::max() - total
                    ? std::numeric_limits<std::uint64_t>::max()
                    : total + amount;
    }
}
class FileHandle final {
  public:
    explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~FileHandle() {
        if (valid())
            CloseHandle(value_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    void reset(const HANDLE value) {
        if (valid())
            CloseHandle(value_);
        value_ = value;
    }

  private:
    HANDLE value_;
};
// A regular file's size, or nothing for anything else.
std::optional<std::uint64_t> regularFileSize(const HANDLE file) {
    FILE_STANDARD_INFO standard{};
    if (GetFileType(file) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ||
        standard.Directory || standard.EndOfFile.QuadPart < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
}
int lastError() { return static_cast<int>(GetLastError()); }
} // namespace
#endif
#if defined(__linux__) || defined(__APPLE__)
namespace {
class Descriptor final {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0)
            ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
    int take() { return std::exchange(value_, -1); }

  private:
    int value_;
};
using Clock = std::chrono::steady_clock;
void elapsed(std::uint64_t& total, const Clock::time_point begin) {
    const auto value =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
    if (value > 0) {
        const auto amount = static_cast<std::uint64_t>(value);
        total = amount > std::numeric_limits<std::uint64_t>::max() - total
                    ? std::numeric_limits<std::uint64_t>::max()
                    : total + amount;
    }
}
struct Temporary final {
    std::filesystem::path directory, file;
    ~Temporary() {
        std::error_code ignored;
        if (!file.empty())
            std::filesystem::remove(file, ignored);
        if (!directory.empty())
            std::filesystem::remove(directory, ignored);
    }
};
} // namespace
#endif
ProcessResult<std::unique_ptr<ReadOnlyFileSnapshot>> ReadOnlyFileSnapshot::capture(
    const std::filesystem::path& source, const std::filesystem::path& spoolDirectory,
    const std::shared_ptr<FileSnapshotQuota>& quota, const std::uint64_t sourceLimit,
    const ProcessCancellation& cancel, const Progress& progress,
    const std::optional<core::Sha256Digest> expected, const std::size_t hostByteBudget) {
#if defined(__linux__) || defined(__APPLE__)
    if (cancel && cancel())
        return ProcessFailure{ProcessError::Cancelled, 0};
    // Observed before the open so a replacement during the copy cannot be recorded as verified.
    const auto observedBefore = observeFileIdentity(source);
    Descriptor input(::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    struct stat status{};
    if (input.get() < 0 || ::fstat(input.get(), &status) != 0 || !S_ISREG(status.st_mode))
        return ProcessFailure{ProcessError::Io, errno};
    if (status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > sourceLimit || !quota)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto size = static_cast<std::uint64_t>(status.st_size);
    constexpr std::size_t overhead = sizeof(State) + sizeof(ReadOnlyFileSnapshot) + 512;
    const auto pathBytes = spoolDirectory.native().size();
    if (pathBytes > (std::numeric_limits<std::size_t>::max() - overhead) / 6)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto metadata = overhead + pathBytes * 6;
    if (hostByteBudget <= metadata)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto chunkBytes = std::min<std::size_t>(65536, hostByteBudget - metadata);
    auto state = std::make_unique<State>();
    if (!quota->reserve(size))
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    state->quota = quota;
    state->bytes = size;
    std::error_code error;
    if (!spoolDirectory.is_absolute())
        return ProcessFailure{ProcessError::Io, EINVAL};
    std::filesystem::create_directories(spoolDirectory, error);
    if (error)
        return ProcessFailure{ProcessError::Io, error.value()};
    auto pattern = (spoolDirectory / "capture-XXXXXX").native();
    if (::mkdtemp(pattern.data()) == nullptr)
        return ProcessFailure{ProcessError::Io, errno};
    Temporary temporary{std::filesystem::path(pattern), {}};
    temporary.file = temporary.directory / "source";
    Descriptor output(
        ::open(temporary.file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (output.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    core::Sha256Hasher hasher;
    std::vector<std::byte> buffer(chunkBytes);
    std::uint64_t total = 0;
    while (total < size) {
        if (cancel && cancel())
            return ProcessFailure{ProcessError::Cancelled, 0};
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - total));
        auto begin = Clock::now();
        ssize_t read = 0;
        do {
            read = ::read(input.get(), buffer.data(), count);
        } while (read < 0 && errno == EINTR);
        elapsed(state->statistics.ioNanoseconds, begin);
        if (read <= 0)
            return ProcessFailure{ProcessError::Io, errno};
        const auto amount = static_cast<std::size_t>(read);
        begin = Clock::now();
        if (!hasher.update(std::span(buffer).first(amount)))
            return ProcessFailure{ProcessError::ResourceLimit, 0};
        elapsed(state->statistics.hashNanoseconds, begin);
        begin = Clock::now();
        std::size_t written = 0;
        while (written < amount) {
            ssize_t result = 0;
            do {
                result = ::write(output.get(), buffer.data() + written, amount - written);
            } while (result < 0 && errno == EINTR);
            if (result <= 0)
                return ProcessFailure{ProcessError::Io, errno};
            written += static_cast<std::size_t>(result);
        }
        elapsed(state->statistics.ioNanoseconds, begin);
        total += amount;
        state->statistics.bytesRead = total;
        state->statistics.bytesHashed = total;
        if (progress)
            progress(total, size);
    }
    std::byte extra{};
    ssize_t read = 0;
    do {
        read = ::read(input.get(), &extra, 1);
    } while (read < 0 && errno == EINTR);
    if (read != 0)
        return ProcessFailure{ProcessError::Io, read < 0 ? errno : EFBIG};
    recordFullSourceRead(total);
    state->digest = hasher.finalize();
    if (const auto observedAfter = observeFileIdentity(source);
        observedBefore && observedAfter && *observedBefore == *observedAfter &&
        observedAfter->size == size &&
        observedAfter->fileIdLow == static_cast<std::uint64_t>(status.st_ino) &&
        observedAfter->volume == static_cast<std::uint64_t>(status.st_dev)) {
        state->sourceIdentity = observedAfter;
        verifiedSourceDigests().record(source, *observedAfter, state->digest);
    }
    if (expected && state->digest != *expected)
        return ProcessFailure{ProcessError::Io, ESTALE};
    if (cancel && cancel())
        return ProcessFailure{ProcessError::Cancelled, 0};
    if (::fchmod(output.get(), 0400) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    // The private pathname is not authority: a same-size replacement must never associate the
    // writer's captured digest with different bytes reopened for the decoder.
    struct stat writerStatus{};
    if (::fstat(output.get(), &writerStatus) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (!S_ISREG(writerStatus.st_mode) || writerStatus.st_size < 0 ||
        static_cast<std::uint64_t>(writerStatus.st_size) != size)
        return ProcessFailure{ProcessError::Io, ESTALE};
    Descriptor readonly(::open(temporary.file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (readonly.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    struct stat frozen{};
    if (::fstat(readonly.get(), &frozen) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (!S_ISREG(frozen.st_mode) || frozen.st_size < 0 ||
        static_cast<std::uint64_t>(frozen.st_size) != size ||
        frozen.st_dev != writerStatus.st_dev || frozen.st_ino != writerStatus.st_ino)
        return ProcessFailure{ProcessError::Io, ESTALE};
    // Publish only after both names have disappeared and the sole write descriptor has closed.
    if (::unlink(temporary.file.c_str()) != 0 || ::rmdir(temporary.directory.c_str()) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    const auto writer = output.take();
    if (::close(writer) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (::fstat(readonly.get(), &frozen) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (!S_ISREG(frozen.st_mode) || frozen.st_nlink != 0 || frozen.st_size < 0 ||
        static_cast<std::uint64_t>(frozen.st_size) != size ||
        frozen.st_dev != writerStatus.st_dev || frozen.st_ino != writerStatus.st_ino)
        return ProcessFailure{ProcessError::Io, ESTALE};
    // Keep source descriptors above the bootstrap/control descriptor pair for spawn file actions.
    state->fd = ::fcntl(readonly.get(), F_DUPFD_CLOEXEC, 5);
    if (state->fd < 0)
        return ProcessFailure{ProcessError::Io, errno};
    return std::unique_ptr<ReadOnlyFileSnapshot>(new ReadOnlyFileSnapshot(std::move(state)));
#elif defined(_WIN32)
    // The same capture as above with Windows' means. The private copy is a file in the spool
    // directory whose name is removed as soon as it is made, so nothing else can open it; the
    // system removes it when its last handle closes, and the worker receives a read-only handle.
    if (cancel && cancel())
        return ProcessFailure{ProcessError::Cancelled, 0};
    // Observed before the open so a replacement during the copy cannot be recorded as verified.
    const auto observedBefore = observeFileIdentity(source);
    const FileHandle input(CreateFileW(source.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input.valid())
        return ProcessFailure{ProcessError::Io, lastError()};
    const auto sourceSize = regularFileSize(input.get());
    if (!sourceSize)
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_DATA};
    if (*sourceSize == 0 || *sourceSize > sourceLimit || !quota)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto size = *sourceSize;
    constexpr std::size_t overhead = sizeof(State) + sizeof(ReadOnlyFileSnapshot) + 512;
    const auto pathBytes = spoolDirectory.native().size();
    if (pathBytes > (std::numeric_limits<std::size_t>::max() - overhead) / 6)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto metadata = overhead + pathBytes * 6;
    if (hostByteBudget <= metadata)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto chunkBytes = std::min<std::size_t>(65536, hostByteBudget - metadata);
    auto state = std::make_unique<State>();
    if (!quota->reserve(size))
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    state->quota = quota;
    state->bytes = size;
    std::error_code error;
    if (!spoolDirectory.is_absolute())
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_PARAMETER};
    std::filesystem::create_directories(spoolDirectory, error);
    if (error)
        return ProcessFailure{ProcessError::Io, error.value()};
    FileHandle output;
    // A second, read-only open of the copy, made while it still has a name. It is what leaves
    // here when the name could be taken away.
    FileHandle reader;
    {
        std::random_device device;
        constexpr std::wstring_view digits = L"0123456789abcdef";
        constexpr DWORD everyone = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        for (int attempt = 0; attempt < 64 && !output.valid(); ++attempt) {
            std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32U) | device();
            std::wstring name = L"capture-";
            for (int nibble = 0; nibble < 16; ++nibble, value >>= 4U)
                name.push_back(digits[value & 0xFU]);
            const auto path = spoolDirectory / name;
            const HANDLE created = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                               FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (created == INVALID_HANDLE_VALUE) {
                if (GetLastError() != ERROR_FILE_EXISTS)
                    return ProcessFailure{ProcessError::Io, lastError()};
                continue;
            }
            output.reset(created);
            // Take the name away at once, so the copy has no pathname while it is written and
            // used: a handle that exists only to remove the name does it, and the name goes when
            // that handle closes. The writer and the reader stay open on a file nothing can find.
            const HANDLE read = ReOpenFile(created, FILE_GENERIC_READ, everyone, 0);
            bool unnamed = false;
            if (read != INVALID_HANDLE_VALUE) {
                const HANDLE unlinker = ReOpenFile(created, DELETE, everyone, 0);
                if (unlinker != INVALID_HANDLE_VALUE) {
                    FILE_DISPOSITION_INFO_EX unlink{};
                    unlink.Flags =
                        FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
                    unnamed = SetFileInformationByHandle(unlinker, FileDispositionInfoEx, &unlink,
                                                         sizeof(unlink)) != 0;
                    CloseHandle(unlinker);
                }
            }
            if (unnamed) {
                reader.reset(read);
                break;
            }
            if (read != INVALID_HANDLE_VALUE)
                CloseHandle(read);
            // This file system cannot take a name away from an open file. Start again with a
            // file that nothing else can open (no sharing) and that the system removes when its
            // last handle closes.
            FILE_DISPOSITION_INFO discard{};
            discard.DeleteFile = TRUE;
            static_cast<void>(SetFileInformationByHandle(created, FileDispositionInfo, &discard,
                                                         sizeof(discard)));
            output.reset(INVALID_HANDLE_VALUE);
            const HANDLE exclusive = CreateFileW(
                path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (exclusive == INVALID_HANDLE_VALUE)
                return ProcessFailure{ProcessError::Io, lastError()};
            output.reset(exclusive);
        }
    }
    if (!output.valid())
        return ProcessFailure{ProcessError::Io, ERROR_FILE_EXISTS};
    core::Sha256Hasher hasher;
    std::vector<std::byte> buffer(chunkBytes);
    std::uint64_t total = 0;
    while (total < size) {
        if (cancel && cancel())
            return ProcessFailure{ProcessError::Cancelled, 0};
        const auto count = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), size - total));
        auto begin = Clock::now();
        DWORD read = 0;
        const BOOL readOk = ReadFile(input.get(), buffer.data(), count, &read, nullptr);
        elapsed(state->statistics.ioNanoseconds, begin);
        if (!readOk || read == 0)
            return ProcessFailure{ProcessError::Io, readOk ? ERROR_HANDLE_EOF : lastError()};
        const auto amount = static_cast<std::size_t>(read);
        begin = Clock::now();
        if (!hasher.update(std::span(buffer).first(amount)))
            return ProcessFailure{ProcessError::ResourceLimit, 0};
        elapsed(state->statistics.hashNanoseconds, begin);
        begin = Clock::now();
        std::size_t written = 0;
        while (written < amount) {
            DWORD result = 0;
            if (!WriteFile(output.get(), buffer.data() + written,
                           static_cast<DWORD>(amount - written), &result, nullptr) ||
                result == 0)
                return ProcessFailure{ProcessError::Io, lastError()};
            written += result;
        }
        elapsed(state->statistics.ioNanoseconds, begin);
        total += amount;
        state->statistics.bytesRead = total;
        state->statistics.bytesHashed = total;
        if (progress)
            progress(total, size);
    }
    std::byte extra{};
    DWORD more = 0;
    if (!ReadFile(input.get(), &extra, 1, &more, nullptr))
        return ProcessFailure{ProcessError::Io, lastError()};
    if (more != 0)
        return ProcessFailure{ProcessError::Io, ERROR_FILE_TOO_LARGE};
    recordFullSourceRead(total);
    state->digest = hasher.finalize();
    if (const auto observedAfter = observeFileIdentity(source); observedBefore && observedAfter &&
                                                                *observedBefore == *observedAfter &&
                                                                observedAfter->size == size) {
        state->sourceIdentity = observedAfter;
        verifiedSourceDigests().record(source, *observedAfter, state->digest);
    }
    if (expected && state->digest != *expected)
        return ProcessFailure{ProcessError::Io, ERROR_FILE_INVALID};
    if (cancel && cancel())
        return ProcessFailure{ProcessError::Cancelled, 0};
    if (regularFileSize(output.get()) != size)
        return ProcessFailure{ProcessError::Io, ERROR_FILE_INVALID};
    // The only handle that leaves here can read and nothing else, so the copy lives exactly as
    // long as this snapshot and the workers given it: the reader made above, or, where the name
    // could not be taken away, a read-only handle to the same open file.
    HANDLE readonly = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), reader.valid() ? reader.get() : output.get(),
                         GetCurrentProcess(), &readonly, FILE_GENERIC_READ, FALSE, 0))
        return ProcessFailure{ProcessError::Io, lastError()};
    state->handle = readonly;
    return std::unique_ptr<ReadOnlyFileSnapshot>(new ReadOnlyFileSnapshot(std::move(state)));
#else
    (void)source;
    (void)spoolDirectory;
    (void)quota;
    (void)sourceLimit;
    (void)cancel;
    (void)progress;
    (void)expected;
    (void)hostByteBudget;
    return ProcessFailure{ProcessError::Unavailable, 0};
#endif
}
// ---- #718: progressive snapshots -------------------------------------------------------------
namespace {
#if defined(__linux__) || defined(__APPLE__)
// Exactly bytes.size() bytes at `offset`, or false with errno-style `error`.
bool readAt(const int file, std::uint64_t offset, std::span<std::byte> bytes, int& error) {
    while (!bytes.empty()) {
        ssize_t count = 0;
        do {
            count = ::pread(file, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            error = count < 0 ? errno : EIO; // a short file: the source shrank
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return true;
}
bool writeAt(const int file, std::uint64_t offset, std::span<const std::byte> bytes, int& error) {
    while (!bytes.empty()) {
        ssize_t count = 0;
        do {
            count = ::pwrite(file, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            error = count < 0 ? errno : EIO;
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return true;
}
void closeFile(const int file) {
    if (file >= 0)
        ::close(file);
}
#elif defined(_WIN32)
bool readAt(const HANDLE file, std::uint64_t offset, std::span<std::byte> bytes, int& error) {
    while (!bytes.empty()) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        const auto ask =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::size_t{1} << 30U));
        DWORD count = 0;
        if (!ReadFile(file, bytes.data(), ask, &count, &position) || count == 0) {
            error = count == 0 && GetLastError() == ERROR_SUCCESS ? ERROR_HANDLE_EOF : lastError();
            return false;
        }
        bytes = bytes.subspan(count);
        offset += count;
    }
    return true;
}
bool writeAt(const HANDLE file, std::uint64_t offset, std::span<const std::byte> bytes,
             int& error) {
    while (!bytes.empty()) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        const auto ask =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::size_t{1} << 30U));
        DWORD count = 0;
        if (!WriteFile(file, bytes.data(), ask, &count, &position) || count == 0) {
            error = lastError();
            return false;
        }
        bytes = bytes.subspan(count);
        offset += count;
    }
    return true;
}
void closeFile(const HANDLE file) {
    if (file != nullptr && file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}
#endif
} // namespace

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
ReadOnlyFileSnapshot::Progressive::~Progressive() {
    stop.store(true);
    changed.notify_all();
    if (thread.joinable())
        thread.join();
    closeFile(writer);
    closeFile(source);
}
bool ReadOnlyFileSnapshot::Progressive::fill(const std::uint64_t index,
                                             const ProcessCancellation& cancel, int& error) {
    {
        std::unique_lock lock(mutex);
        while (state[index] == Filling) {
            // Another thread is bringing this chunk in: its result is this caller's too.
            changed.wait_for(lock, std::chrono::milliseconds(20));
            if (stop.load() || (cancel && cancel()))
                return false;
        }
        if (state[index] == Present)
            return true;
        state[index] = Filling;
    }
    const auto offset = index * kProgressiveChunkBytes;
    std::vector<std::byte> buffer(static_cast<std::size_t>(chunkBytes(index)));
    // Local bytes first; the source only for what the store does not hold, and what the source
    // gave is kept. No lock is held across any of this I/O.
    bool ok = chunks && chunks->read(offset, buffer);
    if (!ok) {
        ok = readAt(source, offset, buffer, error);
        if (ok && chunks)
            chunks->keep(offset, buffer);
    }
    ok = ok && writeAt(writer, offset, buffer, error);
    {
        const std::lock_guard lock(mutex);
        state[index] = ok ? Present : Missing;
        if (ok) {
            ++presentChunks;
            localBytes += buffer.size();
        }
    }
    changed.notify_all();
    return ok;
}
void ReadOnlyFileSnapshot::Progressive::verify() {
    {
        std::unique_lock lock(mutex);
        if (presentChunks != chunkCount())
            return;
        while (hashing) {
            changed.wait_for(lock, std::chrono::milliseconds(20));
            if (stop.load())
                return;
        }
        if (verification != SnapshotVerification::Pending)
            return;
        hashing = true;
    }
    // The local file is whole: hash it (local reads) and compare with the claimed digest. The
    // source must still be the file that was opened, or what was read may be a mixture.
    core::Sha256Hasher hasher;
    std::vector<std::byte> buffer(static_cast<std::size_t>(kProgressiveChunkBytes));
    bool hashed = true;
    for (std::uint64_t offset = 0; hashed && offset < size && !stop.load();) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        int error = 0;
        hashed = readAt(reader, offset, std::span(buffer).first(count), error) &&
                 hasher.update(std::span(buffer).first(count));
        offset += count;
    }
    auto result = SnapshotVerification::Pending;
    if (hashed && !stop.load()) {
        const auto observedAfter = observeFileIdentity(sourcePath);
        const bool unchanged = observedBefore && observedAfter &&
                               *observedBefore == *observedAfter && observedAfter->size == size;
        const bool matches = hasher.finalize() == claimed;
        result = matches && unchanged ? SnapshotVerification::Verified
                                      : SnapshotVerification::Failed;
        if (result == SnapshotVerification::Verified)
            verifiedSourceDigests().record(sourcePath, *observedAfter, claimed);
    }
    {
        const std::lock_guard lock(mutex);
        hashing = false;
        if (result != SnapshotVerification::Pending)
            verification = result;
    }
    changed.notify_all();
}
void ReadOnlyFileSnapshot::Progressive::background() {
    // The rest of the file, behind whatever a frame asks for: idle priority, standing aside for
    // foreground ranges and while the chunk store is behind or short of space.
    const IdleThreadPriorityScope idle(true);
    const auto count = chunkCount();
    int failures = 0;
    for (std::uint64_t index = 0; index < count && !stop.load();) {
        {
            std::unique_lock lock(mutex);
            if (state[index] == Present) {
                ++index;
                continue;
            }
            if (foreground != 0) {
                changed.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
        }
        if (chunks && chunks->underPressure()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        int error = 0;
        if (fill(index, [this] { return stop.load(); }, error)) {
            failures = 0;
            ++index;
            continue;
        }
        // A source that stopped answering: wait and try again a few times, then leave the rest
        // to the ranges frames ask for.
        if (++failures > 5)
            return;
        for (int wait = 0; wait < 25 && !stop.load(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!stop.load())
        verify();
}
#endif

bool ReadOnlyFileSnapshot::progressive() const { return state_->progressive != nullptr; }
std::uint64_t ReadOnlyFileSnapshot::localBytes() const {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    if (auto* fill = state_->progressive.get()) {
        const std::lock_guard lock(fill->mutex);
        return fill->localBytes;
    }
#endif
    return state_->bytes;
}
SnapshotVerification ReadOnlyFileSnapshot::verification() const {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    if (auto* fill = state_->progressive.get()) {
        const std::lock_guard lock(fill->mutex);
        return fill->verification;
    }
#endif
    return SnapshotVerification::Verified;
}
ProcessResult<SnapshotRange>
ReadOnlyFileSnapshot::ensureRange(const std::uint64_t offset, const std::uint64_t length,
                                  const ProcessCancellation& cancel) const {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    auto* fill = state_->progressive.get();
    if (fill == nullptr)
        return SnapshotRange{0, state_->bytes, true};
    if (offset > fill->size)
        return ProcessFailure{ProcessError::Io, 0};
    const auto end = offset + std::min(length, fill->size - offset);
    if (end == offset)
        return SnapshotRange{offset, offset, false};
    const auto first = offset / kProgressiveChunkBytes;
    const auto last = (end - 1) / kProgressiveChunkBytes;
    {
        const std::lock_guard lock(fill->mutex);
        ++fill->foreground;
    }
    bool ok = true;
    int error = 0;
    for (auto index = first; ok && index <= last; ++index)
        ok = fill->fill(index, cancel, error);
    SnapshotRange range;
    {
        const std::lock_guard lock(fill->mutex);
        --fill->foreground;
        if (ok) {
            // Report what is local around the asked range too, so the asker need not come back
            // for its neighbors.
            auto low = first, high = last;
            while (low > 0 && fill->state[low - 1] == Progressive::Present)
                --low;
            const auto count = fill->chunkCount();
            while (high + 1 < count && fill->state[high + 1] == Progressive::Present)
                ++high;
            range.begin = low * kProgressiveChunkBytes;
            range.end = std::min(fill->size, (high + 1) * kProgressiveChunkBytes);
            range.complete = fill->presentChunks == count;
        }
    }
    fill->changed.notify_all();
    if (!ok)
        return ProcessFailure{cancel && cancel() ? ProcessError::Cancelled : ProcessError::Io,
                              error};
    return range;
#else
    (void)offset;
    (void)length;
    (void)cancel;
    return SnapshotRange{0, state_->bytes, true};
#endif
}
ProcessResult<SnapshotVerification>
ReadOnlyFileSnapshot::waitVerified(const ProcessCancellation& cancel,
                                   const Progress& progress) const {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    auto* fill = state_->progressive.get();
    if (fill == nullptr)
        return SnapshotVerification::Verified;
    const auto count = fill->chunkCount();
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto made = ensureRange(index * kProgressiveChunkBytes, kProgressiveChunkBytes, cancel);
        if (const auto* failure = std::get_if<ProcessFailure>(&made))
            return *failure;
        if (progress)
            progress(localBytes(), fill->size);
    }
    fill->verify();
    std::unique_lock lock(fill->mutex);
    while (fill->verification == SnapshotVerification::Pending) {
        // Another thread holds the hash; it settles the result.
        if (cancel && cancel())
            return ProcessFailure{ProcessError::Cancelled, 0};
        if (!fill->hashing) {
            lock.unlock();
            fill->verify();
            lock.lock();
            if (fill->verification == SnapshotVerification::Pending && !fill->hashing)
                return ProcessFailure{ProcessError::Io, 0};
            continue;
        }
        fill->changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    return fill->verification;
#else
    (void)cancel;
    (void)progress;
    return SnapshotVerification::Verified;
#endif
}
ProcessResult<std::unique_ptr<ReadOnlyFileSnapshot>>
ReadOnlyFileSnapshot::captureProgressive(const ProgressiveSnapshotRequest& request) {
#if defined(__linux__) || defined(__APPLE__)
    const auto observedBefore = observeFileIdentity(request.source);
    Descriptor input(::open(request.source.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    struct stat status{};
    if (input.get() < 0 || ::fstat(input.get(), &status) != 0 || !S_ISREG(status.st_mode))
        return ProcessFailure{ProcessError::Io, errno};
    if (status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > request.sourceLimit ||
        !request.quota)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto size = static_cast<std::uint64_t>(status.st_size);
    auto state = std::make_unique<State>();
    if (!request.quota->reserve(size))
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    state->quota = request.quota;
    state->bytes = size;
    state->digest = request.claimed;
    std::error_code error;
    if (!request.spoolDirectory.is_absolute())
        return ProcessFailure{ProcessError::Io, EINVAL};
    std::filesystem::create_directories(request.spoolDirectory, error);
    if (error)
        return ProcessFailure{ProcessError::Io, error.value()};
    auto pattern = (request.spoolDirectory / "capture-XXXXXX").native();
    if (::mkdtemp(pattern.data()) == nullptr)
        return ProcessFailure{ProcessError::Io, errno};
    Temporary temporary{std::filesystem::path(pattern), {}};
    temporary.file = temporary.directory / "source";
    Descriptor output(
        ::open(temporary.file.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (output.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    // The full length at once, without writing it: reads of a range that is not local yet see a
    // hole, and nothing reads one before ensureRange() returned for it.
    if (::ftruncate(output.get(), static_cast<off_t>(size)) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    struct stat writerStatus{};
    if (::fstat(output.get(), &writerStatus) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    Descriptor readonly(::open(temporary.file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (readonly.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    // No name is left: the worker's handle is to a file nothing else can open, and the only
    // writable handle stays in this process, inside the fill state.
    if (::unlink(temporary.file.c_str()) != 0 || ::rmdir(temporary.directory.c_str()) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    struct stat frozen{};
    if (::fstat(readonly.get(), &frozen) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (!S_ISREG(frozen.st_mode) || frozen.st_nlink != 0 || frozen.st_size < 0 ||
        static_cast<std::uint64_t>(frozen.st_size) != size ||
        frozen.st_dev != writerStatus.st_dev || frozen.st_ino != writerStatus.st_ino)
        return ProcessFailure{ProcessError::Io, ESTALE};
    // Keep source descriptors above the bootstrap/control descriptor pair for spawn file actions.
    state->fd = ::fcntl(readonly.get(), F_DUPFD_CLOEXEC, 5);
    if (state->fd < 0)
        return ProcessFailure{ProcessError::Io, errno};
    auto fill = std::make_unique<Progressive>();
    fill->writer = ::fcntl(output.get(), F_DUPFD_CLOEXEC, 5);
    fill->source = ::fcntl(input.get(), F_DUPFD_CLOEXEC, 5);
    if (fill->writer < 0 || fill->source < 0)
        return ProcessFailure{ProcessError::Io, errno};
    fill->reader = state->fd;
#elif defined(_WIN32)
    const auto observedBefore = observeFileIdentity(request.source);
    FileHandle input(CreateFileW(request.source.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr));
    if (!input.valid())
        return ProcessFailure{ProcessError::Io, lastError()};
    const auto sourceSize = regularFileSize(input.get());
    if (!sourceSize)
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_DATA};
    if (*sourceSize == 0 || *sourceSize > request.sourceLimit || !request.quota)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto size = *sourceSize;
    auto state = std::make_unique<State>();
    if (!request.quota->reserve(size))
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    state->quota = request.quota;
    state->bytes = size;
    state->digest = request.claimed;
    std::error_code error;
    if (!request.spoolDirectory.is_absolute())
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_PARAMETER};
    std::filesystem::create_directories(request.spoolDirectory, error);
    if (error)
        return ProcessFailure{ProcessError::Io, error.value()};
    // The private file, made as capture() makes it: its name is taken away at once, or, where
    // the file system cannot do that, it is opened with no sharing and removed on close.
    FileHandle output, reader;
    {
        std::random_device device;
        constexpr std::wstring_view digits = L"0123456789abcdef";
        constexpr DWORD everyone = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        for (int attempt = 0; attempt < 64 && !output.valid(); ++attempt) {
            std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32U) | device();
            std::wstring name = L"capture-";
            for (int nibble = 0; nibble < 16; ++nibble, value >>= 4U)
                name.push_back(digits[value & 0xFU]);
            const auto path = request.spoolDirectory / name;
            const HANDLE created = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                               FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (created == INVALID_HANDLE_VALUE) {
                if (GetLastError() != ERROR_FILE_EXISTS)
                    return ProcessFailure{ProcessError::Io, lastError()};
                continue;
            }
            output.reset(created);
            const HANDLE read = ReOpenFile(created, FILE_GENERIC_READ, everyone, 0);
            bool unnamed = false;
            if (read != INVALID_HANDLE_VALUE) {
                const HANDLE unlinker = ReOpenFile(created, DELETE, everyone, 0);
                if (unlinker != INVALID_HANDLE_VALUE) {
                    FILE_DISPOSITION_INFO_EX unlink{};
                    unlink.Flags =
                        FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
                    unnamed = SetFileInformationByHandle(unlinker, FileDispositionInfoEx, &unlink,
                                                         sizeof(unlink)) != 0;
                    CloseHandle(unlinker);
                }
            }
            if (unnamed) {
                reader.reset(read);
                break;
            }
            if (read != INVALID_HANDLE_VALUE)
                CloseHandle(read);
            FILE_DISPOSITION_INFO discard{};
            discard.DeleteFile = TRUE;
            static_cast<void>(SetFileInformationByHandle(created, FileDispositionInfo, &discard,
                                                         sizeof(discard)));
            output.reset(INVALID_HANDLE_VALUE);
            const HANDLE exclusive = CreateFileW(
                path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (exclusive == INVALID_HANDLE_VALUE)
                return ProcessFailure{ProcessError::Io, lastError()};
            output.reset(exclusive);
        }
    }
    if (!output.valid())
        return ProcessFailure{ProcessError::Io, ERROR_FILE_EXISTS};
    // Sparse, so giving the file its full length and writing a range far into it does not make
    // the file system write zeros up to there first. A file system without sparse files still
    // works; the first far write is then slower.
    DWORD returned = 0;
    static_cast<void>(
        DeviceIoControl(output.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
    LARGE_INTEGER length{};
    length.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(output.get(), length, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(output.get()) || regularFileSize(output.get()) != size)
        return ProcessFailure{ProcessError::Io, lastError()};
    HANDLE readonly = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), reader.valid() ? reader.get() : output.get(),
                         GetCurrentProcess(), &readonly, FILE_GENERIC_READ, FALSE, 0))
        return ProcessFailure{ProcessError::Io, lastError()};
    state->handle = readonly;
    auto fill = std::make_unique<Progressive>();
    HANDLE writer = nullptr, source = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), output.get(), GetCurrentProcess(), &writer, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return ProcessFailure{ProcessError::Io, lastError()};
    fill->writer = writer;
    if (!DuplicateHandle(GetCurrentProcess(), input.get(), GetCurrentProcess(), &source, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return ProcessFailure{ProcessError::Io, lastError()};
    fill->source = source;
    fill->reader = readonly;
#else
    (void)request;
    return ProcessFailure{ProcessError::Unavailable, 0};
#endif
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    fill->sourcePath = request.source;
    fill->size = size;
    fill->claimed = request.claimed;
    fill->observedBefore = observedBefore;
    fill->chunks = request.chunks;
    fill->state.assign(static_cast<std::size_t>(fill->chunkCount()), Progressive::Missing);
    if (observedBefore && observedBefore->size == size)
        state->sourceIdentity = observedBefore;
    auto* raw = fill.get();
    state->progressive = std::move(fill);
    auto snapshot = std::unique_ptr<ReadOnlyFileSnapshot>(new ReadOnlyFileSnapshot(std::move(state)));
    if (request.backgroundFill)
        raw->thread = std::thread([raw] { raw->background(); });
    return snapshot;
#endif
}
ProcessResult<std::unique_ptr<WorkerSnapshotInput>>
WorkerSnapshotInput::open(const std::uint64_t size) {
#if defined(__linux__) || defined(__APPLE__)
    struct stat status{};
    const int flags = ::fcntl(4, F_GETFL);
    if (size == 0 || size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        flags < 0 || (flags & O_ACCMODE) != O_RDONLY || ::fstat(4, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_nlink != 0 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) != size)
        return ProcessFailure{ProcessError::Io, EINVAL};
    // Keep descriptor 4 as the process-wide immutable authority across retained calls. Each
    // input owns a close-on-exec duplicate and uses pread, so shared file offsets are irrelevant.
    Descriptor descriptor(::fcntl(4, F_DUPFD_CLOEXEC, 5));
    if (descriptor.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    auto input =
        std::unique_ptr<WorkerSnapshotInput>(new WorkerSnapshotInput(size, descriptor.get()));
    (void)descriptor.take();
    return input;
#elif defined(_WIN32)
    // The supervisor names the inherited handle in the environment; it is read once and kept as
    // the process-wide authority across retained calls.
    static const HANDLE inherited = [] {
        wchar_t value[32] = {};
        const DWORD length = GetEnvironmentVariableW(kSnapshotVariable, value, 32);
        std::uintptr_t raw = 0;
        for (DWORD i = 0; i < length && length < 32; ++i) {
            if (value[i] < L'0' || value[i] > L'9')
                return static_cast<HANDLE>(nullptr);
            raw = raw * 10U + static_cast<std::uintptr_t>(value[i] - L'0');
        }
        SetEnvironmentVariableW(kSnapshotVariable, nullptr);
        return reinterpret_cast<HANDLE>(raw);
    }();
    // Read-only: a write of nothing is refused for a handle that cannot write.
    DWORD written = 0;
    const bool writable = inherited != nullptr && WriteFile(inherited, "", 0, &written, nullptr);
    if (size == 0 || size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        inherited == nullptr || writable || regularFileSize(inherited) != size)
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_PARAMETER};
    HANDLE own = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(), &own, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return ProcessFailure{ProcessError::Io, lastError()};
    return std::unique_ptr<WorkerSnapshotInput>(new WorkerSnapshotInput(size, own));
#else
    (void)size;
    return ProcessFailure{ProcessError::Unavailable, 0};
#endif
}
WorkerSnapshotInput::~WorkerSnapshotInput() {
#if defined(__linux__) || defined(__APPLE__)
    if (descriptor_ >= 0)
        ::close(descriptor_);
#endif
#if defined(_WIN32)
    if (handle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(handle_));
#endif
}
namespace {
WorkerSnapshotInput::RangeGate& rangeGate() {
    static WorkerSnapshotInput::RangeGate gate;
    return gate;
}
} // namespace
void WorkerSnapshotInput::setRangeGate(RangeGate gate) { rangeGate() = std::move(gate); }
ProcessResult<std::size_t> WorkerSnapshotInput::read(const std::uint64_t offset,
                                                     std::span<std::byte> bytes) const {
#if defined(__linux__) || defined(__APPLE__)
    if (offset > size_)
        return ProcessFailure{ProcessError::Io, EINVAL};
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), size_ - offset));
    if (count == 0)
        return std::size_t{0};
    // #718: a progressive source's range is local before it is read.
    if (const auto& gate = rangeGate(); gate && !gate(offset, count))
        return ProcessFailure{ProcessError::Io, EIO};
    ssize_t result = 0;
    do {
        result = ::pread(descriptor_, bytes.data(), count, static_cast<off_t>(offset));
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return ProcessFailure{ProcessError::Io, errno};
    return static_cast<std::size_t>(result);
#elif defined(_WIN32)
    if (offset > size_)
        return ProcessFailure{ProcessError::Io, ERROR_INVALID_PARAMETER};
    const auto count = static_cast<DWORD>(std::min<std::uint64_t>(
        std::min<std::uint64_t>(bytes.size(), size_ - offset), std::uint64_t{1} << 30U));
    if (count == 0)
        return std::size_t{0};
    // #718: a progressive source's range is local before it is read.
    if (const auto& gate = rangeGate(); gate && !gate(offset, count))
        return ProcessFailure{ProcessError::Io, ERROR_NOT_READY};
    // A read at a stated offset: the handle's own position is not used, so inputs that share
    // the open file do not disturb each other.
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), bytes.data(), count, &read, &position) &&
        GetLastError() != ERROR_HANDLE_EOF)
        return ProcessFailure{ProcessError::Io, lastError()};
    return static_cast<std::size_t>(read);
#else
    (void)offset;
    (void)bytes;
    return ProcessFailure{ProcessError::Unavailable, 0};
#endif
}
} // namespace bloom::platform

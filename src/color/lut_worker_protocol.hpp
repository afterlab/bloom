#pragma once
#include <array>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <cstdint>
#include <span>
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <string>
#include <vector>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstring>
#include <utility>
#include <windows.h>
#endif

namespace bloom::color::detail {
inline constexpr std::uint64_t kLutProtocolMagic = 0x424c4f4f4d4c5554ULL;
inline constexpr std::uint64_t kLutProtocolVersion = 1;
inline constexpr std::size_t kLutSlabBytes = std::size_t{16} * 1024U * 1024U;
// The isolated helper's serialized GPU-program reply is bounded by the same 64 MiB external-LUT
// ceiling; a larger reflected OCIO program is a typed refusal, never a truncated transport.
inline constexpr std::size_t kMaximumGpuProgramBytes = std::size_t{64} * 1024U * 1024U;
// One helper serves one command at a time. Command 0 is the handshake, 1 builds a CPU processor,
// 2 applies a pixel slab, and 3 reflects the executable GPU program for a FileTransform.
inline constexpr std::uint64_t kLutCommandHello = 0;
inline constexpr std::uint64_t kLutCommandBuildCpu = 1;
inline constexpr std::uint64_t kLutCommandApplySlab = 2;
inline constexpr std::uint64_t kLutCommandExtractGpuProgram = 3;
// Same-machine private IPC: fixed-width fields, no pointers or dependency objects. A generation
// owns one helper; every reply echoes its command nonce and exact byte count before publication.
using LutPacket = std::array<std::uint64_t, 12>;
inline LutPacket packet(const std::uint64_t command, const std::uint64_t nonce) {
    return {kLutProtocolMagic, kLutProtocolVersion, command, nonce, 0, 0, 0, 0, 0, 0, 0, 0};
}
inline bool validPacket(const LutPacket& value, const std::uint64_t command,
                        const std::uint64_t nonce) {
    return value[0] == kLutProtocolMagic && value[1] == kLutProtocolVersion &&
           value[2] == command && value[3] == nonce;
}

// Typed, versioned request for command 3. The fixed packet is the wire form; these named fields are
// the checked view both sides use instead of raw indices. `programExtent` is the pre-sized sealed
// output file the helper writes the serialized program into; `programInode` binds that descriptor.
struct LutGpuProgramRequest final {
    std::uint64_t lutBytes = 0;
    std::uint64_t lutFormat = 0;
    std::uint64_t interpolation = 0;
    std::uint64_t direction = 0;
    std::uint64_t lutInode = 0;
    std::uint64_t programExtent = 0;
    std::uint64_t programInode = 0;
    [[nodiscard]] LutPacket encode(const std::uint64_t nonce) const {
        auto value = packet(kLutCommandExtractGpuProgram, nonce);
        value[4] = lutBytes;
        value[5] = lutFormat;
        value[6] = interpolation;
        value[7] = direction;
        value[8] = lutInode;
        value[9] = programExtent;
        value[10] = programInode;
        return value;
    }
    [[nodiscard]] static LutGpuProgramRequest decode(const LutPacket& value) {
        return {value[4], value[5], value[6], value[7], value[8], value[9], value[10]};
    }
};
// Typed reply for command 3. `programBytes` is meaningful only when `error` is None.
struct LutGpuProgramReply final {
    LutError error = LutError::HelperProtocolViolation;
    std::uint64_t programBytes = 0;
};

#if defined(__linux__) || defined(__APPLE__)
class LutFd final {
  public:
    explicit LutFd(int value = -1) : value_(value) {}
    ~LutFd() {
        if (value_ >= 0)
            ::close(value_);
    }
    LutFd(const LutFd&) = delete;
    LutFd& operator=(const LutFd&) = delete;
    [[nodiscard]] int get() const { return value_; }

  private:
    int value_;
};
inline bool writeFd(const int fd, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto count = ::write(fd, bytes.data(), bytes.size());
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}
inline bool readFd(const int fd, std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const auto count = ::read(fd, bytes.data(), bytes.size());
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}
inline std::uint64_t inode(const int fd) {
    struct stat value{};
    return ::fstat(fd, &value) == 0 ? static_cast<std::uint64_t>(value.st_ino) : 0;
}
#ifdef __linux__
inline int sharedFile() { return ::memfd_create("bloom-lut", MFD_CLOEXEC | MFD_ALLOW_SEALING); }
inline bool sealExtent(const int fd) {
    return ::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) == 0;
}
inline bool sealInput(const int fd) {
    return ::fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0;
}
inline bool validSharedFile(const int fd, const std::uint64_t expectedInode,
                            const std::uint64_t bytes, const bool sealed) {
    struct stat info{};
    const auto seals = ::fcntl(fd, F_GET_SEALS);
    return ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
           static_cast<std::uint64_t>(info.st_size) == bytes &&
           static_cast<std::uint64_t>(info.st_ino) == expectedInode && seals >= 0 &&
           (seals & (F_SEAL_GROW | F_SEAL_SHRINK)) == (F_SEAL_GROW | F_SEAL_SHRINK) &&
           (!sealed || (seals & (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK)) ==
                           (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK));
}
#else
// macOS has no anonymous sealed file. A shared file here is a file in the user's private temporary
// directory that is unlinked before anything is written, so it has no name and lives only as long
// as its descriptors. Nothing seals its size or content, so neither side trusts the other to
// leave it alone:
//   - the host reads a result once, an exact byte count, into its own memory and validates that
//     copy; a helper that shrinks, grows or rewrites the file gets a short read or a rejected
//     payload, never a second look at changed bytes;
//   - the helper copies an input into its own memory before it parses it;
//   - the helper lowers its own file-size limit before it touches untrusted bytes, so a
//     compromised helper cannot fill the disk through a descriptor it was given.
//
// The private temporary directory of the user, with its trailing slash; empty when unavailable.
inline std::string lutPrivateDirectory() {
    std::vector<char> buffer(1024);
    const auto length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buffer.data(), buffer.size());
    if (length == 0 || length > buffer.size())
        return {};
    return std::string(buffer.data());
}
inline int sharedFile() {
    auto path = lutPrivateDirectory();
    if (path.empty())
        return -1;
    path += "bloom-lut-XXXXXX";
    const int fd = ::mkstemp(path.data());
    if (fd < 0)
        return -1;
    const bool anonymous = ::unlink(path.c_str()) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    if (!anonymous) {
        ::close(fd);
        return -1;
    }
    return fd;
}
// There is nothing to seal; see above for what stands in for it.
inline bool sealExtent(const int) { return true; }
inline bool sealInput(const int) { return true; }
// A regular file with no name, of the announced size and identity.
inline bool validSharedFile(const int fd, const std::uint64_t expectedInode,
                            const std::uint64_t bytes, const bool) {
    struct stat info{};
    return ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 0 &&
           info.st_size >= 0 && static_cast<std::uint64_t>(info.st_size) == bytes &&
           static_cast<std::uint64_t>(info.st_ino) == expectedInode;
}
#endif
#elif defined(_WIN32)
// Windows passes no descriptors. A shared file here is an unnamed section of the paging file: it
// has no name, is in no file system and its size is fixed when it is made. The host gives the
// helper its own handle to it (ProcessSupervisor::shareHandle), with read access only for an
// input, so the helper cannot change an input and neither side can resize anything. The helper's
// handle value travels in the packet field that carries the file identity on the other systems.
class LutSection final {
  public:
    explicit LutSection(HANDLE value = nullptr) : value_(value) {}
    ~LutSection() {
        if (value_ != nullptr)
            CloseHandle(value_);
    }
    LutSection(const LutSection&) = delete;
    LutSection& operator=(const LutSection&) = delete;
    LutSection(LutSection&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] bool valid() const { return value_ != nullptr; }

  private:
    HANDLE value_;
};
inline HANDLE createSection(const std::uint64_t bytes) {
    if (bytes == 0)
        return nullptr;
    return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                              static_cast<DWORD>(bytes >> 32U),
                              static_cast<DWORD>(bytes & 0xffffffffULL), nullptr);
}
// The first `bytes` of a section, mapped for reading or for reading and writing.
class LutView final {
  public:
    LutView(const HANDLE section, const std::uint64_t bytes, const bool writable)
        : data_(section == nullptr || bytes == 0
                    ? nullptr
                    : MapViewOfFile(section, writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0,
                                    static_cast<SIZE_T>(bytes))) {}
    ~LutView() {
        if (data_ != nullptr)
            UnmapViewOfFile(data_);
    }
    LutView(const LutView&) = delete;
    LutView& operator=(const LutView&) = delete;
    [[nodiscard]] void* data() const { return data_; }
    [[nodiscard]] bool valid() const { return data_ != nullptr; }

  private:
    void* data_;
};
inline bool writeSection(const HANDLE section, const std::span<const std::byte> bytes) {
    const LutView view(section, bytes.size(), true);
    if (!view.valid())
        return false;
    std::memcpy(view.data(), bytes.data(), bytes.size());
    return true;
}
inline bool readSection(const HANDLE section, const std::span<std::byte> bytes) {
    const LutView view(section, bytes.size(), false);
    if (!view.valid())
        return false;
    std::memcpy(bytes.data(), view.data(), bytes.size());
    return true;
}
// A section of at least the announced size that this process can read, and can write exactly when
// it is not an input.
inline bool validSharedSection(const HANDLE section, const std::uint64_t bytes,
                               const bool readOnly) {
    const LutView view(section, bytes, false);
    MEMORY_BASIC_INFORMATION info{};
    if (!view.valid() || VirtualQuery(view.data(), &info, sizeof(info)) == 0 ||
        info.Type != MEM_MAPPED || info.RegionSize < bytes)
        return false;
    const LutView writable(section, bytes, true);
    return writable.valid() != readOnly;
}
// The helper's standard input and output are pipes.
inline bool writeFd(const HANDLE pipe, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        DWORD count = 0;
        if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
            count == 0)
            return false;
        bytes = bytes.subspan(count);
    }
    return true;
}
inline bool readFd(const HANDLE pipe, std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        DWORD count = 0;
        if (!ReadFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
            count == 0)
            return false;
        bytes = bytes.subspan(count);
    }
    return true;
}
#endif
} // namespace bloom::color::detail

#include <bloom/platform/positional_file.hpp>

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bloom::platform {

std::unique_ptr<PositionalFile> PositionalFile::open(const std::filesystem::path& path,
                                                     const bool create) {
    if (create) {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
#if defined(_WIN32)
    // Others may read, write and remove the file while it is open here: an eviction takes the
    // name away and this handle keeps working on the bytes until it closes.
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return nullptr;
    if (create && GetLastError() != ERROR_ALREADY_EXISTS) {
        DWORD returned = 0;
        static_cast<void>(
            DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
    }
    return std::unique_ptr<PositionalFile>(new PositionalFile(handle));
#else
    const int descriptor =
        ::open(path.c_str(), O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
    if (descriptor < 0)
        return nullptr;
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(descriptor);
        return nullptr;
    }
    return std::unique_ptr<PositionalFile>(new PositionalFile(descriptor));
#endif
}

PositionalFile::~PositionalFile() {
#if defined(_WIN32)
    if (handle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(handle_));
#else
    if (descriptor_ >= 0)
        ::close(descriptor_);
#endif
}

bool PositionalFile::readAt(std::uint64_t offset, std::span<std::byte> bytes) const {
    while (!bytes.empty()) {
#if defined(_WIN32)
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        const auto ask =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::size_t{1} << 30U));
        DWORD count = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), bytes.data(), ask, &count, &position) ||
            count == 0)
            return false;
#else
        ssize_t count = 0;
        do {
            count = ::pread(descriptor_, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0)
            return false;
#endif
        bytes = bytes.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return true;
}

bool PositionalFile::writeAt(std::uint64_t offset, std::span<const std::byte> bytes) const {
    while (!bytes.empty()) {
#if defined(_WIN32)
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        const auto ask =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::size_t{1} << 30U));
        DWORD count = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), bytes.data(), ask, &count, &position) ||
            count == 0)
            return false;
#else
        ssize_t count = 0;
        do {
            count = ::pwrite(descriptor_, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0)
            return false;
#endif
        bytes = bytes.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return true;
}

std::optional<std::uint64_t> PositionalFile::size() const {
#if defined(_WIN32)
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &value) || value.QuadPart < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(value.QuadPart);
#else
    struct stat status{};
    if (::fstat(descriptor_, &status) != 0 || status.st_size < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(status.st_size);
#endif
}

} // namespace bloom::platform

#include "mapped_file.hpp"

#include <cstdint>
#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bloom::update::detail {

std::optional<MappedFile> MappedFile::open(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER length{};
    if (GetFileSizeEx(file, &length) == 0 || length.QuadPart < 0 ||
        static_cast<std::uint64_t>(length.QuadPart) > std::numeric_limits<std::size_t>::max()) {
        CloseHandle(file);
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(length.QuadPart);
    if (size == 0) {
        CloseHandle(file);
        return MappedFile(nullptr, 0);
    }
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (mapping == nullptr) {
        return std::nullopt;
    }
    void* const address = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (address == nullptr) {
        return std::nullopt;
    }
    return MappedFile(address, size);
#else
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return std::nullopt;
    }
    struct stat status {};
    if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > std::numeric_limits<std::size_t>::max()) {
        ::close(file);
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(status.st_size);
    if (size == 0) {
        ::close(file);
        return MappedFile(nullptr, 0);
    }
    void* const address = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
    ::close(file);
    if (address == MAP_FAILED) {
        return std::nullopt;
    }
    return MappedFile(address, size);
#endif
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : address_(std::exchange(other.address_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        release();
        address_ = std::exchange(other.address_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

MappedFile::~MappedFile() {
    release();
}

void MappedFile::release() noexcept {
    if (address_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    UnmapViewOfFile(address_);
#else
    ::munmap(address_, size_);
#endif
    address_ = nullptr;
    size_ = 0;
}

} // namespace bloom::update::detail

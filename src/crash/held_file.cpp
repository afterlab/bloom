#include "crash_internal.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bloom::crash::detail {

HeldFile::HeldFile(HeldFile&& other) noexcept
    : path_(std::move(other.path_)), handle_(std::exchange(other.handle_, -1)) {}

HeldFile& HeldFile::operator=(HeldFile&& other) noexcept {
    if (this != &other) {
        close(false);
        path_ = std::move(other.path_);
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

HeldFile::~HeldFile() { close(false); }

bool HeldFile::isOpen() const noexcept { return handle_ != -1; }

bool HeldFile::open(const std::filesystem::path& path) { return openLocked(path, false); }

bool HeldFile::openWaiting(const std::filesystem::path& path) { return openLocked(path, true); }

#if defined(_WIN32)

namespace {
// The locked byte: at 2^62, beyond any file this module writes.
constexpr DWORD kLockOffsetHigh = 0x40000000U;
} // namespace

bool HeldFile::openLocked(const std::filesystem::path& path, const bool wait) {
    close(false);
    // Others may read and write (the crash handler copies it), never delete: removing it fails
    // while this process holds it. The handle is not inherited by programs this one starts.
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < (wait ? 600 : 1); ++attempt) {
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_SHARING_VIOLATION || !wait)
            return false;
        Sleep(50);
    }
    if (file == INVALID_HANDLE_VALUE)
        return false;
    // A Windows byte-range lock is mandatory: a locked range cannot be read by another process,
    // and the crash handler must read the whole file while this process is suspended. So the
    // lock covers one byte far past any content, which only another lock can collide with.
    OVERLAPPED overlapped{};
    overlapped.OffsetHigh = kLockOffsetHigh;
    const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    if (LockFileEx(file, flags, 0, 1, 0, &overlapped) == 0) {
        CloseHandle(file);
        return false;
    }
    handle_ = reinterpret_cast<std::intptr_t>(file);
    path_ = path;
    return true;
}

bool HeldFile::replace(const std::string_view content) noexcept {
    if (!isOpen())
        return false;
    const HANDLE file = reinterpret_cast<HANDLE>(handle_);
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) == 0 || SetEndOfFile(file) == 0)
        return false;
    return append(content);
}

bool HeldFile::append(std::string_view content) noexcept {
    if (!isOpen())
        return false;
    const HANDLE file = reinterpret_cast<HANDLE>(handle_);
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(file, zero, nullptr, FILE_END) == 0)
        return false;
    while (!content.empty()) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(content.size(), 1U << 20));
        if (WriteFile(file, content.data(), chunk, &written, nullptr) == 0 || written == 0)
            return false;
        content.remove_prefix(written);
    }
    return true;
}

std::uint64_t HeldFile::size() const noexcept {
    LARGE_INTEGER size{};
    if (!isOpen() || GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &size) == 0)
        return 0;
    return static_cast<std::uint64_t>(size.QuadPart);
}

void HeldFile::close(const bool remove) noexcept {
    if (!isOpen())
        return;
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = -1;
    if (remove) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
}

bool heldByAnotherProcess(const std::filesystem::path& path) {
    // A held file cannot be opened for deletion; a free one can.
    const HANDLE file =
        CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(file);
    return false;
}

#else

bool HeldFile::openLocked(const std::filesystem::path& path, const bool wait) {
    close(false);
    // Close-on-exec: a program this one starts must not keep the lock alive after it exits.
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
        return false;
    int result = 0;
    do {
        result = flock(descriptor, LOCK_EX | (wait ? 0 : LOCK_NB));
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        ::close(descriptor);
        return false;
    }
    handle_ = descriptor;
    path_ = path;
    return true;
}

bool HeldFile::replace(const std::string_view content) noexcept {
    if (!isOpen())
        return false;
    if (ftruncate(static_cast<int>(handle_), 0) != 0)
        return false;
    return append(content);
}

bool HeldFile::append(std::string_view content) noexcept {
    if (!isOpen())
        return false;
    const int descriptor = static_cast<int>(handle_);
    if (lseek(descriptor, 0, SEEK_END) < 0)
        return false;
    while (!content.empty()) {
        const ssize_t written = ::write(descriptor, content.data(), content.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        content.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

std::uint64_t HeldFile::size() const noexcept {
    struct stat status{};
    if (!isOpen() || fstat(static_cast<int>(handle_), &status) != 0)
        return 0;
    return static_cast<std::uint64_t>(status.st_size);
}

void HeldFile::close(const bool remove) noexcept {
    if (!isOpen())
        return;
    if (remove)
        ::unlink(path_.c_str());
    ::close(static_cast<int>(handle_));
    handle_ = -1;
}

bool heldByAnotherProcess(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    const bool held = flock(descriptor, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(descriptor);
    return held;
}

#endif

} // namespace bloom::crash::detail

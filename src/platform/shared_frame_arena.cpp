#include <bloom/platform/shared_frame_arena.hpp>

#include <limits>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bloom::platform {
#if defined(__linux__)
namespace {
// The child's descriptor for the arena, installed by ProcessSupervisor::launch.
constexpr int kArenaDescriptor = 6;
} // namespace

ProcessResult<std::unique_ptr<SharedFrameArena>> SharedFrameArena::create(const std::uint64_t bytes) {
    if (bytes == 0 || bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return ProcessFailure{ProcessError::ResourceLimit, EINVAL};
    const int descriptor = ::memfd_create("bloom-frame-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0)
        return ProcessFailure{ProcessError::Unavailable, errno};
    const auto fail = [descriptor](const ProcessError code) {
        const int saved = errno;
        ::close(descriptor);
        return ProcessFailure{code, saved};
    };
    if (::ftruncate(descriptor, static_cast<off_t>(bytes)) != 0)
        return fail(ProcessError::ResourceLimit);
    // A worker cannot resize the region under the host's mapping (a shrink would fault the host).
    if (::fcntl(descriptor, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
        return fail(ProcessError::Unavailable);
    void* mapping = ::mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ, MAP_SHARED,
                           descriptor, 0);
    if (mapping == MAP_FAILED)
        return fail(ProcessError::ResourceLimit);
    return std::unique_ptr<SharedFrameArena>(
        new SharedFrameArena(descriptor, static_cast<const std::byte*>(mapping), bytes));
}

SharedFrameArena::~SharedFrameArena() {
    if (mapping_ != nullptr)
        ::munmap(const_cast<std::byte*>(mapping_), static_cast<std::size_t>(size_));
    if (descriptor_ >= 0)
        ::close(descriptor_);
}

std::span<const std::byte> SharedFrameArena::bytes() const noexcept {
    return {mapping_, static_cast<std::size_t>(size_)};
}

ProcessResult<std::unique_ptr<WorkerFrameArena>> WorkerFrameArena::open() {
    struct stat status{};
    const int flags = ::fcntl(kArenaDescriptor, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDWR || ::fstat(kArenaDescriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0)
        return ProcessFailure{ProcessError::Io, EINVAL};
    const auto size = static_cast<std::uint64_t>(status.st_size);
    void* mapping = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE,
                           MAP_SHARED, kArenaDescriptor, 0);
    if (mapping == MAP_FAILED)
        return ProcessFailure{ProcessError::ResourceLimit, errno};
    // The mapping keeps the region; the descriptor is not needed again.
    ::close(kArenaDescriptor);
    return std::unique_ptr<WorkerFrameArena>(
        new WorkerFrameArena(static_cast<std::byte*>(mapping), size));
}

WorkerFrameArena::~WorkerFrameArena() {
    if (mapping_ != nullptr)
        ::munmap(mapping_, static_cast<std::size_t>(size_));
}

std::span<std::byte> WorkerFrameArena::bytes() const noexcept {
    return {mapping_, static_cast<std::size_t>(size_)};
}
#else
ProcessResult<std::unique_ptr<SharedFrameArena>> SharedFrameArena::create(std::uint64_t) {
    return ProcessFailure{ProcessError::Unavailable, 0};
}
SharedFrameArena::~SharedFrameArena() = default;
std::span<const std::byte> SharedFrameArena::bytes() const noexcept {
    return {mapping_, static_cast<std::size_t>(size_)};
}
ProcessResult<std::unique_ptr<WorkerFrameArena>> WorkerFrameArena::open() {
    return ProcessFailure{ProcessError::Unavailable, 0};
}
WorkerFrameArena::~WorkerFrameArena() = default;
std::span<std::byte> WorkerFrameArena::bytes() const noexcept {
    return {mapping_, static_cast<std::size_t>(size_)};
}
#endif
} // namespace bloom::platform

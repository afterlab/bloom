#pragma once
#include <bloom/platform/process_supervisor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bloom::platform {
// Issue #474: anonymous shared memory a retained media worker writes decoded frame planes into, so
// a frame crosses to the host as one copy instead of through the pipe (media-io.md "Shared frame
// arena"). The host creates it, maps it read-only and installs a writable descriptor as the
// child's fd 6; the size is sealed, so the worker cannot shrink it under the host's mapping.
// Linux only (memfd); elsewhere create() reports Unavailable and the frame crosses the pipe.
class SharedFrameArena final {
  public:
    [[nodiscard]] static ProcessResult<std::unique_ptr<SharedFrameArena>>
    create(std::uint64_t bytes);
    ~SharedFrameArena();
    SharedFrameArena(const SharedFrameArena&) = delete;
    SharedFrameArena& operator=(const SharedFrameArena&) = delete;
    // The host's read-only view. A worker may still be writing it; the host copies a frame out
    // before it asks for the next one, and validates every offset against size() first.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

  private:
    friend class ProcessSupervisor;
    SharedFrameArena(int descriptor, const std::byte* mapping, std::uint64_t size) noexcept
        : descriptor_(descriptor), mapping_(mapping), size_(size) {}
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    int descriptor_ = -1;
    const std::byte* mapping_ = nullptr;
    std::uint64_t size_ = 0;
};
// The child's writable view of the arena installed as fd 6 by launch().
class WorkerFrameArena final {
  public:
    [[nodiscard]] static ProcessResult<std::unique_ptr<WorkerFrameArena>> open();
    ~WorkerFrameArena();
    WorkerFrameArena(const WorkerFrameArena&) = delete;
    WorkerFrameArena& operator=(const WorkerFrameArena&) = delete;
    [[nodiscard]] std::span<std::byte> bytes() const noexcept;

  private:
    WorkerFrameArena(std::byte* mapping, std::uint64_t size) noexcept
        : mapping_(mapping), size_(size) {}
    std::byte* mapping_ = nullptr;
    std::uint64_t size_ = 0;
};
} // namespace bloom::platform

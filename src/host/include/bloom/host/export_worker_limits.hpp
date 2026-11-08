#pragma once

#include <cstdint>

namespace bloom::host {

// Opt-in limits on the media worker processes an export launches (the encode worker). Both default
// to "no limit": a run that sets neither behaves exactly as it did before they existed.
struct ExportWorkerLimits final {
    // The most resident memory a worker may hold, in bytes. At the cap the export slows or waits
    // until the worker has finished what it holds; a worker that stays at the cap with nothing left
    // to finish fails the export with a plain sentence. Zero: no cap.
    std::uint64_t memoryCapBytes = 0;
    // Encoder threads in the worker, 1..kMaximumExportWorkerThreads. Zero: automatic (the number
    // the worker picks for the machine and the frame size).
    std::uint32_t encoderThreads = 0;
    friend bool operator==(const ExportWorkerLimits&, const ExportWorkerLimits&) = default;
    [[nodiscard]] constexpr bool any() const noexcept {
        return memoryCapBytes != 0 || encoderThreads != 0;
    }
};

// A cap above this is refused as a typo (1 TiB); the cap itself has no other floor or ceiling.
inline constexpr std::uint64_t kMaximumExportWorkerMemoryCapBytes = std::uint64_t{1} << 40U;
// FFmpeg's frame-threaded encoder maximum (media::provider::kEncodeMaximumThreads).
inline constexpr std::uint32_t kMaximumExportWorkerThreads = 64;

[[nodiscard]] constexpr bool validExportWorkerLimits(const ExportWorkerLimits& limits) noexcept {
    return limits.memoryCapBytes <= kMaximumExportWorkerMemoryCapBytes &&
           limits.encoderThreads <= kMaximumExportWorkerThreads;
}

} // namespace bloom::host

#ifndef BLOOM_RENDER_VULKAN_GPU_PIPELINE_CACHE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_PIPELINE_CACHE_PRIVATE_HPP

// Private to src/render/vulkan. #457: one VkPipelineCache per device generation, used by every
// pipeline creation, so a route rebuild on the same generation reuses compiled pipelines, and,
// when the device was created with a cache directory, persisted across launches.
//
// The file is derived runtime cache state in the per-user cache directory, never project state.
// It is a Bloom header followed by the driver's own pipeline-cache data:
//
//   offset  size  field
//        0     8  magic "BLMVKPC\0"
//        8     4  format version (1)
//       12     4  header bytes (80)
//       16     8  payload bytes
//       24     8  payload FNV-1a 64 checksum
//       32     4  vendor id
//       36     4  device id
//       40     4  driver version
//       44     4  reserved (0)
//       48    16  physical device UUID
//       64    16  pipeline cache UUID
//       80     -  payload (vkGetPipelineCacheData)
//
// all integers little-endian. A file is used only when every field matches this device and driver,
// the checksum matches, and the payload's own VkPipelineCacheHeaderVersionOne names the same
// vendor, device and pipeline-cache UUID; anything else (another GPU, a driver update, a truncated
// or corrupt write) is rejected and the cache starts empty. Payloads are bounded by
// kMaxPipelineCachePayloadBytes on read and on write, a directory keeps at most
// kMaxPipelineCacheFiles cache files, and writes go to a unique temporary file renamed over the
// target, so a reader never sees a partial file.

#include "gpu_device_private.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace bloom::render::vulkan_detail {

inline constexpr std::uint64_t kMaxPipelineCachePayloadBytes = 64ULL << 20;
inline constexpr std::size_t kMaxPipelineCacheFiles = 8;
inline constexpr std::size_t kPipelineCacheHeaderBytes = 80;

struct PipelineCacheIdentity final {
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t driverVersion = 0;
    std::array<std::uint8_t, 16> deviceUuid{};
    std::array<std::uint8_t, 16> pipelineCacheUuid{};
};

enum class PipelineCacheFileCheck : std::uint8_t {
    Valid,
    Truncated,
    BadMagic,
    UnsupportedVersion,
    SizeMismatch,
    TooLarge,
    ChecksumMismatch,
    ForeignDevice,
    ForeignDriver,
    ForeignVulkanHeader,
};

[[nodiscard]] const char* pipelineCacheFileCheckText(PipelineCacheFileCheck check) noexcept;

// Pure: validates a whole file against this identity. On Valid, `payload` views the driver data.
[[nodiscard]] PipelineCacheFileCheck checkPipelineCacheFile(std::span<const std::uint8_t> file,
                                                            const PipelineCacheIdentity& identity,
                                                            std::span<const std::uint8_t>& payload);
// Pure: the file bytes for `payload` under this identity.
[[nodiscard]] std::vector<std::uint8_t>
encodePipelineCacheFile(std::span<const std::uint8_t> payload,
                        const PipelineCacheIdentity& identity);
// The file name for this identity inside the cache directory.
[[nodiscard]] std::string pipelineCacheFileName(const PipelineCacheIdentity& identity);

// Bootstrap, owner thread, after the device exists: records the identity, reads and validates the
// file under `directory` (empty: memory only) and creates the cache. A cache that cannot be created
// leaves `cache` null; pipeline creation then passes VK_NULL_HANDLE exactly as before.
void createPipelineCache(DeviceAllocatorState& state, const VkPhysicalDeviceProperties& properties,
                         const std::array<std::uint8_t, 16>& deviceUuid,
                         const std::filesystem::path& directory) noexcept;

// The handle every pipeline creation passes; counts the creation.
[[nodiscard]] inline VkPipelineCache
pipelineCacheForCreation(DeviceAllocatorState& state) noexcept {
    state.pipelineCache.pipelinesCreated.fetch_add(1, std::memory_order_relaxed);
    return static_cast<VkPipelineCache>(*state.pipelineCache.cache);
}

// Owner thread. Writes the file when the cache is persisted and pipelines were created since the
// last write. Returns true when a file was written.
bool persistPipelineCache(DeviceAllocatorState& state) noexcept;

// Owner thread, before the device is cleared: a last write, then the cache is destroyed.
void closePipelineCache(DeviceAllocatorState& state) noexcept;

} // namespace bloom::render::vulkan_detail

#endif // BLOOM_RENDER_VULKAN_GPU_PIPELINE_CACHE_PRIVATE_HPP

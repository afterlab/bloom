#include "gpu_pipeline_cache_private.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>
#include <utility>

namespace bloom::render::vulkan_detail {

namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'B', 'L', 'M', 'V', 'K', 'P', 'C', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kVulkanHeaderBytes = 32; // VkPipelineCacheHeaderVersionOne
constexpr const char* kExtension = ".pipelinecache";
constexpr std::chrono::hours kStaleTemporaryAge{1};

[[nodiscard]] std::uint64_t fnv1a64(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

[[nodiscard]] std::uint32_t read32(const std::uint8_t* at) noexcept {
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8U) |
           (static_cast<std::uint32_t>(at[2]) << 16U) | (static_cast<std::uint32_t>(at[3]) << 24U);
}

[[nodiscard]] std::uint64_t read64(const std::uint8_t* at) noexcept {
    return static_cast<std::uint64_t>(read32(at)) |
           (static_cast<std::uint64_t>(read32(at + 4)) << 32U);
}

void write32(std::uint8_t* at, const std::uint32_t value) noexcept {
    for (unsigned index = 0; index < 4; ++index)
        at[index] = static_cast<std::uint8_t>(value >> (8U * index));
}

void write64(std::uint8_t* at, const std::uint64_t value) noexcept {
    write32(at, static_cast<std::uint32_t>(value));
    write32(at + 4, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] std::string hex(const std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kDigits[byte >> 4U]);
        text.push_back(kDigits[byte & 0xFU]);
    }
    return text;
}

[[nodiscard]] std::string hex32(const std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{};
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8U * (3 - index)));
    return hex(bytes);
}

[[nodiscard]] PipelineCacheIdentity identityOf(const PipelineCacheState& cache) noexcept {
    return {cache.vendorId, cache.deviceId, cache.driverVersion, cache.deviceUuid,
            cache.pipelineCacheUuid};
}

void setDetail(PipelineCacheState& cache, const char* text) noexcept {
    try {
        cache.detail = text;
    } catch (...) {
        cache.detail.clear();
    }
}

// Keeps the newest kMaxPipelineCacheFiles cache files and removes temporary files a crashed
// writer left behind. Never touches anything but Bloom's own cache file names.
void pruneDirectory(const std::filesystem::path& directory,
                    const std::filesystem::path& keep) noexcept {
    try {
        std::error_code error;
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> caches;
        const auto now = std::filesystem::file_time_type::clock::now();
        for (std::filesystem::directory_iterator entry(directory, error), end;
             !error && entry != end; entry.increment(error)) {
            if (!entry->is_regular_file(error))
                continue;
            const auto& path = entry->path();
            const std::string name = path.filename().string();
            if (name.rfind("vulkan-", 0) != 0)
                continue;
            const auto modified = entry->last_write_time(error);
            if (error)
                continue;
            if (name.find(std::string(kExtension) + ".tmp-") != std::string::npos) {
                if (now - modified > kStaleTemporaryAge)
                    std::filesystem::remove(path, error);
                continue;
            }
            if (path.extension() == kExtension && path != keep)
                caches.emplace_back(modified, path);
        }
        if (caches.size() + 1 <= kMaxPipelineCacheFiles)
            return;
        std::sort(caches.begin(), caches.end());
        const std::size_t excess = caches.size() + 1 - kMaxPipelineCacheFiles;
        for (std::size_t index = 0; index < excess; ++index)
            std::filesystem::remove(caches[index].second, error);
    } catch (...) { // NOLINT(bugprone-empty-catch): pruning is best effort
    }
}

[[nodiscard]] bool writeFileAtomically(const std::filesystem::path& file,
                                       const std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return false;
    std::random_device random;
    const std::uint64_t salt =
        (static_cast<std::uint64_t>(random()) << 32U) ^ random() ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::array<std::uint8_t, 8> saltBytes{};
    write64(saltBytes.data(), salt);
    std::filesystem::path temporary = file;
    temporary += ".tmp-" + hex(saltBytes);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace

const char* pipelineCacheFileCheckText(const PipelineCacheFileCheck check) noexcept {
    switch (check) {
    case PipelineCacheFileCheck::Valid:
        return "valid";
    case PipelineCacheFileCheck::Truncated:
        return "the pipeline cache file is truncated";
    case PipelineCacheFileCheck::BadMagic:
        return "the pipeline cache file is not a Bloom pipeline cache";
    case PipelineCacheFileCheck::UnsupportedVersion:
        return "the pipeline cache file has an unsupported format version";
    case PipelineCacheFileCheck::SizeMismatch:
        return "the pipeline cache file size does not match its header";
    case PipelineCacheFileCheck::TooLarge:
        return "the pipeline cache file exceeds the size bound";
    case PipelineCacheFileCheck::ChecksumMismatch:
        return "the pipeline cache payload checksum does not match";
    case PipelineCacheFileCheck::ForeignDevice:
        return "the pipeline cache file belongs to another GPU";
    case PipelineCacheFileCheck::ForeignDriver:
        return "the pipeline cache file was written by another driver version";
    case PipelineCacheFileCheck::ForeignVulkanHeader:
        return "the driver pipeline-cache header does not match this device";
    }
    return "the pipeline cache file was rejected";
}

PipelineCacheFileCheck checkPipelineCacheFile(const std::span<const std::uint8_t> file,
                                              const PipelineCacheIdentity& identity,
                                              std::span<const std::uint8_t>& payload) {
    payload = {};
    if (file.size() < kPipelineCacheHeaderBytes)
        return PipelineCacheFileCheck::Truncated;
    const std::uint8_t* header = file.data();
    if (!std::equal(kMagic.begin(), kMagic.end(), header))
        return PipelineCacheFileCheck::BadMagic;
    if (read32(header + 8) != kFormatVersion || read32(header + 12) != kPipelineCacheHeaderBytes)
        return PipelineCacheFileCheck::UnsupportedVersion;
    const std::uint64_t payloadBytes = read64(header + 16);
    if (payloadBytes > kMaxPipelineCachePayloadBytes)
        return PipelineCacheFileCheck::TooLarge;
    if (file.size() - kPipelineCacheHeaderBytes != payloadBytes)
        return PipelineCacheFileCheck::SizeMismatch;
    const auto body = file.subspan(kPipelineCacheHeaderBytes);
    if (fnv1a64(body) != read64(header + 24))
        return PipelineCacheFileCheck::ChecksumMismatch;
    if (read32(header + 32) != identity.vendorId || read32(header + 36) != identity.deviceId ||
        !std::equal(identity.deviceUuid.begin(), identity.deviceUuid.end(), header + 48))
        return PipelineCacheFileCheck::ForeignDevice;
    if (read32(header + 40) != identity.driverVersion ||
        !std::equal(identity.pipelineCacheUuid.begin(), identity.pipelineCacheUuid.end(),
                    header + 64))
        return PipelineCacheFileCheck::ForeignDriver;
    // VkPipelineCacheHeaderVersionOne: headerSize, headerVersion, vendorID, deviceID, UUID.
    if (body.size() < kVulkanHeaderBytes)
        return PipelineCacheFileCheck::ForeignVulkanHeader;
    const std::uint32_t vulkanHeaderBytes = read32(body.data());
    if (vulkanHeaderBytes < kVulkanHeaderBytes || vulkanHeaderBytes > body.size() ||
        read32(body.data() + 4) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        read32(body.data() + 8) != identity.vendorId ||
        read32(body.data() + 12) != identity.deviceId ||
        !std::equal(identity.pipelineCacheUuid.begin(), identity.pipelineCacheUuid.end(),
                    body.data() + 16))
        return PipelineCacheFileCheck::ForeignVulkanHeader;
    payload = body;
    return PipelineCacheFileCheck::Valid;
}

std::vector<std::uint8_t> encodePipelineCacheFile(const std::span<const std::uint8_t> payload,
                                                  const PipelineCacheIdentity& identity) {
    std::vector<std::uint8_t> file(kPipelineCacheHeaderBytes + payload.size(), 0);
    std::uint8_t* header = file.data();
    std::copy(kMagic.begin(), kMagic.end(), header);
    write32(header + 8, kFormatVersion);
    write32(header + 12, static_cast<std::uint32_t>(kPipelineCacheHeaderBytes));
    write64(header + 16, payload.size());
    write64(header + 24, fnv1a64(payload));
    write32(header + 32, identity.vendorId);
    write32(header + 36, identity.deviceId);
    write32(header + 40, identity.driverVersion);
    std::copy(identity.deviceUuid.begin(), identity.deviceUuid.end(), header + 48);
    std::copy(identity.pipelineCacheUuid.begin(), identity.pipelineCacheUuid.end(), header + 64);
    if (!payload.empty())
        std::memcpy(header + kPipelineCacheHeaderBytes, payload.data(), payload.size());
    return file;
}

std::string pipelineCacheFileName(const PipelineCacheIdentity& identity) {
    return "vulkan-" + hex32(identity.vendorId) + "-" + hex32(identity.deviceId) + "-" +
           hex(identity.deviceUuid) + kExtension;
}

void createPipelineCache(DeviceAllocatorState& state, const VkPhysicalDeviceProperties& properties,
                         const std::array<std::uint8_t, 16>& deviceUuid,
                         const std::filesystem::path& directory) noexcept {
    auto& cache = state.pipelineCache;
    cache.vendorId = properties.vendorID;
    cache.deviceId = properties.deviceID;
    cache.driverVersion = properties.driverVersion;
    cache.deviceUuid = deviceUuid;
    std::copy(std::begin(properties.pipelineCacheUUID), std::end(properties.pipelineCacheUUID),
              cache.pipelineCacheUuid.begin());
    std::vector<std::uint8_t> file;
    std::span<const std::uint8_t> payload;
    try {
        if (!directory.empty()) {
            cache.file = directory / pipelineCacheFileName(identityOf(cache));
            cache.load = GpuPipelineCacheLoad::Missing;
            std::error_code error;
            const auto bytes = std::filesystem::file_size(cache.file, error);
            if (!error) {
                PipelineCacheFileCheck check = PipelineCacheFileCheck::TooLarge;
                if (bytes <= kPipelineCacheHeaderBytes + kMaxPipelineCachePayloadBytes) {
                    file.resize(static_cast<std::size_t>(bytes));
                    std::ifstream in(cache.file, std::ios::binary);
                    in.read(reinterpret_cast<char*>(file.data()),
                            static_cast<std::streamsize>(file.size()));
                    check = in && in.gcount() == static_cast<std::streamsize>(file.size())
                                ? checkPipelineCacheFile(file, identityOf(cache), payload)
                                : PipelineCacheFileCheck::Truncated;
                }
                if (check == PipelineCacheFileCheck::Valid) {
                    cache.load = GpuPipelineCacheLoad::Loaded;
                    cache.loadedBytes = payload.size();
                } else {
                    cache.load = GpuPipelineCacheLoad::Rejected;
                    setDetail(cache, pipelineCacheFileCheckText(check));
                    payload = {};
                }
            }
        }
    } catch (...) {
        if (cache.load == GpuPipelineCacheLoad::Loaded) {
            cache.load = GpuPipelineCacheLoad::Rejected;
            cache.loadedBytes = 0;
        }
        payload = {};
    }
    const VkDevice device = static_cast<VkDevice>(*state.device);
    const auto* dispatcher = state.device.getDispatcher();
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = payload.size();
    info.pInitialData = payload.empty() ? nullptr : payload.data();
    VkPipelineCache raw = VK_NULL_HANDLE;
    VkResult created = dispatcher->vkCreatePipelineCache(device, &info, nullptr, &raw);
    if (created != VK_SUCCESS && !payload.empty()) {
        // The driver refused data that passed every header check: start empty instead.
        cache.load = GpuPipelineCacheLoad::Rejected;
        cache.loadedBytes = 0;
        setDetail(cache, "the driver refused the pipeline cache data");
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        created = dispatcher->vkCreatePipelineCache(device, &info, nullptr, &raw);
    }
    if (created == VK_SUCCESS)
        cache.cache = vk::raii::PipelineCache(state.device, raw);
}

bool persistPipelineCache(DeviceAllocatorState& state) noexcept {
    auto& cache = state.pipelineCache;
    if (state.owner != std::this_thread::get_id() || cache.file.empty() ||
        *cache.cache == vk::PipelineCache{})
        return false;
    const std::uint64_t created = cache.pipelinesCreated.load(std::memory_order_relaxed);
    if (created == cache.pipelinesAtLastWrite)
        return false;
    try {
        const VkDevice device = static_cast<VkDevice>(*state.device);
        const auto* dispatcher = state.device.getDispatcher();
        const VkPipelineCache raw = static_cast<VkPipelineCache>(*cache.cache);
        std::size_t bytes = 0;
        if (dispatcher->vkGetPipelineCacheData(device, raw, &bytes, nullptr) != VK_SUCCESS)
            return false;
        if (bytes > kMaxPipelineCachePayloadBytes) {
            cache.pipelinesAtLastWrite = created;
            setDetail(cache, "the pipeline cache exceeds the size bound; it was not written");
            return false;
        }
        std::vector<std::uint8_t> payload(bytes);
        const VkResult read =
            dispatcher->vkGetPipelineCacheData(device, raw, &bytes, payload.data());
        // VK_INCOMPLETE: pipelines created meanwhile on another thread; the next write has them.
        if (read != VK_SUCCESS && read != VK_INCOMPLETE)
            return false;
        payload.resize(bytes);
        const auto file = encodePipelineCacheFile(payload, identityOf(cache));
        cache.pipelinesAtLastWrite = created;
        if (!writeFileAtomically(cache.file, file)) {
            setDetail(cache, "the pipeline cache file could not be written");
            return false;
        }
        ++cache.writes;
        cache.lastWriteBytes = file.size();
        pruneDirectory(cache.file.parent_path(), cache.file);
        return true;
    } catch (...) {
        return false;
    }
}

void closePipelineCache(DeviceAllocatorState& state) noexcept {
    static_cast<void>(persistPipelineCache(state));
    state.pipelineCache.cache = vk::raii::PipelineCache{nullptr};
}

} // namespace bloom::render::vulkan_detail

// #457: the device generation's VkPipelineCache. The pure checks pin the on-disk format: a file is
// used only when its Bloom header, checksum, device, driver and the driver's own pipeline-cache
// header all match, so a stale, foreign, truncated or corrupt file is rejected. The device checks
// prove the real cycle on this machine: a cold cache is written off the pipelines a route created,
// the next device generation loads it and produces byte-identical coverage, a corrupt or foreign
// file is rejected without failing pipeline creation, a cache without a directory stays in memory,
// the directory stays bounded, and consecutive path-coverage shapes reuse one command set.

#include "gpu_device_private.hpp"
#include "gpu_path_coverage_test_support.hpp"
#include "gpu_pipeline_cache_private.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {
using namespace bloom::render;
using namespace bloom::render::vulkan_detail;
using bloom::render::gpu_path_coverage_test::Expectations;

[[nodiscard]] PipelineCacheIdentity identity() {
    PipelineCacheIdentity id;
    id.vendorId = 0x10de;
    id.deviceId = 0x2684;
    id.driverVersion = 0x9a0b4000;
    for (std::size_t index = 0; index < 16; ++index) {
        id.deviceUuid[index] = static_cast<std::uint8_t>(index + 1);
        id.pipelineCacheUuid[index] = static_cast<std::uint8_t>(0xA0 + index);
    }
    return id;
}

// A driver payload: VkPipelineCacheHeaderVersionOne naming `id`, then opaque bytes.
[[nodiscard]] std::vector<std::uint8_t> payloadFor(const PipelineCacheIdentity& id,
                                                   const std::size_t extra = 64) {
    std::vector<std::uint8_t> payload(32 + extra, 0);
    const auto put32 = [&payload](const std::size_t at, const std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index)
            payload[at + index] = static_cast<std::uint8_t>(value >> (8U * index));
    };
    put32(0, 32);
    put32(4, VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
    put32(8, id.vendorId);
    put32(12, id.deviceId);
    std::copy(id.pipelineCacheUuid.begin(), id.pipelineCacheUuid.end(), payload.begin() + 16);
    for (std::size_t index = 32; index < payload.size(); ++index)
        payload[index] = static_cast<std::uint8_t>(index * 7U);
    return payload;
}

[[nodiscard]] PipelineCacheFileCheck check(const std::vector<std::uint8_t>& file,
                                           const PipelineCacheIdentity& id) {
    std::span<const std::uint8_t> payload;
    return checkPipelineCacheFile(file, id, payload);
}

void testFileFormat(Expectations& checks) {
    const auto id = identity();
    const auto payload = payloadFor(id);
    const auto file = encodePipelineCacheFile(payload, id);
    std::span<const std::uint8_t> decoded;
    checks.expect(checkPipelineCacheFile(file, id, decoded) == PipelineCacheFileCheck::Valid &&
                      std::equal(decoded.begin(), decoded.end(), payload.begin(), payload.end()),
                  "an encoded cache round-trips to the same driver payload");
    checks.expect(file.size() == kPipelineCacheHeaderBytes + payload.size(),
                  "the file is the 80-byte header plus the payload");

    auto truncated = file;
    truncated.resize(40);
    checks.expect(check(truncated, id) == PipelineCacheFileCheck::Truncated,
                  "a file shorter than the header is truncated");
    auto shortPayload = file;
    shortPayload.pop_back();
    checks.expect(check(shortPayload, id) == PipelineCacheFileCheck::SizeMismatch,
                  "a payload shorter than its header says is rejected");
    auto magic = file;
    magic[0] = 'X';
    checks.expect(check(magic, id) == PipelineCacheFileCheck::BadMagic,
                  "another file format is rejected");
    auto version = file;
    version[8] = 2;
    checks.expect(check(version, id) == PipelineCacheFileCheck::UnsupportedVersion,
                  "another format version is rejected");
    auto corrupt = file;
    corrupt[kPipelineCacheHeaderBytes + 40] ^= 0x5AU;
    checks.expect(check(corrupt, id) == PipelineCacheFileCheck::ChecksumMismatch,
                  "a corrupt payload fails the checksum");
    auto huge = file;
    for (unsigned index = 0; index < 8; ++index)
        huge[16 + index] = 0xFF;
    checks.expect(check(huge, id) == PipelineCacheFileCheck::TooLarge,
                  "a header claiming more than the bound is rejected before any use");

    auto otherGpu = id;
    otherGpu.deviceUuid[3] ^= 1U;
    checks.expect(check(file, otherGpu) == PipelineCacheFileCheck::ForeignDevice,
                  "another physical GPU's cache is rejected");
    auto otherModel = id;
    otherModel.deviceId += 1;
    checks.expect(check(file, otherModel) == PipelineCacheFileCheck::ForeignDevice,
                  "another device model's cache is rejected");
    auto updatedDriver = id;
    updatedDriver.driverVersion += 1;
    checks.expect(check(file, updatedDriver) == PipelineCacheFileCheck::ForeignDriver,
                  "a cache written by another driver version is rejected");
    auto otherCacheUuid = id;
    otherCacheUuid.pipelineCacheUuid[0] ^= 1U;
    checks.expect(check(file, otherCacheUuid) == PipelineCacheFileCheck::ForeignDriver,
                  "a cache with another pipeline-cache UUID is rejected");

    // A Bloom header that matches but a driver header that does not.
    auto foreignPayload = payloadFor(id);
    foreignPayload[16] ^= 1U;
    checks.expect(check(encodePipelineCacheFile(foreignPayload, id), id) ==
                      PipelineCacheFileCheck::ForeignVulkanHeader,
                  "a driver header naming another pipeline-cache UUID is rejected");
    auto badHeaderVersion = payloadFor(id);
    badHeaderVersion[4] = 9;
    checks.expect(check(encodePipelineCacheFile(badHeaderVersion, id), id) ==
                      PipelineCacheFileCheck::ForeignVulkanHeader,
                  "an unknown driver header version is rejected");
    std::vector<std::uint8_t> tiny(8, 0);
    checks.expect(check(encodePipelineCacheFile(tiny, id), id) ==
                      PipelineCacheFileCheck::ForeignVulkanHeader,
                  "a payload without a driver header is rejected");
    checks.expect(pipelineCacheFileName(id) ==
                      "vulkan-000010de-00002684-0102030405060708090a0b0c0d0e0f10.pipelinecache",
                  "the file name carries the vendor, device and physical device UUID");
}

[[nodiscard]] std::filesystem::path temporaryDirectory() {
    std::random_device random;
    const auto name =
        "bloom-pipeline-cache-test-" + std::to_string(random()) + "-" + std::to_string(random());
    return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::unique_ptr<GpuDevice> deviceWith(const std::filesystem::path& loader,
                                                    const std::filesystem::path& directory) {
    GpuDeviceCreationOptions options;
    options.loader_path = loader;
    options.pipeline_cache_directory = directory;
    auto created = GpuDevice::create(options);
    return std::move(created.device);
}

// One coverage mask from a fresh GpuPathCoverage, whose pipeline goes through the cache.
[[nodiscard]] std::vector<std::uint8_t> coverageOn(Expectations& checks, GpuDevice& device) {
    using namespace bloom::render::gpu_path_coverage_test;
    auto produced = GpuPathCoverage::create(device);
    checks.expect(produced.hasValue(), "the coverage producer is created");
    if (!produced)
        return {};
    const auto window = ImageWindow::create(0, 0, 24, 16);
    if (!window)
        return {};
    const auto raster = rasterOf(PathRaster::create(ellipsePath(20.0, 12.0), {}, 1, 1));
    std::vector<std::uint8_t> mask;
    checks.expect(runCoverage(*produced.coverage, *window.value(), PathFillRule::NonZero, false,
                              raster, mask),
                  "the coverage runs");
    return mask;
}

void testDeviceCycle(Expectations& checks, const std::filesystem::path& loader, bool& ran) {
    const auto directory = temporaryDirectory();
    std::vector<std::uint8_t> coldMask;
    std::filesystem::path file;
    {
        auto device = deviceWith(loader, directory);
        if (!device)
            return;
        ran = true;
        const auto cold = device->pipelineCacheStats();
        checks.expect(cold.active && cold.load == GpuPipelineCacheLoad::Missing &&
                          cold.file.parent_path() == directory,
                      "a first launch has an active cache and no file yet");
        checks.expect(!std::filesystem::exists(directory),
                      "the cache directory is created only by a write");
        auto state = GpuRendererAccess::state(*device);
        {
            using namespace bloom::render::gpu_path_coverage_test;
            auto produced = GpuPathCoverage::create(*device);
            checks.expect(produced.hasValue(), "the coverage producer is created");
            const auto window = ImageWindow::create(0, 0, 24, 16);
            if (produced && window) {
                const auto raster = rasterOf(PathRaster::create(ellipsePath(20.0, 12.0), {}, 1, 1));
                checks.expect(runCoverage(*produced.coverage, *window.value(),
                                          PathFillRule::NonZero, false, raster, coldMask),
                              "the cold coverage runs");
                const auto created = state->commandSetsCreated;
                const auto reused = state->commandSetsReused;
                std::vector<std::uint8_t> again;
                for (int shape = 0; shape < 3; ++shape)
                    checks.expect(runCoverage(*produced.coverage, *window.value(),
                                              PathFillRule::NonZero, false, raster, again) &&
                                      again == coldMask,
                                  "every later shape matches the first");
                checks.expect(state->commandSetsCreated == created &&
                                  state->commandSetsReused == reused + 3,
                              "consecutive coverage shapes reuse one command set");
            }
        }
        checks.expect(device->pipelineCacheStats().pipelinesCreated > 0,
                      "the route's pipelines were created through the cache");
        checks.expect(device->persistPipelineCache(), "the cold cache is written");
        checks.expect(!device->persistPipelineCache(),
                      "nothing is written again without new pipelines");
        const auto written = device->pipelineCacheStats();
        file = written.file;
        checks.expect(written.writes == 1 && written.lastWriteBytes > kPipelineCacheHeaderBytes &&
                          std::filesystem::file_size(file) == written.lastWriteBytes,
                      "the file holds the header and the driver's data");
    }
    {
        auto device = deviceWith(loader, directory);
        checks.expect(device != nullptr, "the warm device is created");
        if (device) {
            const auto warm = device->pipelineCacheStats();
            checks.expect(warm.load == GpuPipelineCacheLoad::Loaded && warm.loadedBytes > 0 &&
                              warm.loadedBytes + kPipelineCacheHeaderBytes ==
                                  std::filesystem::file_size(file),
                          "the next launch loads the validated file");
            checks.expect(coverageOn(checks, *device) == coldMask,
                          "a warm-cache pipeline produces byte-identical coverage");
        }
    }
    const auto good = readFile(file);
    {
        auto corrupt = good;
        corrupt[kPipelineCacheHeaderBytes + 20] ^= 0x33U;
        writeFile(file, corrupt);
        auto device = deviceWith(loader, directory);
        checks.expect(device != nullptr, "a corrupt cache never fails device creation");
        if (device) {
            const auto stats = device->pipelineCacheStats();
            checks.expect(stats.load == GpuPipelineCacheLoad::Rejected && stats.active &&
                              stats.loadedBytes == 0 &&
                              stats.detail.find("checksum") != std::string::npos,
                          "a corrupt cache is rejected and the cache starts empty");
            checks.expect(coverageOn(checks, *device) == coldMask,
                          "pipelines still build after a rejected cache");
            checks.expect(device->persistPipelineCache() && readFile(file) != corrupt,
                          "the next write replaces the rejected file");
        }
    }
    {
        // A valid file for this device, but written by another driver build.
        auto probe = deviceWith(loader, {});
        if (probe) {
            const auto& cache = GpuRendererAccess::state(*probe)->pipelineCache;
            PipelineCacheIdentity foreign{cache.vendorId, cache.deviceId, cache.driverVersion + 1,
                                          cache.deviceUuid, cache.pipelineCacheUuid};
            const std::vector<std::uint8_t> body(good.begin() + kPipelineCacheHeaderBytes,
                                                 good.end());
            writeFile(file, encodePipelineCacheFile(body, foreign));
        }
        auto device = deviceWith(loader, directory);
        if (device) {
            const auto stats = device->pipelineCacheStats();
            checks.expect(stats.load == GpuPipelineCacheLoad::Rejected &&
                              stats.detail.find("driver") != std::string::npos,
                          "a cache from another driver version is rejected");
        }
    }
    {
        auto device = deviceWith(loader, {});
        if (device) {
            const auto stats = device->pipelineCacheStats();
            checks.expect(stats.active && stats.load == GpuPipelineCacheLoad::NotPersistent &&
                              stats.file.empty(),
                          "without a directory the cache stays in memory");
            static_cast<void>(coverageOn(checks, *device));
            checks.expect(!device->persistPipelineCache(), "a memory-only cache writes nothing");
        }
    }
    {
        // The directory keeps at most kMaxPipelineCacheFiles cache files, newest first.
        const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
        for (int index = 0; index < 12; ++index) {
            const auto stale =
                directory / ("vulkan-stale-" + std::to_string(index) + ".pipelinecache");
            writeFile(stale, {1, 2, 3});
            std::error_code error;
            std::filesystem::last_write_time(stale, old + std::chrono::minutes(index), error);
        }
        const auto leftover = directory / "vulkan-x.pipelinecache.tmp-0123456789abcdef";
        writeFile(leftover, {1});
        std::error_code error;
        std::filesystem::last_write_time(leftover, old, error);
        auto device = deviceWith(loader, directory);
        if (device) {
            static_cast<void>(coverageOn(checks, *device));
            checks.expect(device->persistPipelineCache(), "the bounded write succeeds");
        }
        std::size_t caches = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".pipelinecache")
                ++caches;
        checks.expect(caches == kMaxPipelineCacheFiles && std::filesystem::exists(file) &&
                          !std::filesystem::exists(directory / "vulkan-stale-0.pipelinecache") &&
                          !std::filesystem::exists(leftover),
                      "the oldest cache files and stale temporary files are pruned");
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

} // namespace

int main(int argc, char** argv) {
    using namespace bloom::render::gpu_path_coverage_test;
    const auto options = parseOptions(argc, argv);
    if (!options.valid)
        return 2;
    Expectations checks;
    testFileFormat(checks);
    bool ran = false;
    testDeviceCycle(checks, options.loader_path, ran);
    if (!checks.ok()) {
        std::cerr << "FAIL: pipeline cache checks failed\n";
        return 1;
    }
    if (!ran) {
        std::cout << "SKIP: no compatible Vulkan device; the file format checks passed\n";
        return options.require_device ? 1 : 77;
    }
    std::cout << "PASS: pipeline cache format, persistence, rejection and bounds\n";
    return 0;
}

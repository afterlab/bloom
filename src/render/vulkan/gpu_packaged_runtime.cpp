// See gpu_packaged_runtime.hpp.
#include "gpu_packaged_runtime.hpp"

#if defined(__APPLE__)

#include <array>
#include <cstdlib>
#include <mutex>
#include <system_error>

#include <dlfcn.h>

namespace bloom::render::vulkan_detail {
namespace {

constexpr const char* kLoaderName = "libvulkan.1.dylib";
constexpr const char* kManifestName = "MoltenVK_icd.json";

// The directory of the executable or library this code is linked into.
[[nodiscard]] std::filesystem::path imageDirectory() {
    static const char anchor = 0;
    Dl_info info{};
    if (dladdr(&anchor, &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    std::error_code error;
    const auto image = std::filesystem::canonical(info.dli_fname, error);
    return error ? std::filesystem::path{} : image.parent_path();
}

[[nodiscard]] bool isFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

// The driver manifest that sits with a loader in `loaderDirectory`, or an empty path.
[[nodiscard]] std::filesystem::path manifestBeside(const std::filesystem::path& loaderDirectory) {
    const std::array<std::filesystem::path, 3> candidates{
        loaderDirectory / kManifestName,
        loaderDirectory / ".." / "Resources" / "vulkan" / "icd.d" / kManifestName,
        loaderDirectory / ".." / "share" / "vulkan" / "icd.d" / kManifestName,
    };
    for (const auto& candidate : candidates) {
        if (isFile(candidate)) {
            std::error_code error;
            const auto resolved = std::filesystem::canonical(candidate, error);
            return error ? candidate : resolved;
        }
    }
    return {};
}

} // namespace

std::filesystem::path packagedVulkanLoader() {
    const auto directory = imageDirectory();
    if (directory.empty()) {
        return {};
    }
    const std::array<std::filesystem::path, 2> candidates{
        directory / "bloom-vulkan" / kLoaderName,
        directory / ".." / "Frameworks" / kLoaderName,
    };
    for (const auto& candidate : candidates) {
        if (isFile(candidate)) {
            std::error_code error;
            const auto resolved = std::filesystem::canonical(candidate, error);
            return error ? candidate : resolved;
        }
    }
    return {};
}

void usePackagedVulkanDriver(const std::filesystem::path& loader) {
    // One writer at a time. The variable is only ever added, never changed or removed, so a
    // concurrent reader of the environment sees it either absent or complete.
    static std::mutex guard;
    const std::lock_guard lock(guard);
    if (std::getenv("VK_DRIVER_FILES") != nullptr || std::getenv("VK_ICD_FILENAMES") != nullptr) {
        return;
    }
    std::error_code error;
    const auto resolved = std::filesystem::canonical(loader, error);
    if (error) {
        return;
    }
    const auto manifest = manifestBeside(resolved.parent_path());
    if (manifest.empty()) {
        return;
    }
    static_cast<void>(::setenv("VK_DRIVER_FILES", manifest.c_str(), 0));
}

} // namespace bloom::render::vulkan_detail

#endif

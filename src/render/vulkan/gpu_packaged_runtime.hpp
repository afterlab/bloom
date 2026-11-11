#pragma once

// The Vulkan runtime Bloom ships on macOS.
//
// Linux and Windows have a system Vulkan loader and the GPU vendor's driver. macOS has neither: a
// Vulkan application brings its own loader and the MoltenVK driver, and tells the loader where the
// driver's manifest is. Bloom packages all three and finds them here, relative to the image this
// code is linked into, so the application, the command-line tools and the tests reach a device with
// nothing set in the environment.
//
// Two layouts are recognized, each from the directory `D` of that image:
//
//   beside an executable    D/bloom-vulkan/libvulkan.1.dylib
//                           D/bloom-vulkan/libMoltenVK.dylib
//                           D/bloom-vulkan/MoltenVK_icd.json
//   in an application bundle  D/../Frameworks/libvulkan.1.dylib
//                             D/../Frameworks/libMoltenVK.dylib
//                             D/../Resources/vulkan/icd.d/MoltenVK_icd.json
//
// A loader given by path (a dependency prefix in a test, for instance) is matched with the
// manifest that sits with it, including the prefix layout `lib/` + `share/vulkan/icd.d/`.

#if defined(__APPLE__)

#include <filesystem>

namespace bloom::render::vulkan_detail {

// The packaged loader, or an empty path when this image has none beside it.
[[nodiscard]] std::filesystem::path packagedVulkanLoader();

// Points the loader at the driver manifest that sits with `loader`, by setting VK_DRIVER_FILES for
// this process. The loader then loads that driver and no other, so a driver installed elsewhere on
// the machine cannot change Bloom's results. It does nothing when VK_DRIVER_FILES or
// VK_ICD_FILENAMES is already set (an explicit choice, which tests use to select or remove a
// driver) or when no manifest sits with the loader. Call it before the loader's first function.
void usePackagedVulkanDriver(const std::filesystem::path& loader);

} // namespace bloom::render::vulkan_detail

#endif

# MoltenVK v1.4.2 - the Vulkan driver for Apple's Metal (Apache-2.0; see
# dependencies/licenses/moltenvk/). macOS has no system Vulkan driver, so Bloom ships this one
# beside the locked Vulkan loader. It is built on Apple platforms only.
#
# MoltenVK's own build fetches its dependencies from the network when it configures. This recipe
# does not run that: projects/moltenvk/CMakeLists.txt supplies the same dependencies from locked
# archives and then adds MoltenVK's source tree unchanged.
#
#   SPIRV-Cross      at the revision MoltenVK pins; compiled into libMoltenVK (static), a source-only
#                    provider here like spirv-headers
#   cereal           at the revision MoltenVK pins; headers only, for the pipeline cache
#   Vulkan-Headers   MoltenVK pins the revision the lock already holds as vulkan-headers, so it
#                    builds against that component in the prefix
#   SPIRV-Tools      left out (MVK_EXCLUDE_SPIRV_TOOLS): it only adds a debug shader disassembly
#
# The wrapper refuses to configure when a locked revision differs from MoltenVK's pin.
#
# Installed: lib/libMoltenVK*.dylib and share/vulkan/icd.d/MoltenVK_icd.json, whose library path is
# relative to the manifest so the prefix can be moved. Fast-math shader compilation is off by
# default in this build (see the wrapper).
if(NOT APPLE)
    return()
endif()

set(BLOOM_MOLTENVK_VERSION v1.4.2)
set(BLOOM_MOLTENVK_COMMIT db66022459ffb663aa2b50f6b018bc2e124f5edf)
set(BLOOM_MOLTENVK_URL
    "https://codeload.github.com/KhronosGroup/MoltenVK/tar.gz/refs/tags/${BLOOM_MOLTENVK_VERSION}")
set(BLOOM_MOLTENVK_SHA256
    6864db532f1dbbdb621a8d0ec13f24edae318fd9269dd3dd0cdff791334bb1cb)

set(BLOOM_SPIRV_CROSS_COMMIT 6c09849fe88c48eaed08413aa022aaa136a3a057)
set(BLOOM_SPIRV_CROSS_URL
    "https://codeload.github.com/KhronosGroup/SPIRV-Cross/tar.gz/${BLOOM_SPIRV_CROSS_COMMIT}")
set(BLOOM_SPIRV_CROSS_SHA256
    b81b9956289950570953738e666a031ca32ff64e4fc925eba89f227c42109518)

set(BLOOM_CEREAL_COMMIT a56bad8bbb770ee266e930c95d37fff2a5be7fea)
set(BLOOM_CEREAL_URL
    "https://codeload.github.com/USCiLab/cereal/tar.gz/${BLOOM_CEREAL_COMMIT}")
set(BLOOM_CEREAL_SHA256
    12e10fb541270632f6683286ce8c7e8c321fe4addb53fa91ed498b95c1758f93)

# The vulkan-headers component's commit (vulkan-headers.cmake builds its release tag).
set(BLOOM_MOLTENVK_VULKAN_HEADERS_COMMIT e3b1eec08173d6b825cd3ac88c885a63b621504a)

# Source-only providers: acquired and extracted, never configured, built or installed.
ExternalProject_Add(bloom_dependency_spirv-cross
    URL "${BLOOM_SPIRV_CROSS_URL}"
    URL_HASH SHA256=${BLOOM_SPIRV_CROSS_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME spirv-cross-${BLOOM_SPIRV_CROSS_COMMIT}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    USES_TERMINAL_DOWNLOAD ON)

ExternalProject_Add(bloom_dependency_cereal
    URL "${BLOOM_CEREAL_URL}"
    URL_HASH SHA256=${BLOOM_CEREAL_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME cereal-${BLOOM_CEREAL_COMMIT}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    USES_TERMINAL_DOWNLOAD ON)

ExternalProject_Get_Property(bloom_dependency_spirv-cross SOURCE_DIR)
set(BLOOM_SPIRV_CROSS_SOURCE_DIR "${SOURCE_DIR}")
ExternalProject_Get_Property(bloom_dependency_cereal SOURCE_DIR)
set(BLOOM_CEREAL_SOURCE_DIR "${SOURCE_DIR}")

ExternalProject_Add(bloom_dependency_moltenvk
    URL "${BLOOM_MOLTENVK_URL}"
    URL_HASH SHA256=${BLOOM_MOLTENVK_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME moltenvk-${BLOOM_MOLTENVK_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS
        bloom_dependency_vulkan-headers
        bloom_dependency_spirv-cross
        bloom_dependency_cereal
    # The wrapper project is the source directory; MoltenVK's own tree is one of its inputs.
    CONFIGURE_COMMAND "${CMAKE_COMMAND}"
        -S "${CMAKE_CURRENT_LIST_DIR}/moltenvk"
        -B <BINARY_DIR>
        -G "${CMAKE_GENERATOR}"
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        "-DCMAKE_PREFIX_PATH:PATH=${BLOOM_DEPENDENCY_PREFIX}"
        -DCMAKE_INSTALL_LIBDIR:STRING=lib
        -DCMAKE_INSTALL_DATADIR:STRING=share
        -DBLOOM_MOLTENVK_SOURCE_DIR:PATH=<SOURCE_DIR>
        "-DBLOOM_SPIRV_CROSS_SOURCE_DIR:PATH=${BLOOM_SPIRV_CROSS_SOURCE_DIR}"
        "-DBLOOM_CEREAL_SOURCE_DIR:PATH=${BLOOM_CEREAL_SOURCE_DIR}"
        -DBLOOM_MOLTENVK_REVISION:STRING=${BLOOM_MOLTENVK_COMMIT}
        -DBLOOM_SPIRV_CROSS_REVISION:STRING=${BLOOM_SPIRV_CROSS_COMMIT}
        -DBLOOM_CEREAL_REVISION:STRING=${BLOOM_CEREAL_COMMIT}
        -DBLOOM_VULKAN_HEADERS_REVISION:STRING=${BLOOM_MOLTENVK_VULKAN_HEADERS_COMMIT}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

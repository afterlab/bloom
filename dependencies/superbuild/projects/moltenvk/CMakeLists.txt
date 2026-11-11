# Builds MoltenVK from its release archive, offline, with the sources the dependency lock pins.
#
# MoltenVK's own CMake build fetches its dependencies from the network at configure time. Each of
# its dependency recipes returns early when the target it would create already exists, so this
# project creates those targets first from the locked sources and then adds MoltenVK's source tree
# unchanged. Nothing is downloaded and no file of the MoltenVK archive is edited.
#
# Inputs (all required):
#   BLOOM_MOLTENVK_SOURCE_DIR     the extracted MoltenVK release archive
#   BLOOM_SPIRV_CROSS_SOURCE_DIR  the extracted SPIRV-Cross archive, at MoltenVK's pinned revision
#   BLOOM_CEREAL_SOURCE_DIR       the extracted cereal archive, at MoltenVK's pinned revision
#   BLOOM_MOLTENVK_REVISION       the MoltenVK commit the release archive was made from
#   BLOOM_SPIRV_CROSS_REVISION, BLOOM_CEREAL_REVISION, BLOOM_VULKAN_HEADERS_REVISION
#                                 the commits of the locked sources, checked against MoltenVK's pins
# Vulkan-Headers comes from the dependency prefix (CMAKE_PREFIX_PATH): MoltenVK pins the same
# revision the lock already holds.
cmake_minimum_required(VERSION 3.25)

project(BloomMoltenVK LANGUAGES C CXX OBJC OBJCXX)

foreach(bloom_input IN ITEMS BLOOM_MOLTENVK_SOURCE_DIR BLOOM_SPIRV_CROSS_SOURCE_DIR
                             BLOOM_CEREAL_SOURCE_DIR BLOOM_MOLTENVK_REVISION)
    if(NOT ${bloom_input})
        message(FATAL_ERROR "${bloom_input} is required.")
    endif()
endforeach()
foreach(bloom_pin IN ITEMS SPIRV-Cross cereal Vulkan-Headers)
    if(NOT EXISTS "${BLOOM_MOLTENVK_SOURCE_DIR}/ExternalRevisions/${bloom_pin}_repo_revision")
        message(FATAL_ERROR "${BLOOM_MOLTENVK_SOURCE_DIR} does not record a ${bloom_pin} revision.")
    endif()
endforeach()

# What MoltenVK sets for itself when it is the top-level project.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# A locked source must be the revision this MoltenVK release pins, so a MoltenVK update that
# moves a pin cannot be built against the previous one.
function(bloom_require_pinned_revision name locked)
    file(READ "${BLOOM_MOLTENVK_SOURCE_DIR}/ExternalRevisions/${name}_repo_revision" pinned)
    string(STRIP "${pinned}" pinned)
    if(NOT locked STREQUAL pinned)
        message(FATAL_ERROR
            "MoltenVK pins ${name} at ${pinned}, but the locked source is '${locked}'. Update "
            "the locked ${name} component together with MoltenVK.")
    endif()
endfunction()
bloom_require_pinned_revision(SPIRV-Cross "${BLOOM_SPIRV_CROSS_REVISION}")
bloom_require_pinned_revision(cereal "${BLOOM_CEREAL_REVISION}")
bloom_require_pinned_revision(Vulkan-Headers "${BLOOM_VULKAN_HEADERS_REVISION}")

# --- Vulkan::Headers -----------------------------------------------------------------------------
find_package(VulkanHeaders CONFIG REQUIRED)

# --- SPIRV-Cross, with the options of MoltenVK's cmake/recipes/SPIRV-Cross.cmake -----------------
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_NAMESPACE_OVERRIDE MVK_spirv_cross CACHE STRING "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${BLOOM_SPIRV_CROSS_SOURCE_DIR}" spirv-cross EXCLUDE_FROM_ALL SYSTEM)

# The target names below, misspelling included, are the ones MoltenVK's build files link.
add_library(SPRIV-Cross::Core ALIAS spirv-cross-core)
add_library(SPRIV-Cross::Reflect ALIAS spirv-cross-reflect)
add_library(SPRIV-Cross::GLSL ALIAS spirv-cross-glsl)
add_library(SPRIV-Cross::MSL ALIAS spirv-cross-msl)
add_library(SPRIV-Cross INTERFACE)
add_library(SPRIV-Cross::SPRIV-Cross ALIAS SPRIV-Cross)
target_link_libraries(SPRIV-Cross INTERFACE
    spirv-cross-core spirv-cross-reflect spirv-cross-glsl spirv-cross-msl)

# --- cereal: headers only ------------------------------------------------------------------------
add_library(bloom_moltenvk_cereal INTERFACE)
target_include_directories(bloom_moltenvk_cereal SYSTEM INTERFACE
    "${BLOOM_CEREAL_SOURCE_DIR}/include")
add_library(cereal::cereal ALIAS bloom_moltenvk_cereal)

# --- The revision string -------------------------------------------------------------------------
# MoltenVK's build asks git for the revision of the checkout it is built from and stops when git
# is missing. A release archive is not a checkout, so the answer comes from the lock instead: a
# stand-in that prints the locked commit, whatever directory or repository the build runs in.
set(bloom_revision_tool "${CMAKE_CURRENT_BINARY_DIR}/moltenvk-revision")
file(WRITE "${bloom_revision_tool}" "#!/bin/sh\necho ${BLOOM_MOLTENVK_REVISION}\n")
file(CHMOD "${bloom_revision_tool}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ
                WORLD_EXECUTE)
set(GIT_EXECUTABLE "${bloom_revision_tool}" CACHE FILEPATH
    "Stand-in that reports the locked MoltenVK revision" FORCE)

# --- MoltenVK ------------------------------------------------------------------------------------
# No SPIRV-Tools: it only adds a debug disassembly of shaders to the log.
set(MVK_EXCLUDE_SPIRV_TOOLS ON CACHE BOOL "" FORCE)
set(MVK_EXCLUDE_CEREAL OFF CACHE BOOL "" FORCE)
set(MVK_USE_METAL_PRIVATE_API OFF CACHE BOOL "" FORCE)
set(MVK_BUILD_SHADER_CONVERTER_TOOL OFF CACHE BOOL "" FORCE)
set(MOLTEN_VK_WITH_CCACHE OFF CACHE BOOL "" FORCE)
# Warnings and errors only: the informational log prints a banner on every instance creation.
set(MVK_CONFIG_LOG_LEVEL "warn" CACHE STRING "" FORCE)
# MoltenVK's cache-directory module resolves a per-user directory unless this variable names one,
# even though nothing is fetched; name one in the build tree so nothing outside it is touched.
set(ENV{CPM_SOURCE_CACHE} "${CMAKE_CURRENT_BINARY_DIR}/unused-fetch-cache")

# Metal's fast-math shader compilation changes results (it reorders and contracts floating-point
# arithmetic), and Bloom's GPU results are compared with a CPU reference. 0 is
# MVK_CONFIG_FAST_MATH_NEVER; it becomes this build's default, and Bloom also asks for it when it
# creates the instance.
add_compile_definitions(MVK_CONFIG_FAST_MATH_ENABLED=0)

add_subdirectory("${BLOOM_MOLTENVK_SOURCE_DIR}" moltenvk)

# --- Install -------------------------------------------------------------------------------------
# MoltenVK installs only when it is the top-level project, and its driver manifest then names the
# library by absolute path. This installs the same library with a manifest that names it relative
# to the manifest file, so the prefix can be moved and packaged.
include(GNUInstallDirs)
install(TARGETS MoltenVK LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")

set(bloom_manifest_directory "${CMAKE_INSTALL_DATADIR}/vulkan/icd.d")
file(RELATIVE_PATH bloom_manifest_to_library
    "/${bloom_manifest_directory}" "/${CMAKE_INSTALL_LIBDIR}")
function(bloom_write_moltenvk_manifest)
    # The template's one variable; every other field stays MoltenVK's.
    set(CMAKE_INSTALL_FULL_LIBDIR "${bloom_manifest_to_library}")
    configure_file("${BLOOM_MOLTENVK_SOURCE_DIR}/Templates/cmake/MoltenVK_icd.json.in"
        "${CMAKE_CURRENT_BINARY_DIR}/MoltenVK_icd.json" @ONLY)
endfunction()
bloom_write_moltenvk_manifest()
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MoltenVK_icd.json"
    DESTINATION "${bloom_manifest_directory}")

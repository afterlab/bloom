# The distribution libraries a Linux Bloom package carries (dependencies/linux-runtime).
#
# The Qt Project's Linux binaries load a handful of small X11 helper libraries that a desktop
# does not always have installed. They are not built here: each is taken from the Ubuntu 22.04
# binary package the inventory names, by checksum (linux-runtime-fetch.cmake), into the
# prefix's `linux-runtime` directory, and a package carries them beside Qt
# (cmake/package/stage_linux_runtime.cmake). Ubuntu 22.04 is the oldest system a package runs
# on, so its libraries need nothing newer than that system's C library.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()
find_program(BLOOM_LINUX_RUNTIME_PATCHELF patchelf REQUIRED)
set(bloom_linux_runtime_arguments
    "-DBLOOM_INVENTORY=${CMAKE_CURRENT_LIST_DIR}/../../linux-runtime/packages.json"
    "-DBLOOM_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../.."
    "-DBLOOM_DESTINATION=${BLOOM_DEPENDENCY_PREFIX}/linux-runtime"
    "-DBLOOM_DOWNLOAD_DIR=${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    -DBLOOM_ALLOW_DOWNLOADS=${BLOOM_DEPENDENCY_ALLOW_DOWNLOADS}
    "-DBLOOM_PATCHELF=${BLOOM_LINUX_RUNTIME_PATCHELF}")
# Strictly offline: the packages must already be in the cache with their recorded SHA-256 when
# the superbuild is configured, so the build never reaches the network.
if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${bloom_linux_runtime_arguments} -DBLOOM_CHECK_ONLY=ON
                -P "${CMAKE_CURRENT_LIST_DIR}/linux-runtime-fetch.cmake"
        RESULT_VARIABLE bloom_linux_runtime_result
        OUTPUT_VARIABLE bloom_linux_runtime_output
        ERROR_VARIABLE bloom_linux_runtime_output)
    if(NOT bloom_linux_runtime_result EQUAL 0)
        message(FATAL_ERROR
            "BLOOM_DEPENDENCY_ALLOW_DOWNLOADS is OFF and the archive cache "
            "${BLOOM_DEPENDENCY_DOWNLOAD_DIR} does not hold the Linux runtime packages:\n"
            "${bloom_linux_runtime_output}")
    endif()
endif()
# Runs on every build: the script leaves a directory that already holds the inventory's
# packages untouched, and replaces one that holds anything else.
add_custom_target(bloom_dependency_linux_runtime ALL
    COMMAND "${CMAKE_COMMAND}" ${bloom_linux_runtime_arguments}
        -P "${CMAKE_CURRENT_LIST_DIR}/linux-runtime-fetch.cmake"
    COMMENT "Checking the Linux runtime libraries in the dependency prefix"
    VERBATIM)

# The AppImage runtime (dependencies/appimage-runtime): the published static program an
# AppImage begins with, by checksum, in the prefix's `appimage-runtime` directory. Nothing links
# it; cmake/package/make_archives.cmake joins it to the package tree's image.
set(bloom_appimage_runtime_arguments
    "-DBLOOM_RECORD=${CMAKE_CURRENT_LIST_DIR}/../../appimage-runtime/runtime.json"
    "-DBLOOM_DESTINATION=${BLOOM_DEPENDENCY_PREFIX}/appimage-runtime"
    "-DBLOOM_DOWNLOAD_DIR=${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    -DBLOOM_ALLOW_DOWNLOADS=${BLOOM_DEPENDENCY_ALLOW_DOWNLOADS})
if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${bloom_appimage_runtime_arguments} -DBLOOM_CHECK_ONLY=ON
                -P "${CMAKE_CURRENT_LIST_DIR}/appimage-runtime-fetch.cmake"
        RESULT_VARIABLE bloom_appimage_runtime_result
        OUTPUT_VARIABLE bloom_appimage_runtime_output
        ERROR_VARIABLE bloom_appimage_runtime_output)
    if(NOT bloom_appimage_runtime_result EQUAL 0)
        message(FATAL_ERROR
            "BLOOM_DEPENDENCY_ALLOW_DOWNLOADS is OFF and the archive cache "
            "${BLOOM_DEPENDENCY_DOWNLOAD_DIR} does not hold the AppImage runtime:\n"
            "${bloom_appimage_runtime_output}")
    endif()
endif()
add_custom_target(bloom_dependency_appimage_runtime ALL
    COMMAND "${CMAKE_COMMAND}" ${bloom_appimage_runtime_arguments}
        -P "${CMAKE_CURRENT_LIST_DIR}/appimage-runtime-fetch.cmake"
    COMMENT "Checking the AppImage runtime in the dependency prefix"
    VERBATIM)

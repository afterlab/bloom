# One recipe file per direct dependency, per docs/architecture/dependency-intake.md. Each recipe
# declares its exact locked archive URL, SHA-256, enabled feature set, and install behavior.
include(yyjson.cmake)
include(zlib.cmake)
include(libzip.cmake)
include(imath.cmake)
include(libdeflate.cmake)
include(openexr.cmake)
include(expat.cmake)
include(yaml-cpp.cmake)
include(pystring.cmake)
include(minizip-ng.cmake)
include(sse2neon.cmake)
include(opencolorio.cmake)
include(nasm.cmake)
include(codec-common.cmake)
include(openh264.cmake)
include(kvazaar.cmake)
include(nv-codec-headers.cmake)
include(dav1d.cmake)
include(svt-av1.cmake)
include(libvpx.cmake)
include(opus.cmake)
include(ffmpeg.cmake)
include(openssl.cmake)
include(xz.cmake)
include(zstd.cmake)
include(sqlite.cmake)
include(bzip2.cmake)
include(libffi.cmake)
include(python.cmake)
include(cython.cmake)
include(numpy.cmake)
include(nanobind.cmake)
include(vulkan-headers.cmake)
include(vulkan-memory-allocator.cmake)
include(vulkan-loader.cmake)
include(moltenvk.cmake)
include(spirv-headers.cmake)
include(spirv-tools.cmake)
include(glslang.cmake)
include(qt.cmake)
include(linux-runtime.cmake)

# Prebuilt update and installer items, taken as published, by checksum (dependencies/update-runtime
# and dependencies/installer-tools; see docs/architecture/dependency-intake.md). None of them is
# built here or part of the dependency lock. Each lands in its own directory of the prefix, and
# only on the platform whose package or installer needs it: Sparkle on macOS, WinSparkle and the
# Inno Setup compiler on Windows.
function(bloom_prebuilt_item script record destination target comment)
    set(arguments
        "-DBLOOM_RECORD=${CMAKE_CURRENT_LIST_DIR}/../../${record}"
        "-DBLOOM_DESTINATION=${BLOOM_DEPENDENCY_PREFIX}/${destination}"
        "-DBLOOM_DOWNLOAD_DIR=${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        -DBLOOM_ALLOW_DOWNLOADS=${BLOOM_DEPENDENCY_ALLOW_DOWNLOADS})
    # Strictly offline: the file must already be in the cache with its recorded SHA-256 when
    # the superbuild is configured, so the build never reaches the network.
    if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" ${arguments} -DBLOOM_CHECK_ONLY=ON
                    -P "${CMAKE_CURRENT_LIST_DIR}/${script}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR
                "BLOOM_DEPENDENCY_ALLOW_DOWNLOADS is OFF and the archive cache "
                "${BLOOM_DEPENDENCY_DOWNLOAD_DIR} does not hold ${destination}:\n${output}")
        endif()
    endif()
    add_custom_target(${target} ALL
        COMMAND "${CMAKE_COMMAND}" ${arguments} -P "${CMAKE_CURRENT_LIST_DIR}/${script}"
        COMMENT "${comment}"
        VERBATIM)
endfunction()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    bloom_prebuilt_item(update-runtime-fetch.cmake update-runtime/sparkle.json
        sparkle bloom_dependency_sparkle "Checking Sparkle in the dependency prefix")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    bloom_prebuilt_item(update-runtime-fetch.cmake update-runtime/winsparkle.json
        winsparkle bloom_dependency_winsparkle "Checking WinSparkle in the dependency prefix")
    bloom_prebuilt_item(installer-tools-fetch.cmake installer-tools/innosetup.json
        innosetup bloom_dependency_innosetup "Checking Inno Setup in the dependency prefix")
endif()

# Zstandard 1.5.7 (BSD-3-Clause, the license Bloom takes of its two; see
# dependencies/licenses/zstd/) - the library behind the bundled Python's `compression.zstd` module
# on macOS. Bloom itself does not link it.
#
# Built on Apple platforms only, where Bloom ships its own interpreter (python.cmake). The static
# library and its headers only, into the prefix's private `python-support` directory; the command
# line programs, the shared library, tests and contributed code are not built.
if(NOT APPLE)
    return()
endif()

set(BLOOM_ZSTD_VERSION 1.5.7)
set(BLOOM_ZSTD_URL
    "https://github.com/facebook/zstd/releases/download/v${BLOOM_ZSTD_VERSION}/zstd-${BLOOM_ZSTD_VERSION}.tar.gz")
set(BLOOM_ZSTD_SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3)

ExternalProject_Add(bloom_dependency_zstd
    URL "${BLOOM_ZSTD_URL}"
    URL_HASH SHA256=${BLOOM_ZSTD_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME zstd-${BLOOM_ZSTD_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR build/cmake
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_INSTALL_PREFIX:PATH=${BLOOM_DEPENDENCY_PREFIX}/python-support
        -DZSTD_BUILD_PROGRAMS:BOOL=OFF
        -DZSTD_BUILD_SHARED:BOOL=OFF
        -DZSTD_BUILD_STATIC:BOOL=ON
        -DZSTD_BUILD_TESTS:BOOL=OFF
        -DZSTD_BUILD_CONTRIB:BOOL=OFF
        -DZSTD_LEGACY_SUPPORT:BOOL=OFF
    BUILD_ALWAYS OFF
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

# bzip2 1.0.8 (bzip2-1.0.6 license; see dependencies/licenses/bzip2/) - the library behind the
# carried Python's `bz2` module. Bloom itself does not link it.
#
# Built for every profile (python.cmake, issue #677), so the module does not follow the operating
# system's libbz2. The archive has a Unix Makefile and an NMAKE file but no build that is the same
# everywhere, so the adjacent bzip2/CMakeLists.txt compiles the library's seven source files into a
# static library; the bzip2 and bzip2recover programs and the scripts are not built. Installed
# into the prefix's private `python-support` directory. On Windows CPython's own project files
# compile the same seven source files (python-windows.cmake), so the archive is only unpacked there.
set(BLOOM_BZIP2_VERSION 1.0.8)
set(BLOOM_BZIP2_URL "https://sourceware.org/pub/bzip2/bzip2-${BLOOM_BZIP2_VERSION}.tar.gz")
set(BLOOM_BZIP2_SHA256 ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269)

if(WIN32)
    # The archive is unpacked only; CPython's project files compile it (python-windows.cmake).
    ExternalProject_Add(bloom_dependency_bzip2
        URL "${BLOOM_BZIP2_URL}"
        URL_HASH SHA256=${BLOOM_BZIP2_SHA256}
        DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        DOWNLOAD_NAME bzip2-${BLOOM_BZIP2_VERSION}.tar.gz
        DOWNLOAD_NO_PROGRESS ON
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
        BUILD_ALWAYS OFF
        USES_TERMINAL_DOWNLOAD ON)
    ExternalProject_Get_Property(bloom_dependency_bzip2 SOURCE_DIR)
    set(BLOOM_BZIP2_SOURCE_DIR "${SOURCE_DIR}")
    return()
endif()

ExternalProject_Add(bloom_dependency_bzip2
    URL "${BLOOM_BZIP2_URL}"
    URL_HASH SHA256=${BLOOM_BZIP2_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME bzip2-${BLOOM_BZIP2_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_LIST_DIR}/bzip2/CMakeLists.txt" "<SOURCE_DIR>/CMakeLists.txt"
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_INSTALL_PREFIX:PATH=${BLOOM_DEPENDENCY_PREFIX}/python-support
    BUILD_ALWAYS OFF
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

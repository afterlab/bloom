# XZ Utils 5.8.4 (liblzma: 0BSD; see dependencies/licenses/xz/) - the LZMA library behind the
# bundled Python's `lzma` module on macOS. Bloom itself does not link it.
#
# Built on Apple platforms only, where Bloom ships its own interpreter (python.cmake). The static
# liblzma and its headers only, into the prefix's private `python-support` directory; the command
# line tools, scripts, translations and documentation are not built.
if(NOT APPLE)
    return()
endif()

set(BLOOM_XZ_VERSION 5.8.4)
set(BLOOM_XZ_URL
    "https://github.com/tukaani-project/xz/releases/download/v${BLOOM_XZ_VERSION}/xz-${BLOOM_XZ_VERSION}.tar.gz")
set(BLOOM_XZ_SHA256 0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9)

ExternalProject_Add(bloom_dependency_xz
    URL "${BLOOM_XZ_URL}"
    URL_HASH SHA256=${BLOOM_XZ_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME xz-${BLOOM_XZ_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_INSTALL_PREFIX:PATH=${BLOOM_DEPENDENCY_PREFIX}/python-support
        -DXZ_TOOL_XZ:BOOL=OFF
        -DXZ_TOOL_XZDEC:BOOL=OFF
        -DXZ_TOOL_LZMADEC:BOOL=OFF
        -DXZ_TOOL_LZMAINFO:BOOL=OFF
        -DXZ_TOOL_SCRIPTS:BOOL=OFF
        -DXZ_DOC:BOOL=OFF
        -DXZ_NLS:BOOL=OFF
        -DXZ_DOXYGEN:BOOL=OFF
        -DBUILD_TESTING:BOOL=OFF
    BUILD_ALWAYS OFF
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

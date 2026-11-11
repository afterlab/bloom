# CPython 3.14.7 (PSF-2.0; see dependencies/licenses/python/) - the interpreter Bloom embeds for
# scripting, on macOS. 3.14 is the minor version of the system Python the Linux build embeds, so
# the scripting surface is the same on both.
#
# It is built on Apple platforms only. Linux takes Python from the system; macOS has none to take,
# and the one a package manager installs is built for the newest system and brings its own
# unlocked libraries, so an application bundle built against it neither starts on the deployment
# target nor carries what it needs. This build is for the locked deployment target and depends on
# the operating system and on locked components only:
#
#   ssl, hashlib        OpenSSL (openssl.cmake), static
#   lzma                liblzma (xz.cmake), static
#   compression.zstd    Zstandard (zstd.cmake), static
#   pyexpat             expat, the locked component (expat.cmake), static
#   decimal             the libmpdec that CPython carries (recorded with this component)
#   hashlib's own SHA-2, SHA-3, BLAKE2, MD5 and SHA-1
#                       the HACL* code that CPython carries (recorded with this component)
#   sqlite3, zlib, bz2, ctypes (libffi), curses, readline (libedit), dbm.ndbm, uuid
#                       the operating system's libraries, from the SDK
#
# Not built, because the operating system has no library for them and none is locked: tkinter and
# dbm.gnu. The mimalloc allocator CPython carries is left out; it serves the free-threaded build,
# which Bloom does not use. pkg-config is switched off, so nothing on the build machine can be
# picked up instead.
#
# Installed under the prefix's `python` directory: the shared interpreter library, whose install
# name is @rpath-relative so a bundle can carry it, the standard library, the headers and the
# `python3.14` program the tests run. pip and the standard library's own test packages are not
# installed.
if(NOT APPLE)
    return()
endif()

set(BLOOM_PYTHON_VERSION 3.14.7)
set(BLOOM_PYTHON_URL
    "https://www.python.org/ftp/python/${BLOOM_PYTHON_VERSION}/Python-${BLOOM_PYTHON_VERSION}.tar.xz")
set(BLOOM_PYTHON_SHA256 3b48dac8fb59f62eaa67ac83c1eb12bda1b7a08406dd286e252c11a66be27f81)

set(BLOOM_PYTHON_PREFIX "${BLOOM_CODEC_PREFIX}/python")

# libffi is the operating system's; its headers are in the SDK, in a directory of their own. With
# pkg-config off, CPython is told where. The SDK is reached through a plain path when its own has
# whitespace (see bloom_dependency_plain_path).
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE BLOOM_PYTHON_SDK OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE bloom_python_sdk_result)
if(NOT bloom_python_sdk_result EQUAL 0 OR NOT IS_DIRECTORY "${BLOOM_PYTHON_SDK}/usr/include/ffi")
    message(FATAL_ERROR "The macOS SDK with libffi's headers was not found (xcrun --show-sdk-path: "
        "'${BLOOM_PYTHON_SDK}')")
endif()
bloom_dependency_plain_path(BLOOM_PYTHON_PLAIN_SDK "${BLOOM_PYTHON_SDK}")

ExternalProject_Add(bloom_dependency_python
    PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_python-prefix"
    URL "${BLOOM_PYTHON_URL}"
    URL_HASH SHA256=${BLOOM_PYTHON_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME python-${BLOOM_PYTHON_VERSION}.tar.xz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS bloom_dependency_openssl bloom_dependency_xz bloom_dependency_zstd
            bloom_dependency_expat
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        PKG_CONFIG=/nonexistent/bloom-no-pkg-config
        "CFLAGS=${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG}"
        "LDFLAGS=${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG} -Wl,-rpath,@loader_path/../lib"
        "LIBLZMA_CFLAGS=-I${BLOOM_PYTHON_SUPPORT_PREFIX}/include"
        "LIBLZMA_LIBS=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib/liblzma.a"
        "LIBZSTD_CFLAGS=-I${BLOOM_PYTHON_SUPPORT_PREFIX}/include"
        "LIBZSTD_LIBS=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib/libzstd.a"
        "LIBFFI_CFLAGS=-I${BLOOM_PYTHON_PLAIN_SDK}/usr/include/ffi -DUSING_APPLE_OS_LIBFFI=1"
        "LIBFFI_LIBS=-lffi"
        "LIBEXPAT_CFLAGS=-I${BLOOM_CODEC_PREFIX}/include -DXML_STATIC"
        "LIBEXPAT_LDFLAGS=${BLOOM_CODEC_PREFIX}/lib/libexpat.a"
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        <SOURCE_DIR>/configure
        --prefix=${BLOOM_PYTHON_PREFIX}
        --enable-shared
        --enable-optimizations=no
        --without-ensurepip
        --disable-test-modules
        --with-openssl=${BLOOM_PYTHON_SUPPORT_PREFIX}
        --with-openssl-rpath=no
        --with-system-libmpdec=no
        --with-system-expat
        --without-mimalloc
        --with-readline=editline
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV} make -j3
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV} make install
        COMMAND ${CMAKE_COMMAND}
        -DPYTHON_PREFIX=${BLOOM_PYTHON_PREFIX}
        -DPYTHON_VERSION=${BLOOM_PYTHON_VERSION}
        -P "${CMAKE_CURRENT_LIST_DIR}/python-install.cmake"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

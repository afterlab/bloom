# libffi 3.8.0 (MIT; see dependencies/licenses/libffi/) - the foreign function interface behind the
# carried Python's `ctypes` module. Bloom itself does not link it.
#
# Built for every profile (python.cmake, issue #677), so `ctypes` has the same libffi everywhere
# instead of the operating system's: a static, position-independent library and its headers,
# into the prefix's private `python-support` directory, linked into the interpreter's `_ctypes`
# extension module. No documentation, no shared library.
#
# Windows: CPython's project files link libffi as a library of its own beside the extension
# modules (libffi-8.dll), so it is built that way from the same archive, by libffi's own
# configure script and its `msvcc.sh` wrapper around the Visual Studio compiler, run from the
# MSYS2 shell the FFmpeg build already needs. The library, its import library and the three
# headers go to `python-support/libffi`.
set(BLOOM_LIBFFI_PLATFORM_ENV)
if(BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG)
    set(BLOOM_LIBFFI_PLATFORM_ENV
        MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        "CFLAGS=${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG} -O2"
        "LDFLAGS=${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG}")
endif()

set(BLOOM_LIBFFI_VERSION 3.8.0)
set(BLOOM_LIBFFI_URL
    "https://github.com/libffi/libffi/releases/download/v${BLOOM_LIBFFI_VERSION}/libffi-${BLOOM_LIBFFI_VERSION}.tar.gz")
set(BLOOM_LIBFFI_SHA256 7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4)

if(WIN32)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR "libffi: no locked target for ${CMAKE_SYSTEM_PROCESSOR} on Windows")
    endif()
    find_program(BLOOM_LIBFFI_SHELL NAMES sh bash
        HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
    find_program(BLOOM_LIBFFI_MAKE_PROGRAM NAMES make
        HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
    set(BLOOM_LIBFFI_PREFIX "${BLOOM_PYTHON_SUPPORT_PREFIX}/libffi")
    ExternalProject_Add(bloom_dependency_libffi
        PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_libffi-prefix"
        URL "${BLOOM_LIBFFI_URL}"
        URL_HASH SHA256=${BLOOM_LIBFFI_SHA256}
        DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        DOWNLOAD_NAME libffi-${BLOOM_LIBFFI_VERSION}.tar.gz
        DOWNLOAD_NO_PROGRESS ON
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        # Built in the source directory, with configure named relative to it: the script looks
        # for its helper files along a colon-separated list, which a drive-letter path cuts in
        # two. The triplet names the Windows x86-64 target; the compiler is the Visual Studio
        # one, and libffi's sources are told they are being built into a library of their own.
        BUILD_IN_SOURCE ON
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
            "${BLOOM_LIBFFI_SHELL}" ./configure
            "CC=<SOURCE_DIR>/msvcc.sh -m64"
            "CXX=<SOURCE_DIR>/msvcc.sh -m64"
            LD=link
            "CPP=cl -nologo -EP"
            "CXXCPP=cl -nologo -EP"
            CPPFLAGS=-DFFI_BUILDING_DLL
            "NM=dumpbin -symbols"
            STRIP=:
            --build=x86_64-w64-mingw32
            --host=x86_64-w64-mingw32
            --disable-static
            --enable-shared
            --disable-docs
            # Without this an in-source configure moves the build into a directory named after
            # the triplet.
            --disable-builddir
        BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
            "${BLOOM_LIBFFI_MAKE_PROGRAM}" -j3
        INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory "${BLOOM_LIBFFI_PREFIX}/include"
            COMMAND ${CMAKE_COMMAND} -E copy
                <BINARY_DIR>/.libs/libffi-8.dll <BINARY_DIR>/.libs/libffi-8.lib
                "${BLOOM_LIBFFI_PREFIX}"
            COMMAND ${CMAKE_COMMAND} -E copy
                <BINARY_DIR>/include/ffi.h <BINARY_DIR>/fficonfig.h
                <SOURCE_DIR>/src/x86/ffitarget.h
                "${BLOOM_LIBFFI_PREFIX}/include"
        ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
        BUILD_ALWAYS OFF
        USES_TERMINAL_DOWNLOAD ON
        USES_TERMINAL_BUILD ON)
    return()
endif()

ExternalProject_Add(bloom_dependency_libffi
    PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_libffi-prefix"
    URL "${BLOOM_LIBFFI_URL}"
    URL_HASH SHA256=${BLOOM_LIBFFI_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME libffi-${BLOOM_LIBFFI_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        ${BLOOM_LIBFFI_PLATFORM_ENV}
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        <SOURCE_DIR>/configure
        --prefix=${BLOOM_PYTHON_SUPPORT_PREFIX}
        --libdir=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib
        --disable-shared
        --enable-static
        --with-pic
        --disable-docs
        --disable-multi-os-directory
    # MAKE is named plainly: libffi's Makefiles start their sub-makes through $(MAKE) without
    # quotes, which breaks when make's own path holds a space (Xcode on a volume with one).
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV} make -j3 MAKE=make
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
        make install MAKE=make
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

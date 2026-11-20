# Crashpad - the out-of-process crash reporter (Apache-2.0; see dependencies/licenses/crashpad/)
# that writes a minidump of a crashed Bloom program (docs/architecture/platform-support.md,
# "Crash Reports"). Built on every profile.
#
# The source is the getsentry/crashpad fork at the exact commit sentry-native 0.17.6 pins: upstream
# Crashpad builds only with GN and depot_tools, and this fork carries a CMake build of the same
# code that installs a CMake package. Two more locked archives complete its tree, both at the
# commits the fork's submodules name:
#
#   mini-chromium          Chromium's "base" routines Crashpad is written against; compiled into
#                          libmini_chromium.a (static)
#   linux-syscall-support  one header of raw system calls, Linux only
#
# zlib is the prefix's own (CRASHPAD_ZLIB_SYSTEM): the fork's zlib submodule is not used.
#
# Reviewed source patches (dependencies/patches/crashpad/, recorded in the lock with their
# digests, applied in order): 0001 builds without an HTTP transport on any platform, so the handler
# can never upload a report (Linux would otherwise need libcurl and Windows links WinHTTP for it;
# Bloom keeps every report on the machine that wrote it); 0002 makes the installed package config
# declare its targets before it gives crashpad::zlib the system zlib, which it otherwise cannot.
#
# Installed: the static libraries (client, handler, snapshot, minidump, util, tools, getopt,
# mpack, mini_chromium) with their headers below include/crashpad and the CMake package
# lib/cmake/crashpad. Bloom builds its own handler program from the handler library
# (apps/bloom-crash-handler); the fork's crashpad_handler program is built here too and is not
# shipped.
set(BLOOM_CRASHPAD_COMMIT 0bd6ca43a0958a86939a37569b61162f92f9a022)
set(BLOOM_CRASHPAD_URL
    "https://codeload.github.com/getsentry/crashpad/tar.gz/${BLOOM_CRASHPAD_COMMIT}")
set(BLOOM_CRASHPAD_SHA256 d5c4eb0a553be3f7d2bee4723851652aee6fe6067835a498915e39e73ca3a618)

set(BLOOM_MINI_CHROMIUM_COMMIT 2f5c168fa462cf3836c4a7801b4f4955496d975d)
set(BLOOM_MINI_CHROMIUM_URL
    "https://codeload.github.com/getsentry/mini_chromium/tar.gz/${BLOOM_MINI_CHROMIUM_COMMIT}")
set(BLOOM_MINI_CHROMIUM_SHA256 00f60d55f810dd66a64cf882cb8f4d241809e9e14c24baaaa96031b66ca79ac4)

set(BLOOM_LSS_COMMIT 9719c1e1e676814c456b55f5f070eabad6709d31)
set(BLOOM_LSS_URL
    "https://codeload.github.com/getsentry/chromium-linux-syscall-support/tar.gz/${BLOOM_LSS_COMMIT}")
set(BLOOM_LSS_SHA256 a858367f0de0fd22a22172c562d750c29edb43f586cb6ff94be1941b77656bf3)

set(BLOOM_CRASHPAD_PATCHES "${CMAKE_CURRENT_LIST_DIR}/../../patches/crashpad")
# On Windows, the patch program of the MSYS2 installation the FFmpeg build uses.
find_program(BLOOM_CRASHPAD_PATCH_PROGRAM NAMES patch
    HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)

# Source-only providers: acquired and extracted, never configured, built or installed on their
# own. Crashpad's build compiles them from inside its tree.
ExternalProject_Add(bloom_dependency_mini-chromium
    URL "${BLOOM_MINI_CHROMIUM_URL}"
    URL_HASH SHA256=${BLOOM_MINI_CHROMIUM_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME mini-chromium-${BLOOM_MINI_CHROMIUM_COMMIT}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    USES_TERMINAL_DOWNLOAD ON)
ExternalProject_Get_Property(bloom_dependency_mini-chromium SOURCE_DIR)
set(BLOOM_MINI_CHROMIUM_SOURCE_DIR "${SOURCE_DIR}")

set(BLOOM_CRASHPAD_DEPENDS bloom_dependency_zlib bloom_dependency_mini-chromium)
set(BLOOM_CRASHPAD_LSS_COMMAND)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ExternalProject_Add(bloom_dependency_linux-syscall-support
        URL "${BLOOM_LSS_URL}"
        URL_HASH SHA256=${BLOOM_LSS_SHA256}
        DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        DOWNLOAD_NAME linux-syscall-support-${BLOOM_LSS_COMMIT}.tar.gz
        DOWNLOAD_NO_PROGRESS ON
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        USES_TERMINAL_DOWNLOAD ON)
    ExternalProject_Get_Property(bloom_dependency_linux-syscall-support SOURCE_DIR)
    list(APPEND BLOOM_CRASHPAD_DEPENDS bloom_dependency_linux-syscall-support)
    set(BLOOM_CRASHPAD_LSS_COMMAND
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOURCE_DIR}"
            <SOURCE_DIR>/third_party/lss/lss)
endif()

ExternalProject_Add(bloom_dependency_crashpad
    URL "${BLOOM_CRASHPAD_URL}"
    URL_HASH SHA256=${BLOOM_CRASHPAD_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME crashpad-${BLOOM_CRASHPAD_COMMIT}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS ${BLOOM_CRASHPAD_DEPENDS}
    # The submodule trees the archive leaves empty, then the reviewed patches.
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy_directory "${BLOOM_MINI_CHROMIUM_SOURCE_DIR}"
            <SOURCE_DIR>/third_party/mini_chromium/mini_chromium
        ${BLOOM_CRASHPAD_LSS_COMMAND}
        COMMAND "${BLOOM_CRASHPAD_PATCH_PROGRAM}" -p1 --forward --batch
            "--input=${BLOOM_CRASHPAD_PATCHES}/0001-build-without-an-http-transport.patch"
        COMMAND "${BLOOM_CRASHPAD_PATCH_PROGRAM}" -p1 --forward --batch
            "--input=${BLOOM_CRASHPAD_PATCHES}/0002-declare-the-targets-before-the-system-zlib.patch"
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        # zlib's static-only package config names no ZLIB::ZLIB target; FindZLIB under ZLIB_ROOT
        # finds the static archive (as in libzip.cmake).
        -DCMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=OFF
        ${BLOOM_ZLIB_CONSUMER_ARGUMENTS}
        "-DCMAKE_PREFIX_PATH:PATH=${BLOOM_DEPENDENCY_PREFIX}"
        "-DZLIB_ROOT:PATH=${BLOOM_DEPENDENCY_PREFIX}"
        -DCMAKE_INSTALL_LIBDIR:STRING=lib
        -DCMAKE_INSTALL_INCLUDEDIR:STRING=include
        -DCMAKE_INSTALL_BINDIR:STRING=bin
        -DCRASHPAD_ZLIB_SYSTEM:BOOL=ON
        -DCRASHPAD_ENABLE_INSTALL:BOOL=ON
        -DCRASHPAD_ENABLE_INSTALL_DEV:BOOL=ON
        -DCRASHPAD_ENABLE_STACKTRACE:BOOL=OFF
        -DCRASHPAD_BUILD_TOOLS:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

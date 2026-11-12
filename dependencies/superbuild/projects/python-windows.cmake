# CPython on Windows (included by python.cmake, which holds the version and the archive).
#
# CPython has no configure script for the Visual Studio compiler; its Windows build is the set of
# project files in the archive's PCbuild directory, driven by MSBuild. Those files fetch prebuilt
# libraries of CPython's own choosing. Bloom gives them the locked components instead, so the
# Windows interpreter is made of the same sources as the Linux and macOS ones:
#
#   ssl, hashlib        OpenSSL (openssl.cmake), as libcrypto-3-x64.dll and libssl-3-x64.dll
#   ctypes              libffi (libffi.cmake), as libffi-8.dll
#   sqlite3             SQLite (sqlite.cmake), compiled by CPython's project from the amalgamation
#   bz2                 bzip2 (bzip2.cmake), compiled by CPython's project
#   compression.zstd    Zstandard (zstd.cmake), compiled by CPython's project
#   lzma                liblzma (xz.cmake), the static library            } through the patch in
#   zlib                zlib, the locked component (zlib.cmake), static   } dependencies/patches/
#   pyexpat             expat, the locked component (expat.cmake), static } python/
#   decimal, hashlib's own algorithms
#                       the libmpdec and HACL* code that CPython carries, as elsewhere
#
# The patch adds three switches to four project files; without them CPython's own zlib-ng,
# the expat it carries (an older release than the locked one) and a liblzma archive layout that
# XZ Utils no longer has would be used. It also adds one header of one line, through which the
# decimal project reaches the libmpdec the archive carries (the project expects the header of a
# separate mpdecimal release). Nothing else of CPython is changed.
#
# Not built, as on the other platforms: tkinter (and so no Tcl/Tk), the test modules. curses,
# readline and the dbm back ends do not exist on Windows. The JIT and the free-threaded build are
# off (CPython's defaults).
#
# MSBuild runs a few of CPython's source generators with an existing Python (PythonForBuild); the
# build machine's own is used for that, as it is for Meson. It contributes no file to the result.
#
# Installed with CPython's own layout tool (PC/layout), run by the interpreter just built, into
# the prefix's `python` directory: python.exe, python314.dll, the extension modules and the three
# libraries above in `DLLs`, the standard library in `Lib`, the headers and the import library.
# python-install.cmake then checks the modules and what they load.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "CPython: no locked target for ${CMAKE_SYSTEM_PROCESSOR} on Windows")
endif()

set(BLOOM_PYTHON_PROGRAM "${BLOOM_PYTHON_PREFIX}/python.exe")
# The Visual Studio compiler never fuses a multiplication and an addition on x86-64 under its
# default floating-point model, so no flag is needed for what -ffp-contract=off does elsewhere.
set(BLOOM_PYTHON_FLOAT_FLAG "")

find_program(BLOOM_PYTHON_MSBUILD NAMES msbuild REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The patch program of the MSYS2 installation the FFmpeg build uses. The project files have
# Windows line endings, and so has the patch; --binary keeps both as they are.
find_program(BLOOM_PYTHON_PATCH_PROGRAM NAMES patch
    HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
set(BLOOM_PYTHON_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/../../patches/python/0001-windows-build-with-locked-libraries.patch")

# CPython's sqlite project reads the SQLite version from the name of its source directory, which
# must end in the four-part version; the amalgamation is copied to a directory named that way.
set(BLOOM_PYTHON_EXTERNALS "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_python-prefix/externals")
set(BLOOM_PYTHON_SQLITE_DIR "${BLOOM_PYTHON_EXTERNALS}/sqlite-${BLOOM_SQLITE_VERSION}.0")

# CPython's project files choose the compiler toolset from the Visual Studio release and know
# none for a release newer than the archive, so the toolset of the compiler this superbuild runs
# with is named: v145 for Visual Studio 2026 (compiler 19.50), v143 for Visual Studio 2022.
if(MSVC_VERSION GREATER_EQUAL 1950)
    set(BLOOM_PYTHON_TOOLSET v145)
else()
    set(BLOOM_PYTHON_TOOLSET v143)
endif()

set(BLOOM_PYTHON_OUTPUT <SOURCE_DIR>/PCbuild/amd64)
ExternalProject_Add(bloom_dependency_python
    PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_python-prefix"
    URL "${BLOOM_PYTHON_URL}"
    URL_HASH SHA256=${BLOOM_PYTHON_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME python-${BLOOM_PYTHON_VERSION}.tar.xz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS bloom_dependency_openssl bloom_dependency_xz bloom_dependency_zstd
        bloom_dependency_expat bloom_dependency_zlib bloom_dependency_bzip2
        bloom_dependency_sqlite bloom_dependency_libffi
    PATCH_COMMAND "${BLOOM_PYTHON_PATCH_PROGRAM}" --binary -p1 -i "${BLOOM_PYTHON_PATCH}"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E make_directory "${BLOOM_PYTHON_SQLITE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${BLOOM_SQLITE_SOURCE_DIR}/sqlite3.c" "${BLOOM_SQLITE_SOURCE_DIR}/sqlite3.h"
            "${BLOOM_SQLITE_SOURCE_DIR}/sqlite3ext.h" "${BLOOM_PYTHON_SQLITE_DIR}"
    BUILD_IN_SOURCE ON
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
        "${BLOOM_PYTHON_MSBUILD}" <SOURCE_DIR>/PCbuild/pcbuild.proj /t:Build /m /nologo
        /verbosity:minimal
        /p:Configuration=Release
        /p:Platform=x64
        /p:PlatformToolset=${BLOOM_PYTHON_TOOLSET}
        /p:IncludeExternals=true
        /p:IncludeSSL=true
        /p:IncludeCTypes=true
        /p:IncludeTkinter=false
        /p:IncludeTests=false
        "/p:PythonForBuild=${Python3_EXECUTABLE}"
        "/p:sqlite3Dir=${BLOOM_PYTHON_SQLITE_DIR}"
        "/p:bz2Dir=${BLOOM_BZIP2_SOURCE_DIR}/"
        "/p:zstdDir=${BLOOM_ZSTD_SOURCE_DIR}/"
        # The libmpdec that the archive itself carries.
        /p:mpdecimalDir=<SOURCE_DIR>/Modules/_decimal/
        "/p:opensslOutDir=${BLOOM_OPENSSL_PREFIX}/bin/"
        "/p:opensslIncludeDir=${BLOOM_OPENSSL_PREFIX}/include"
        # The name OpenSSL's own Windows build gives its libraries.
        /p:_DLLSuffix=-3-x64
        "/p:libffiOutDir=${BLOOM_LIBFFI_PREFIX}/"
        "/p:libffiIncludeDir=${BLOOM_LIBFFI_PREFIX}/include"
        # The switches of the patch: the locked zlib and expat of the prefix, the locked liblzma.
        "/p:ExternalZlibDir=${BLOOM_CODEC_PREFIX}/"
        "/p:ExternalExpatDir=${BLOOM_CODEC_PREFIX}/"
        "/p:ExternalLzmaDir=${BLOOM_PYTHON_SUPPORT_PREFIX}/"
    INSTALL_COMMAND ${CMAKE_COMMAND} -E rm -rf "${BLOOM_PYTHON_PREFIX}"
        COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
            ${BLOOM_PYTHON_OUTPUT}/python.exe -E -s <SOURCE_DIR>/PC/layout
            --source <SOURCE_DIR>
            --build ${BLOOM_PYTHON_OUTPUT}
            --arch amd64
            --copy "${BLOOM_PYTHON_PREFIX}"
            --include-dev
            --include-venv
        COMMAND ${CMAKE_COMMAND}
            -DPYTHON_PREFIX=${BLOOM_PYTHON_PREFIX}
            -DPYTHON_VERSION=${BLOOM_PYTHON_VERSION}
            -P "${CMAKE_CURRENT_LIST_DIR}/python-install.cmake"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

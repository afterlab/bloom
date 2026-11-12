# CPython 3.14.7 (PSF-2.0; see dependencies/licenses/python/) - the interpreter Bloom embeds for
# scripting and carries in every package (issue #677), so scripting starts on a computer that has
# no Python and behaves the same on Linux, macOS and Windows.
#
# The interpreter a system or a package manager provides is built for that system and brings its
# own unlocked libraries. This build depends on the operating system and on locked components
# only:
#
#   ssl, hashlib        OpenSSL (openssl.cmake), static
#   lzma                liblzma (xz.cmake), static
#   compression.zstd    Zstandard (zstd.cmake), static
#   zlib                zlib, the locked component (zlib.cmake), static
#   bz2                 bzip2 (bzip2.cmake), static
#   sqlite3             SQLite (sqlite.cmake), static
#   pyexpat             expat, the locked component (expat.cmake), static
#   ctypes              libffi (libffi.cmake), static
#   decimal             the libmpdec that CPython carries (recorded with this component)
#   hashlib's own SHA-2, SHA-3, BLAKE2, MD5 and SHA-1
#                       the HACL* code that CPython carries (recorded with this component)
#
# Not built on any platform, so the module set is the same everywhere: tkinter, dbm.gnu,
# dbm.ndbm, curses and readline (an embedded interpreter has no terminal). The private `_uuid`
# accelerator is built where the operating system itself has the function (macOS, Windows) and
# not on Linux, where it would need the system's libuuid; the `uuid` module works without it.
# The mimalloc allocator CPython carries is left out; it serves the free-threaded build, which
# Bloom does not use. pkg-config is switched off and every library is named explicitly, so
# nothing on the build machine can be picked up instead.
#
# Installed under the prefix's `python` directory: the shared interpreter library, which a
# package can carry (macOS: an @rpath-relative install name; Linux: found through the program's
# and Bloom's own $ORIGIN-relative search path), the standard library, the headers and the
# `python3.14` program the tests run. pip and the standard library's own test packages are not
# installed. python-install.cmake then checks the modules and what they load.
#
# Windows builds the same archive with CPython's own project files (python-windows.cmake).
set(BLOOM_PYTHON_VERSION 3.14.7)
set(BLOOM_PYTHON_URL
    "https://www.python.org/ftp/python/${BLOOM_PYTHON_VERSION}/Python-${BLOOM_PYTHON_VERSION}.tar.xz")
set(BLOOM_PYTHON_SHA256 3b48dac8fb59f62eaa67ac83c1eb12bda1b7a08406dd286e252c11a66be27f81)

set(BLOOM_PYTHON_PREFIX "${BLOOM_CODEC_PREFIX}/python")
if(WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/python-windows.cmake")
    return()
endif()
# The interpreter the tests and the package builds (numpy.cmake) run.
set(BLOOM_PYTHON_PROGRAM "${BLOOM_PYTHON_PREFIX}/bin/python3.14")

# A multiplication and an addition are never fused into one operation. Arm64 compilers fuse them
# by default and x86-64 ones cannot (the baseline has no such instruction), which would make the
# same script print different last bits on the two (complex multiplication, numpy's dot
# products). With fusing off the interpreter and numpy compute the same on every platform.
set(BLOOM_PYTHON_FLOAT_FLAG -ffp-contract=off)

set(BLOOM_PYTHON_DEPENDS bloom_dependency_openssl bloom_dependency_xz bloom_dependency_zstd
    bloom_dependency_expat bloom_dependency_zlib bloom_dependency_bzip2 bloom_dependency_sqlite
    bloom_dependency_libffi)
# The modules no platform builds (see above). A module configure is told is "n/a" is neither
# built nor reported as missing.
set(BLOOM_PYTHON_MODULE_ARGUMENTS
    py_cv_module__tkinter=n/a
    py_cv_module__gdbm=n/a
    py_cv_module__dbm=n/a
    py_cv_module__curses=n/a
    py_cv_module__curses_panel=n/a
    py_cv_module_readline=n/a)
if(APPLE)
    set(BLOOM_PYTHON_PLATFORM_ENV
        MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        "CFLAGS=${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG} ${BLOOM_PYTHON_FLOAT_FLAG}"
        "LDFLAGS=${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG} -Wl,-rpath,@loader_path/../lib")
else()
    # The private `_uuid` accelerator would need the system's libuuid on Linux.
    list(APPEND BLOOM_PYTHON_MODULE_ARGUMENTS py_cv_module__uuid=n/a)
    # The program finds the interpreter library beside it. The value is written for the Makefile
    # CPython's configure generates: `\$$ORIGIN` there reaches the linker as `$ORIGIN`.
    set(BLOOM_PYTHON_PLATFORM_ENV
        "CFLAGS=${BLOOM_PYTHON_FLOAT_FLAG}"
        "LDFLAGS=-Wl,-rpath,\\$$ORIGIN/../lib")
endif()

ExternalProject_Add(bloom_dependency_python
    PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_python-prefix"
    URL "${BLOOM_PYTHON_URL}"
    URL_HASH SHA256=${BLOOM_PYTHON_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME python-${BLOOM_PYTHON_VERSION}.tar.xz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS ${BLOOM_PYTHON_DEPENDS}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        PKG_CONFIG=/nonexistent/bloom-no-pkg-config
        ${BLOOM_PYTHON_PLATFORM_ENV}
        "LIBLZMA_CFLAGS=-I${BLOOM_PYTHON_SUPPORT_PREFIX}/include"
        "LIBLZMA_LIBS=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib/liblzma.a"
        "LIBZSTD_CFLAGS=-I${BLOOM_PYTHON_SUPPORT_PREFIX}/include"
        "LIBZSTD_LIBS=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib/libzstd.a"
        "ZLIB_CFLAGS=-I${BLOOM_CODEC_PREFIX}/include"
        "ZLIB_LIBS=${BLOOM_CODEC_PREFIX}/lib/libz.a"
        "BZIP2_CFLAGS=-I${BLOOM_PYTHON_SUPPORT_PREFIX}/include"
        "BZIP2_LIBS=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib/libbz2.a"
        "LIBSQLITE3_CFLAGS=-I${BLOOM_PYTHON_SUPPORT_PREFIX}/include"
        "LIBSQLITE3_LIBS=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib/libsqlite3.a"
        "LIBFFI_CFLAGS=-I${BLOOM_PYTHON_SUPPORT_PREFIX}/include"
        "LIBFFI_LIBS=${BLOOM_PYTHON_SUPPORT_PREFIX}/lib/libffi.a"
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
        --without-readline
        ${BLOOM_PYTHON_MODULE_ARGUMENTS}
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

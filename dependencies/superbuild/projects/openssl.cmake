# OpenSSL 3.5.9 (Apache-2.0; see dependencies/licenses/openssl/) - the TLS and hash library behind
# the carried Python's `ssl` and `hashlib` modules. Bloom itself does not link it.
#
# Bloom carries its own interpreter on every platform (python.cmake, issue #677), so this is built
# for every profile from the same archive.
#
# Linux and macOS: static libraries and headers only, into the prefix's private `python-support`
# directory, which no other recipe and no Bloom target searches: the libraries are linked into the
# interpreter's extension modules and nothing of OpenSSL is installed as a file of its own. The
# configuration directory is a path that does not exist, so no configuration file on the machine
# is read; the interpreter is pointed at the system's certificates when it starts
# (src/scripting/python/runtime.cpp).
#
# Windows: CPython's project files link OpenSSL as two libraries of its own beside the extension
# modules (libcrypto-3-x64.dll, libssl-3-x64.dll), so they are built that way, with the Visual
# Studio compiler and the locked NASM, into `python-support/openssl`. The configuration directory
# is OpenSSL's own default for Windows, under the Common Files directory that only an
# administrator can write; a directory any user could create would let that user plant a
# configuration file. The build needs a Perl that runs as a Windows program (MSYS2's
# `mingw-w64-ucrt-x86_64-perl`, or Strawberry Perl); the MSYS2 shell's own Perl is refused by
# OpenSSL's Configure because it does not produce Windows paths.
#
# No programs, tests, documentation, loadable modules, engines or legacy algorithms are built on
# any platform.
set(BLOOM_OPENSSL_VERSION 3.5.9)
set(BLOOM_OPENSSL_URL
    "https://github.com/openssl/openssl/releases/download/openssl-${BLOOM_OPENSSL_VERSION}/openssl-${BLOOM_OPENSSL_VERSION}.tar.gz")
set(BLOOM_OPENSSL_SHA256 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)

# The interpreter's private build inputs. A plain path: OpenSSL's and CPython's build scripts split
# their arguments on whitespace (see bloom_dependency_plain_path).
set(BLOOM_PYTHON_SUPPORT_PREFIX "${BLOOM_CODEC_PREFIX}/python-support")

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(BLOOM_OPENSSL_TARGET darwin64-arm64-cc)
    else()
        set(BLOOM_OPENSSL_TARGET darwin64-x86_64-cc)
    endif()
    set(BLOOM_OPENSSL_PLATFORM_ARGUMENTS ${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG})
elseif(WIN32)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR "OpenSSL: no locked target for ${CMAKE_SYSTEM_PROCESSOR} on Windows")
    endif()
    find_program(BLOOM_OPENSSL_PERL NAMES perl
        HINTS "$ENV{MSYS2_ROOT}/ucrt64/bin" "$ENV{MSYS2_ROOT}/mingw64/bin"
              "C:/msys64/ucrt64/bin" "C:/Strawberry/perl/bin" REQUIRED)
    find_program(BLOOM_OPENSSL_NMAKE nmake REQUIRED)
    set(BLOOM_OPENSSL_PREFIX "${BLOOM_PYTHON_SUPPORT_PREFIX}/openssl")
    # OpenSSL's Configure and its makefile run the assembler by its bare name, so the locked
    # NASM's directory is put first on the search path of those two steps.
    cmake_path(GET BLOOM_NASM_EXECUTABLE PARENT_PATH bloom_openssl_nasm_directory)
    set(BLOOM_OPENSSL_ENV ${BLOOM_DEPENDENCY_COMMON_ENV}
        --modify "PATH=path_list_prepend:${bloom_openssl_nasm_directory}")
    ExternalProject_Add(bloom_dependency_openssl
        PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_openssl-prefix"
        URL "${BLOOM_OPENSSL_URL}"
        URL_HASH SHA256=${BLOOM_OPENSSL_SHA256}
        DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        DOWNLOAD_NAME openssl-${BLOOM_OPENSSL_VERSION}.tar.gz
        DOWNLOAD_NO_PROGRESS ON
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        DEPENDS bloom_dependency_nasm
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_OPENSSL_ENV}
            "${BLOOM_OPENSSL_PERL}" <SOURCE_DIR>/Configure VC-WIN64A
            --prefix=${BLOOM_OPENSSL_PREFIX}
            no-tests
            no-apps
            no-docs
            no-module
            no-legacy
            no-engine
        BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_OPENSSL_ENV} CL=/MP
            "${BLOOM_OPENSSL_NMAKE}" /nologo build_libs
        # install_dev installs the headers, the import libraries and the two libraries with their
        # symbol files. CPython's project files read the import libraries from the directory of
        # the libraries, so they are copied beside them.
        INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
            "${BLOOM_OPENSSL_NMAKE}" /nologo install_dev
            COMMAND ${CMAKE_COMMAND} -E copy
                "${BLOOM_OPENSSL_PREFIX}/lib/libcrypto.lib" "${BLOOM_OPENSSL_PREFIX}/lib/libssl.lib"
                "${BLOOM_OPENSSL_PREFIX}/bin"
        ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
        BUILD_ALWAYS OFF
        USES_TERMINAL_DOWNLOAD ON
        USES_TERMINAL_BUILD ON)
    return()
else()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        message(FATAL_ERROR "OpenSSL: no locked target for ${CMAKE_SYSTEM_PROCESSOR} on Linux")
    endif()
    set(BLOOM_OPENSSL_TARGET linux-x86_64)
    # The archives are linked into shared extension modules.
    set(BLOOM_OPENSSL_PLATFORM_ARGUMENTS -fPIC)
endif()

ExternalProject_Add(bloom_dependency_openssl
    PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_openssl-prefix"
    URL "${BLOOM_OPENSSL_URL}"
    URL_HASH SHA256=${BLOOM_OPENSSL_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME openssl-${BLOOM_OPENSSL_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        perl <SOURCE_DIR>/Configure ${BLOOM_OPENSSL_TARGET}
        --prefix=${BLOOM_PYTHON_SUPPORT_PREFIX}
        --libdir=lib
        --openssldir=/nonexistent/bloom-openssl
        no-shared
        no-tests
        no-apps
        no-docs
        no-module
        no-legacy
        no-engine
        ${BLOOM_OPENSSL_PLATFORM_ARGUMENTS}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV} make -j3 build_libs
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV} make install_dev
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

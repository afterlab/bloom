# OpenSSL 3.5.9 (Apache-2.0; see dependencies/licenses/openssl/) - the TLS and hash library behind
# the bundled Python's `ssl` and `hashlib` modules on macOS. Bloom itself does not link it.
#
# It is built on Apple platforms only, where Bloom ships its own interpreter (python.cmake). Linux
# takes Python, and with it OpenSSL, from the system.
#
# Static libraries and headers only, into the prefix's private `python-support` directory, which no
# other recipe and no Bloom target searches: the libraries are linked into the interpreter's
# extension modules and nothing of OpenSSL is installed as a file of its own. No programs, tests or
# documentation are built. The configuration directory is a path that does not exist, so no
# configuration file on the machine is read; the interpreter is pointed at the system's
# certificate bundle when it starts (src/scripting/python/runtime.cpp).
if(NOT APPLE)
    return()
endif()

set(BLOOM_OPENSSL_VERSION 3.5.9)
set(BLOOM_OPENSSL_URL
    "https://github.com/openssl/openssl/releases/download/openssl-${BLOOM_OPENSSL_VERSION}/openssl-${BLOOM_OPENSSL_VERSION}.tar.gz")
set(BLOOM_OPENSSL_SHA256 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)

# The interpreter's private build inputs. A plain path: OpenSSL's and CPython's build scripts split
# their arguments on whitespace (see bloom_dependency_plain_path).
set(BLOOM_PYTHON_SUPPORT_PREFIX "${BLOOM_CODEC_PREFIX}/python-support")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(BLOOM_OPENSSL_TARGET darwin64-arm64-cc)
else()
    set(BLOOM_OPENSSL_TARGET darwin64-x86_64-cc)
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
        ${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV} make -j3 build_libs
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV} make install_dev
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

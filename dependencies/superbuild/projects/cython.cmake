# Cython 3.3.1 (Apache-2.0; see dependencies/licenses/cython/) - the compiler that turns numpy's
# `.pyx` sources into C. A build-host tool only: nothing of it is installed into the prefix or
# carried by a package (the generated C carries Cython's utility code, which its license covers
# and numpy's own wheels carry the same way).
#
# The source archive is unpacked into the superbuild's private tree and run as it is, as pure
# Python under the carried interpreter (numpy.cmake); its optional compiled accelerators are not
# built, and nothing is fetched or installed with pip.
set(BLOOM_CYTHON_VERSION 3.3.1)
set(BLOOM_CYTHON_URL
    "https://files.pythonhosted.org/packages/source/c/cython/cython-${BLOOM_CYTHON_VERSION}.tar.gz")
set(BLOOM_CYTHON_SHA256 e0e45989e821bef6aa9ae15c7d583ab5e055f7406c4b3624e49e06d24e90375a)

ExternalProject_Add(bloom_dependency_cython
    PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_cython-prefix"
    URL "${BLOOM_CYTHON_URL}"
    URL_HASH SHA256=${BLOOM_CYTHON_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME cython-${BLOOM_CYTHON_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    USES_TERMINAL_DOWNLOAD ON)
ExternalProject_Get_Property(bloom_dependency_cython SOURCE_DIR)
set(BLOOM_CYTHON_SCRIPT "${SOURCE_DIR}/cython.py")

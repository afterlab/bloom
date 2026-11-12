# numpy 2.5.4 (BSD-3-Clause and the licenses of the code it carries; see
# dependencies/licenses/numpy/) - the one third-party package Bloom's carried Python has (issue
# #677). Scripts import it the same way on Linux, macOS and Windows.
#
# Built from the source archive with the Meson that the archive itself carries, run by the carried
# interpreter (python.cmake), and with the locked Cython (cython.cmake). Nothing is fetched and pip
# is not used. The adjacent numpy.configure-arguments holds the options, the same on every
# platform:
#
#   no BLAS or LAPACK library   linear algebra uses numpy's own built-in routines, so its results
#                               do not follow a system or a vendor library (macOS would otherwise
#                               take Accelerate). They are slower than an accelerated build.
#   SVML off                    Intel's vector math library is x86-64 Linux only; with it off the
#                               same code computes the transcendental functions everywhere.
#   CPU-specific code off       numpy's hand-written kernels for particular instruction sets,
#                               and its choice among them when it starts, are not built. They
#                               fuse a multiplication and an addition where the processor can
#                               (always on arm64, on x86-64 only on processors that have the
#                               instruction), so `einsum`, complex multiplication and complex
#                               absolute value gave different last bits on different computers.
#                               The portable C loops compute the same everywhere. They are
#                               slower than the kernels on large arrays.
#
# Installed into the carried interpreter's own `site-packages`, without numpy's test packages.
# numpy-install.cmake then imports it under the carried interpreter and checks its build record.
set(BLOOM_NUMPY_VERSION 2.5.4)
set(BLOOM_NUMPY_URL
    "https://files.pythonhosted.org/packages/source/n/numpy/numpy-${BLOOM_NUMPY_VERSION}.tar.gz")
set(BLOOM_NUMPY_SHA256 9a94cf751c9ad8ebaa835bcd3d40dacf8534ad086b88c38029b65123c7999d2a)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/numpy.configure-arguments" BLOOM_NUMPY_ARGUMENTS)

# Meson finds the interpreter it installs for and the Cython it compiles with through this file,
# never through the build machine's PATH.
set(BLOOM_NUMPY_NATIVE_FILE "${CMAKE_BINARY_DIR}/bloom-numpy-meson-native.ini")
file(WRITE "${BLOOM_NUMPY_NATIVE_FILE}"
    "[binaries]\n"
    "python = '${BLOOM_PYTHON_PROGRAM}'\n"
    "cython = ['${BLOOM_PYTHON_PROGRAM}', '${BLOOM_CYTHON_SCRIPT}']\n"
    "ninja = '${BLOOM_NINJA_EXECUTABLE}'\n"
    "pkg-config = '/nonexistent/bloom-no-pkg-config'\n")

# The compiler flags: the deployment target on macOS, and no fused multiply-add anywhere
# (BLOOM_PYTHON_FLOAT_FLAG in python.cmake; empty for the Visual Studio compiler, which does not
# fuse), so numpy computes the same on arm64 and x86-64.
set(BLOOM_NUMPY_FLAGS "${BLOOM_PYTHON_FLOAT_FLAG}")
set(BLOOM_NUMPY_LINK_FLAGS "")
if(BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG)
    set(BLOOM_NUMPY_FLAGS "${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG} ${BLOOM_PYTHON_FLOAT_FLAG}")
    set(BLOOM_NUMPY_LINK_FLAGS "${BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG}")
endif()
set(BLOOM_NUMPY_PLATFORM_ARGUMENTS
    "-Dc_args=${BLOOM_NUMPY_FLAGS}"
    "-Dcpp_args=${BLOOM_NUMPY_FLAGS}"
    "-Dc_link_args=${BLOOM_NUMPY_LINK_FLAGS}"
    "-Dcpp_link_args=${BLOOM_NUMPY_LINK_FLAGS}")

ExternalProject_Add(bloom_dependency_numpy
    PREFIX "${BLOOM_CODEC_BUILD_ROOT}/bloom_dependency_numpy-prefix"
    URL "${BLOOM_NUMPY_URL}"
    URL_HASH SHA256=${BLOOM_NUMPY_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME numpy-${BLOOM_NUMPY_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS bloom_dependency_python bloom_dependency_cython
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        "${BLOOM_PYTHON_PROGRAM}" -I <SOURCE_DIR>/vendored-meson/meson/meson.py setup
        <BINARY_DIR> <SOURCE_DIR>
        --prefix=${BLOOM_PYTHON_PREFIX}
        --native-file=${BLOOM_NUMPY_NATIVE_FILE}
        ${BLOOM_NUMPY_ARGUMENTS}
        ${BLOOM_NUMPY_PLATFORM_ARGUMENTS}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
        "${BLOOM_PYTHON_PROGRAM}" -I <SOURCE_DIR>/vendored-meson/meson/meson.py compile
        -C <BINARY_DIR>
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${BLOOM_DEPENDENCY_COMMON_ENV}
        "${BLOOM_PYTHON_PROGRAM}" -I <SOURCE_DIR>/vendored-meson/meson/meson.py install
        -C <BINARY_DIR> --no-rebuild --tags=runtime,python-runtime
        COMMAND ${CMAKE_COMMAND}
        -DPYTHON_PROGRAM=${BLOOM_PYTHON_PROGRAM}
        -DNUMPY_VERSION=${BLOOM_NUMPY_VERSION}
        -P "${CMAKE_CURRENT_LIST_DIR}/numpy-install.cmake"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

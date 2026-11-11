# sse2neon - one header that implements the SSE intrinsics with Arm NEON (MIT; see
# dependencies/licenses/sse2neon/). OpenColorIO's CPU renderers are written with SSE intrinsics;
# on Arm it compiles the same renderers through this header, and without it falls back to scalar
# code that calls the C library's pow, log and exp. Bloom's CPU color results are the reference its
# GPU programs reproduce and the pixels every platform has to agree on, so the Arm build has to run
# the same arithmetic as the x86-64 one: this header is required there, not optional.
#
# OpenColorIO's own build clones the header from the network while it configures. This recipe
# supplies the revision OpenColorIO 2.5.2 pins (share/cmake/modules/install/Installsse2neon.cmake)
# from a locked archive instead; opencolorio.cmake hands it over and refuses a build that did not
# use it. It is built on Apple platforms only, the one Arm target Bloom has.
if(NOT APPLE)
    return()
endif()

set(BLOOM_SSE2NEON_COMMIT 227cc413fb2d50b2a10073087be96b59d5364aea)
set(BLOOM_SSE2NEON_URL
    "https://codeload.github.com/DLTcollab/sse2neon/tar.gz/${BLOOM_SSE2NEON_COMMIT}")
set(BLOOM_SSE2NEON_SHA256
    3427a495743bb6fd1b5f9f806b80f57d67b1ac7ccf39a5f44aedd487fd7e6da1)

# A source-only provider: acquired and extracted, never configured, built or installed.
ExternalProject_Add(bloom_dependency_sse2neon
    URL "${BLOOM_SSE2NEON_URL}"
    URL_HASH SHA256=${BLOOM_SSE2NEON_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME sse2neon-${BLOOM_SSE2NEON_COMMIT}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    USES_TERMINAL_DOWNLOAD ON)

ExternalProject_Get_Property(bloom_dependency_sse2neon SOURCE_DIR)
# OpenColorIO's configure check passes this directory to the compiler without quoting it, so a
# path with whitespace makes the check fail and the build silently drops the SSE renderers. Reach
# it through a link without whitespace, as the codec recipes do.
bloom_dependency_plain_path(BLOOM_SSE2NEON_SOURCE_DIR "${SOURCE_DIR}")

# Run as a script after OpenColorIO configures on Arm: stops the build unless OpenColorIO took the
# locked sse2neon header and kept its SSE renderers.
#
#   cmake -DBLOOM_OPENCOLORIO_CACHE=<build>/CMakeCache.txt -DBLOOM_SSE2NEON_SOURCE_DIR=<dir>
#         -P opencolorio-require-sse2neon.cmake
#
# OpenColorIO turns the SSE renderers off without an error when its compile check of the header
# fails, and clones the header from the network when it does not find one. Either would leave a
# library that builds and runs but computes color with different arithmetic from the x86-64 build.
if(NOT BLOOM_OPENCOLORIO_CACHE OR NOT EXISTS "${BLOOM_OPENCOLORIO_CACHE}")
    message(FATAL_ERROR "BLOOM_OPENCOLORIO_CACHE must name OpenColorIO's CMakeCache.txt.")
endif()
if(NOT BLOOM_SSE2NEON_SOURCE_DIR)
    message(FATAL_ERROR "BLOOM_SSE2NEON_SOURCE_DIR is required.")
endif()

function(bloom_cache_value output name)
    file(STRINGS "${BLOOM_OPENCOLORIO_CACHE}" bloom_lines REGEX "^${name}:[A-Z]+=")
    set(bloom_value "")
    foreach(bloom_line IN LISTS bloom_lines)
        string(REGEX REPLACE "^[^=]*=" "" bloom_value "${bloom_line}")
    endforeach()
    set(${output} "${bloom_value}" PARENT_SCOPE)
endfunction()

bloom_cache_value(bloom_include_dir sse2neon_INCLUDE_DIR)
bloom_cache_value(bloom_supported COMPILER_SUPPORTS_SSE_WITH_SSE2NEON)

if(NOT bloom_include_dir STREQUAL BLOOM_SSE2NEON_SOURCE_DIR)
    message(FATAL_ERROR
        "OpenColorIO did not take sse2neon from the locked source: it used "
        "'${bloom_include_dir}', not '${BLOOM_SSE2NEON_SOURCE_DIR}'.")
endif()
if(NOT bloom_supported)
    message(FATAL_ERROR
        "OpenColorIO's compile check of sse2neon failed, so it would build without its SSE "
        "renderers and compute color with different arithmetic from the x86-64 build. See "
        "CMakeFiles/CMakeConfigureLog.yaml in OpenColorIO's build directory.")
endif()
message(STATUS "OpenColorIO builds its SSE renderers through the locked sse2neon header")

add_library(bloom_media_cache STATIC)

target_sources(
    bloom_media_cache
    PRIVATE
        source_mirror.cpp
        source_mirror_settings.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/media/cache/source_mirror.hpp
            include/bloom/media/cache/source_mirror_settings.hpp
)

target_compile_features(bloom_media_cache PUBLIC cxx_std_20)
target_link_libraries(bloom_media_cache PUBLIC bloom_core bloom_render bloom_media_image bloom_runtime
                      PRIVATE bloom_platform)
bloom_enable_warnings(bloom_media_cache)
bloom_enable_strict_floating_point(bloom_media_cache)

if(BUILD_TESTING)
    include(BloomTesting)
    # #718: the local mirror of source media on slow storage.
    add_executable(bloom_media_source_mirror_test tests/source_mirror_tests.cpp)
    target_link_libraries(bloom_media_source_mirror_test PRIVATE bloom_media_cache)
    bloom_enable_warnings(bloom_media_source_mirror_test)
    bloom_add_test(NAME bloom.media.source-mirror COMMAND bloom_media_source_mirror_test
                   LABELS unit media cache TIMEOUT 120)
endif()

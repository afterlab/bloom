# SVG scene reader (#343): bytes of an SVG file to a tree of groups and shapes. Qt-free, no
# third-party code, nothing fetched or run.
add_library(bloom_media_vector STATIC svg_xml.cpp svg_values.cpp svg_scene.cpp)
target_sources(bloom_media_vector PUBLIC FILE_SET HEADERS BASE_DIRS include
    FILES include/bloom/media/svg_scene.hpp)
target_compile_features(bloom_media_vector PUBLIC cxx_std_20)
target_link_libraries(bloom_media_vector PUBLIC bloom_core)
bloom_enable_warnings(bloom_media_vector)
# The Python module links this library statically.
set_target_properties(bloom_media_vector PROPERTIES POSITION_INDEPENDENT_CODE ON)
bloom_enable_strict_floating_point(bloom_media_vector)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_media_svg_scene_test tests/svg_scene_tests.cpp)
    target_link_libraries(bloom_media_svg_scene_test PRIVATE bloom_media_vector)
    bloom_enable_warnings(bloom_media_svg_scene_test)
    bloom_add_test(NAME bloom.media.svg-scene COMMAND bloom_media_svg_scene_test LABELS unit media)
endif()

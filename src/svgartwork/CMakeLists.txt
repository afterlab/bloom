# ADR 0038, #343, #642: the SVG artwork builder, one description of an SVG scene as document records,
# written into a Draft by the editable import and into a compile-time expansion by the Vector Artwork
# layer (docs/architecture/vector-artwork.md). Its own module so only its users (commands and runtime)
# link the SVG reader and the platform font catalogue, which document may not depend on.
add_library(bloom_svg_artwork STATIC svg_artwork.cpp svg_artwork_expansion.cpp)
target_sources(bloom_svg_artwork PUBLIC FILE_SET HEADERS BASE_DIRS include
    FILES include/bloom/svgartwork/svg_artwork.hpp)
target_compile_features(bloom_svg_artwork PUBLIC cxx_std_20)
target_link_libraries(bloom_svg_artwork PUBLIC bloom_document PRIVATE bloom_media_vector bloom_platform)
bloom_enable_warnings(bloom_svg_artwork)

if(BUILD_TESTING)
    include(BloomTesting)

    add_executable(bloom_svgartwork_test tests/svg_artwork_tests.cpp)
    target_link_libraries(bloom_svgartwork_test PRIVATE bloom_svg_artwork bloom_media_vector
                                                        bloom_platform)
    bloom_enable_warnings(bloom_svgartwork_test)
    bloom_add_test(NAME bloom.svgartwork.svg-artwork COMMAND bloom_svgartwork_test LABELS unit)
endif()

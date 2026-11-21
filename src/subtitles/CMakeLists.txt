# #752: the subtitle file readers and writers (docs/architecture/subtitles.md). Qt-free; SRT,
# WebVTT, TTML/IMSC 1.1 Text (through the locked Expat, docs/architecture/dependency-intake.md
# "Expat") and ASS/SSA read into a document::CueListValue, and SRT, WebVTT and IMSC 1.1 Text
# written from one. Its own module so only its users (commands, host, scripting, mcp) link Expat.
bloom_find_dependency(expat)

add_library(bloom_subtitles STATIC
    ass.cpp
    common.cpp
    encoding.cpp
    load.cpp
    srt.cpp
    ttml.cpp
    vtt.cpp
    webvtt.cpp)
target_sources(bloom_subtitles PUBLIC FILE_SET HEADERS BASE_DIRS include
    FILES include/bloom/subtitles/subtitles.hpp include/bloom/subtitles/webvtt.hpp)
target_compile_features(bloom_subtitles PUBLIC cxx_std_20)
target_link_libraries(bloom_subtitles PUBLIC bloom_document bloom_core PRIVATE expat::expat)
bloom_enable_warnings(bloom_subtitles)
target_compile_definitions(bloom_subtitles PRIVATE XML_STATIC) # the locked Expat is static

if(BUILD_TESTING)
    include(BloomTesting)
    bloom_find_dependency(yyjson)

    add_executable(bloom_subtitles_webvtt_conformance_test tests/webvtt_conformance_tests.cpp)
    target_link_libraries(bloom_subtitles_webvtt_conformance_test PRIVATE bloom_subtitles
                                                                          yyjson::yyjson)
    bloom_enable_warnings(bloom_subtitles_webvtt_conformance_test)
    bloom_add_test(NAME bloom.subtitles.webvtt-conformance
        COMMAND bloom_subtitles_webvtt_conformance_test "${PROJECT_SOURCE_DIR}/tests/fixtures/subtitles"
        LABELS unit)

    add_executable(bloom_subtitles_test tests/subtitles_tests.cpp)
    target_link_libraries(bloom_subtitles_test PRIVATE bloom_subtitles)
    bloom_enable_warnings(bloom_subtitles_test)
    bloom_add_test(NAME bloom.subtitles.formats COMMAND bloom_subtitles_test LABELS unit)
endif()

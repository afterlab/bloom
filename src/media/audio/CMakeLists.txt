find_package(Threads REQUIRED)

add_library(bloom_media_dr_wav OBJECT wav_backend.cpp wav_backend.hpp)
target_compile_features(bloom_media_dr_wav PRIVATE cxx_std_20)
target_link_libraries(bloom_media_dr_wav PRIVATE bloom_core)
target_include_directories(
    bloom_media_dr_wav
    PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dr_wav"
)
if(MSVC)
    target_compile_options(bloom_media_dr_wav PRIVATE /w)
else()
    target_compile_options(bloom_media_dr_wav PRIVATE -w)
endif()
bloom_enable_strict_floating_point(bloom_media_dr_wav)

add_library(bloom_media_minimp3 OBJECT mp3_backend.cpp mp3_backend.hpp)
target_compile_features(bloom_media_minimp3 PRIVATE cxx_std_20)
target_link_libraries(bloom_media_minimp3 PRIVATE bloom_core)
target_include_directories(
    bloom_media_minimp3
    PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minimp3"
)
if(MSVC)
    target_compile_options(bloom_media_minimp3 PRIVATE /w)
else()
    target_compile_options(bloom_media_minimp3 PRIVATE -w)
endif()
bloom_enable_strict_floating_point(bloom_media_minimp3)

add_library(bloom_media_miniaudio OBJECT playback/miniaudio_backend.cpp playback/miniaudio_filters.cpp)
target_compile_features(bloom_media_miniaudio PRIVATE cxx_std_20)
target_include_directories(
    bloom_media_miniaudio
    PRIVATE
        include
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/playback/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio"
)
target_link_libraries(bloom_media_miniaudio PRIVATE bloom_core Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    target_link_libraries(bloom_media_miniaudio PRIVATE m)
endif()
if(MSVC)
    target_compile_options(bloom_media_miniaudio PRIVATE /w)
else()
    target_compile_options(bloom_media_miniaudio PRIVATE -w)
endif()
bloom_enable_strict_floating_point(bloom_media_miniaudio)

# libebur128 (EBU R128 / ITU-R BS.1770 loudness and true peak), vendored like the other reference
# libraries. It is C, so the object library is compiled without the project's warning set.
add_library(bloom_media_ebur128 OBJECT ../third_party/ebur128/ebur128.c)
target_include_directories(
    bloom_media_ebur128
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ebur128"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ebur128/queue"
)
if(MSVC)
    target_compile_options(bloom_media_ebur128 PRIVATE /w)
else()
    target_compile_options(bloom_media_ebur128 PRIVATE -w)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(bloom_media_ebur128 PRIVATE m)
endif()

add_library(
    bloom_media_audio
    STATIC
    analysis.cpp
    audio.cpp
    channel_layout.cpp
    effects.cpp
    loudness.cpp
    playback/audio_engine.cpp
)
target_sources(
    bloom_media_audio
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include playback/include
        FILES
            include/bloom/media/audio/analysis.hpp
            include/bloom/media/audio/audio.hpp
            include/bloom/media/audio/channel_layout.hpp
            include/bloom/media/audio/effects.hpp
            include/bloom/media/audio/loudness.hpp
            playback/include/bloom/media/audio/playback/audio_engine.hpp
)
target_compile_features(bloom_media_audio PUBLIC cxx_std_20)
target_include_directories(
    bloom_media_audio
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ebur128"
)
target_link_libraries(
    bloom_media_audio
    PUBLIC bloom_core
    PRIVATE
        bloom_media_dr_wav
        bloom_media_ebur128
        bloom_media_minimp3
        bloom_media_miniaudio
        Threads::Threads
        ${CMAKE_DL_LIBS}
)
if(UNIX AND NOT APPLE)
    target_link_libraries(bloom_media_audio PRIVATE m)
endif()
bloom_enable_warnings(bloom_media_audio)
bloom_enable_strict_floating_point(bloom_media_audio)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_media_audio_test tests/audio_tests.cpp)
    add_executable(
        bloom_media_audio_playback_test
        playback/tests/audio_engine_tests.cpp
    )
    add_executable(bloom_media_audio_effects_test tests/effects_tests.cpp)
    add_executable(bloom_media_audio_analysis_test tests/analysis_tests.cpp)
    target_link_libraries(bloom_media_audio_analysis_test PRIVATE bloom_media_audio)
    bloom_enable_warnings(bloom_media_audio_analysis_test)
    bloom_enable_strict_floating_point(bloom_media_audio_analysis_test)
    bloom_add_test(
        NAME bloom.media.audio-analysis
        COMMAND bloom_media_audio_analysis_test
        LABELS unit media audio analysis
        TIMEOUT 120
    )
    add_executable(bloom_media_loudness_test tests/loudness_tests.cpp)
    target_link_libraries(bloom_media_loudness_test PRIVATE bloom_media_audio)
    bloom_enable_warnings(bloom_media_loudness_test)
    bloom_enable_strict_floating_point(bloom_media_loudness_test)
    bloom_add_test(
        NAME bloom.media.loudness
        COMMAND bloom_media_loudness_test
        LABELS unit media audio loudness
        TIMEOUT 120
    )
    target_link_libraries(bloom_media_audio_effects_test PRIVATE bloom_media_audio)
    target_include_directories(
        bloom_media_audio_effects_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/playback/include"
    )
    bloom_enable_warnings(bloom_media_audio_effects_test)
    bloom_enable_strict_floating_point(bloom_media_audio_effects_test)
    bloom_add_test(
        NAME bloom.media.audio-effects
        COMMAND bloom_media_audio_effects_test
        LABELS unit media audio effects
        TIMEOUT 120
    )
    target_link_libraries(bloom_media_audio_test PRIVATE bloom_media_audio)
    target_link_libraries(bloom_media_audio_playback_test PRIVATE bloom_media_audio)
    target_include_directories(bloom_media_audio_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(
        bloom_media_audio_test
        PRIVATE BLOOM_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    target_include_directories(
        bloom_media_audio_playback_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/playback/include"
    )
    bloom_enable_warnings(bloom_media_audio_test)
    bloom_enable_strict_floating_point(bloom_media_audio_test)
    bloom_enable_warnings(bloom_media_audio_playback_test)
    bloom_enable_strict_floating_point(bloom_media_audio_playback_test)
    bloom_add_test(
        NAME bloom.media.audio
        COMMAND bloom_media_audio_test
        LABELS unit media audio
        TIMEOUT 60
    )
    bloom_add_test(
        NAME bloom.media.audio-playback
        COMMAND bloom_media_audio_playback_test
        LABELS unit media audio playback
        TIMEOUT 60
    )
endif()

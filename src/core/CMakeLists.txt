add_library(bloom_core STATIC)

target_sources(
    bloom_core
    PRIVATE
        rational_time.cpp
        rational_interval.cpp
        frame_time_mapping.cpp
        scalar_primitives.cpp
        value_primitives.cpp
        field_program.cpp
        film_grain_noise.cpp
        dither_pattern.cpp
        camera_shake.cpp
        light_flicker.cpp
        motion_values.cpp
        sha256.cpp
        utf8.cpp
        file_uri.cpp
        float_chars.cpp
        safe_parse.cpp
        telemetry_counters.cpp
        worker_thread.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/core/artifact_target_key.hpp
            include/bloom/core/blend_mode.hpp
            include/bloom/core/field_program.hpp
            include/bloom/core/film_grain_noise.hpp
            include/bloom/core/dither_pattern.hpp
            include/bloom/core/camera_shake.hpp
            include/bloom/core/light_flicker.hpp
            include/bloom/core/motion_values.hpp
            include/bloom/core/color.hpp
            include/bloom/core/float_chars.hpp
            include/bloom/core/floating_point.hpp
            include/bloom/core/frame_time_mapping.hpp
            include/bloom/core/id.hpp
            include/bloom/core/pixel_aspect_ratio.hpp
            include/bloom/core/rational_interval.hpp
            include/bloom/core/rational_time.hpp
            include/bloom/core/safe_parse.hpp
            include/bloom/core/scalar_primitives.hpp
            include/bloom/core/sha256.hpp
            include/bloom/core/telemetry_counters.hpp
            include/bloom/core/utf8.hpp
            include/bloom/core/file_uri.hpp
            include/bloom/core/value_primitives.hpp
            include/bloom/core/worker_thread.hpp
)

# #525: the field kernel is one source compiled as C++ (field_program.cpp includes it) and handed to
# the GPU program as text. Embed the text byte for byte, regenerated whenever the kernel changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/field_kernel.glsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/field_kernel.glsl" BLOOM_FIELD_KERNEL_HEX HEX)
string(LENGTH "${BLOOM_FIELD_KERNEL_HEX}" BLOOM_FIELD_KERNEL_HEX_LENGTH)
math(EXPR BLOOM_FIELD_KERNEL_SIZE "${BLOOM_FIELD_KERNEL_HEX_LENGTH} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BLOOM_FIELD_KERNEL_BYTES
    "${BLOOM_FIELD_KERNEL_HEX}")
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/field_kernel_source.cpp"
    CONTENT "#include \"bloom/core/field_program.hpp\"\n\nnamespace bloom::core::field {\nnamespace {\nconstexpr unsigned char kFieldKernelSource[] = {${BLOOM_FIELD_KERNEL_BYTES}0x00};\n} // namespace\n\nstd::string_view fieldKernelGlsl() noexcept {\n    return {reinterpret_cast<const char*>(kFieldKernelSource), ${BLOOM_FIELD_KERNEL_SIZE}};\n}\n\n} // namespace bloom::core::field\n"
    @ONLY)
target_sources(bloom_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/field_kernel_source.cpp")

target_compile_features(bloom_core PUBLIC cxx_std_20)
# worker_thread.cpp starts native threads with a stated stack size.
find_package(Threads REQUIRED)
target_link_libraries(bloom_core PUBLIC Threads::Threads)
bloom_enable_warnings(bloom_core)

# Scalar primitives are the backend-neutral CPU reference. Keep ordinary expressions unfused,
# honor the runtime rounding mode check, and reserve fused semantics for explicit std::fma calls.
bloom_enable_strict_floating_point(bloom_core)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_core_pixel_aspect_ratio_test tests/pixel_aspect_ratio_tests.cpp)
    target_link_libraries(bloom_core_pixel_aspect_ratio_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_pixel_aspect_ratio_test)
    bloom_add_test(
        NAME bloom.core.pixel_aspect_ratio
        COMMAND bloom_core_pixel_aspect_ratio_test
        LABELS unit
    )

    add_executable(bloom_core_worker_thread_test tests/worker_thread_tests.cpp)
    target_link_libraries(bloom_core_worker_thread_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_worker_thread_test)
    bloom_add_test(
        NAME bloom.core.worker_thread
        COMMAND bloom_core_worker_thread_test
        LABELS unit
    )

    add_executable(bloom_core_telemetry_counters_test tests/telemetry_counters_tests.cpp)
    target_link_libraries(bloom_core_telemetry_counters_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_telemetry_counters_test)
    bloom_add_test(
        NAME bloom.core.telemetry_counters
        COMMAND bloom_core_telemetry_counters_test
        LABELS unit
    )

    add_executable(bloom_core_rational_interval_test tests/rational_interval_tests.cpp)
    target_link_libraries(bloom_core_rational_interval_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_rational_interval_test)
    bloom_enable_strict_floating_point(bloom_core_rational_interval_test)
    bloom_add_test(
        NAME bloom.core.rational_interval
        COMMAND bloom_core_rational_interval_test
        LABELS unit primitives
    )

    add_executable(bloom_core_frame_time_mapping_test tests/frame_time_mapping_tests.cpp)
    target_link_libraries(bloom_core_frame_time_mapping_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_frame_time_mapping_test)
    bloom_add_test(
        NAME bloom.core.frame_time_mapping
        COMMAND bloom_core_frame_time_mapping_test
        LABELS unit time
    )

    add_executable(bloom_core_scalar_primitives_test tests/scalar_primitives_tests.cpp)
    target_link_libraries(bloom_core_scalar_primitives_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_scalar_primitives_test)
    bloom_enable_strict_floating_point(bloom_core_scalar_primitives_test)
    bloom_add_test(
        NAME bloom.core.scalar_primitives
        COMMAND bloom_core_scalar_primitives_test
        LABELS unit primitives
    )

    add_executable(bloom_core_value_primitives_test tests/value_primitives_tests.cpp)
    target_link_libraries(bloom_core_value_primitives_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_value_primitives_test)
    bloom_enable_strict_floating_point(bloom_core_value_primitives_test)
    bloom_add_test(
        NAME bloom.core.value_primitives
        COMMAND bloom_core_value_primitives_test
        LABELS unit primitives
    )

    add_executable(bloom_core_camera_shake_test tests/camera_shake_tests.cpp)
    target_link_libraries(bloom_core_camera_shake_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_camera_shake_test)
    bloom_enable_strict_floating_point(bloom_core_camera_shake_test)
    bloom_add_test(
        NAME bloom.core.camera_shake
        COMMAND bloom_core_camera_shake_test
        LABELS unit
    )

    add_executable(bloom_core_light_flicker_test tests/light_flicker_tests.cpp)
    target_link_libraries(bloom_core_light_flicker_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_light_flicker_test)
    bloom_enable_strict_floating_point(bloom_core_light_flicker_test)
    bloom_add_test(
        NAME bloom.core.light_flicker
        COMMAND bloom_core_light_flicker_test
        LABELS unit
    )

    add_executable(bloom_core_motion_values_test tests/motion_values_tests.cpp)
    target_link_libraries(bloom_core_motion_values_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_motion_values_test)
    bloom_enable_strict_floating_point(bloom_core_motion_values_test)
    bloom_add_test(
        NAME bloom.core.motion_values
        COMMAND bloom_core_motion_values_test
        LABELS unit
    )

    add_executable(bloom_core_field_program_test tests/field_program_tests.cpp)
    target_link_libraries(bloom_core_field_program_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_field_program_test)
    bloom_enable_strict_floating_point(bloom_core_field_program_test)
    bloom_add_test(
        NAME bloom.core.field_program
        COMMAND bloom_core_field_program_test
        LABELS unit
    )

    add_executable(bloom_core_procedural_warp_field_test tests/procedural_warp_field_tests.cpp)
    target_link_libraries(bloom_core_procedural_warp_field_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_procedural_warp_field_test)
    bloom_enable_strict_floating_point(bloom_core_procedural_warp_field_test)
    bloom_add_test(
        NAME bloom.core.procedural_warp_field
        COMMAND bloom_core_procedural_warp_field_test
        LABELS unit
    )

    add_executable(bloom_core_dither_pattern_test tests/dither_pattern_tests.cpp)
    target_link_libraries(bloom_core_dither_pattern_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_dither_pattern_test)
    bloom_enable_strict_floating_point(bloom_core_dither_pattern_test)
    bloom_add_test(
        NAME bloom.core.dither_pattern
        COMMAND bloom_core_dither_pattern_test
        LABELS unit
    )

    add_executable(bloom_core_sha256_test tests/sha256_tests.cpp)
    target_link_libraries(bloom_core_sha256_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_sha256_test)
    bloom_add_test(
        NAME bloom.core.sha256
        COMMAND bloom_core_sha256_test
        LABELS unit identity
    )

    add_executable(bloom_core_utf8_test tests/utf8_tests.cpp)
    target_link_libraries(bloom_core_utf8_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_utf8_test)
    bloom_add_test(NAME bloom.core.utf8 COMMAND bloom_core_utf8_test LABELS unit text)

    add_executable(bloom_core_file_uri_test tests/file_uri_tests.cpp)
    target_link_libraries(bloom_core_file_uri_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_file_uri_test)
    bloom_add_test(NAME bloom.core.file-uri COMMAND bloom_core_file_uri_test LABELS unit path)

    add_executable(bloom_core_float_chars_test tests/float_chars_tests.cpp)
    target_link_libraries(bloom_core_float_chars_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_float_chars_test)
    bloom_enable_strict_floating_point(bloom_core_float_chars_test)
    bloom_add_test(
        NAME bloom.core.float-chars
        COMMAND bloom_core_float_chars_test
        LABELS unit text primitives
    )

    add_executable(bloom_core_safe_parse_test tests/safe_parse_tests.cpp)
    target_link_libraries(bloom_core_safe_parse_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_safe_parse_test)
    bloom_enable_strict_floating_point(bloom_core_safe_parse_test)
    bloom_add_test(
        NAME bloom.core.safe-parse
        COMMAND bloom_core_safe_parse_test
        LABELS unit text primitives
    )
endif()

find_package(Threads REQUIRED)

add_library(bloom_runtime STATIC)

target_sources(
    bloom_runtime
    PRIVATE
        row_band_execution.cpp
        memory_budget_ledger.cpp
        memory_eviction_priority.cpp
        memory_pressure_monitor.cpp
        process_memory_sample.cpp
        profiler_ledger_adapter.cpp
        profiler_process_reads.cpp
        profiler_sampler.cpp
        profiler_snapshot_json.cpp
        source_working_set.cpp
        task_primitives.cpp
        task_completion.cpp
        task_gpu_executor.cpp
        task_gpu_executor_lease.cpp
        task_scheduler.cpp
        task_scheduler_admission.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/cancellation.hpp
            include/bloom/runtime/memory_budget_ledger.hpp
            include/bloom/runtime/memory_eviction_priority.hpp
            include/bloom/runtime/memory_pool_counters.hpp
            include/bloom/runtime/memory_pressure_monitor.hpp
            include/bloom/runtime/process_memory_sample.hpp
            include/bloom/runtime/profiler_ledger_adapter.hpp
            include/bloom/runtime/profiler_process_reads.hpp
            include/bloom/runtime/profiler_sampler.hpp
            include/bloom/runtime/profiler_seqlock.hpp
            include/bloom/runtime/profiler_snapshot.hpp
            include/bloom/runtime/profiler_snapshot_json.hpp
            include/bloom/runtime/row_band_execution.hpp
            include/bloom/runtime/source_working_set.hpp
            include/bloom/runtime/task_gpu_executor.hpp
            include/bloom/runtime/task_completion.hpp
            include/bloom/runtime/task_scheduler.hpp
            include/bloom/runtime/task_types.hpp
            include/bloom/runtime/task_budget_view.hpp
)

target_compile_features(bloom_runtime PUBLIC cxx_std_20)
target_link_libraries(bloom_runtime PUBLIC bloom_core Threads::Threads)
bloom_enable_warnings(bloom_runtime)

# Font IDs to font files: installed fonts, the per-user cache, Google Fonts downloads and the
# fallback table (docs/architecture/fonts.md). No Qt and no linked network library: the transport is a seam
# the application fills.
add_library(bloom_runtime_fonts STATIC)

target_sources(
    bloom_runtime_fonts
    PRIVATE
        font_fallback.cpp
        font_resolver.cpp
        google_catalogue.cpp
        google_catalogue_data.cpp
        google_fonts.cpp
        headless_font_transport.cpp
        system_font_index.cpp
        variable_font_cff2.cpp
        variable_font_instance.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/font_fallback.hpp
            include/bloom/runtime/font_resolver.hpp
            include/bloom/runtime/font_transport.hpp
            include/bloom/runtime/google_catalogue.hpp
            include/bloom/runtime/google_fonts.hpp
            include/bloom/runtime/headless_font_transport.hpp
            include/bloom/runtime/system_font_index.hpp
            include/bloom/runtime/variable_font_instance.hpp
)

# The headless transport (command-line tools, MCP, Python) speaks through the system's HTTP client
# library: libcurl loaded at run time on Linux and macOS, WinHTTP on Windows.
if(WIN32)
    target_sources(bloom_runtime_fonts PRIVATE headless_font_transport_winhttp.cpp)
    target_link_libraries(bloom_runtime_fonts PRIVATE winhttp)
else()
    target_sources(bloom_runtime_fonts PRIVATE headless_font_transport_curl.cpp)
    target_link_libraries(bloom_runtime_fonts PRIVATE ${CMAKE_DL_LIBS})
endif()

target_compile_features(bloom_runtime_fonts PUBLIC cxx_std_20)
target_link_libraries(bloom_runtime_fonts PUBLIC bloom_core bloom_document bloom_platform
                                                 Threads::Threads)
bloom_enable_warnings(bloom_runtime_fonts)

add_library(bloom_runtime_compiler STATIC)

target_sources(
    bloom_runtime_compiler
    PRIVATE
        compile_diagnostics.cpp
        compiled_plan.cpp
        compiled_plan_cache.cpp
        composition_time.cpp
        plan_time_dependence.cpp
        curve_compilation.cpp
        cryptomatte_matte_list.cpp
        snapshot_compiler.cpp
        vector_artwork_scene_cache.cpp
        compositing_recipes.cpp
        depth_effect_recipes.cpp
        motion_blur_recipes.cpp
        codec_degrade_recipes.cpp
        look_back_recipes.cpp
        auto_exposure_recipes.cpp
        histogram_recipes.cpp
        statistics_recipes.cpp
        compositing_recipes.hpp
        missing_font_layers.cpp
        plan_time_scopes.cpp
        motion_shutter.cpp
        profiler_gpu_reader.cpp
        layer_time_projection.cpp
        snapshot_compiler_entries.ipp
        snapshot_compiler_layer_time.ipp
        snapshot_compiler_lowering.ipp
        snapshot_compiler_fields.ipp
        snapshot_compiler_warp.ipp
        snapshot_compiler_recipes.ipp
        snapshot_compiler_support.hpp
        snapshot_compiler_value_graph.ipp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/audio_analysis_source.hpp
            include/bloom/runtime/compiled_plan.hpp
            include/bloom/runtime/missing_font_layers.hpp
            include/bloom/runtime/compiled_composition_source.hpp
            include/bloom/runtime/channel_expression.hpp
            include/bloom/runtime/channel_inspection.hpp
            include/bloom/runtime/channel_inspection_listing.hpp
            include/bloom/runtime/cryptomatte_inspection.hpp
            include/bloom/runtime/channel_layers.hpp
            include/bloom/runtime/cryptomatte_matte_list.hpp
            include/bloom/runtime/compiled_plan_cache.hpp
            include/bloom/runtime/composition_time.hpp
            include/bloom/runtime/layer_time_projection.hpp
            include/bloom/runtime/compiled_value_graph.hpp
            include/bloom/runtime/operation_index.hpp
            include/bloom/runtime/curve_compilation.hpp
            include/bloom/runtime/node_definition_registry.hpp
            include/bloom/runtime/snapshot_compiler.hpp
            include/bloom/runtime/profiler_gpu_reader.hpp
)

target_compile_features(bloom_runtime_compiler PUBLIC cxx_std_20)
target_link_libraries(bloom_runtime_compiler PUBLIC bloom_runtime bloom_runtime_fonts bloom_document
                                                    bloom_render
                                             PRIVATE bloom_svg_artwork bloom_media_vector)
bloom_enable_warnings(bloom_runtime_compiler)

add_library(bloom_runtime_evaluator STATIC)

target_sources(
    bloom_runtime_evaluator
    PRIVATE
        operation_cache.cpp
        text_metrics.cpp
        cpu_layer_time_resolution.cpp
        video_source.cpp
        composite_frame_cache.cpp
        video_source.hpp
        image_source.cpp
        footage_time.cpp
        footage_interpretation_pass.cpp
        image_sequence_read_ahead.cpp
        channel_resolution.cpp
        channel_shuffle.cpp
        channel_source_time.cpp
        channel_static.cpp
        channel_inspection_listing.cpp
        cryptomatte_inspection.cpp
        channel_layers.cpp
        cryptomatte.cpp
        image_source.hpp
        input_color_context.cpp
        input_color_context.hpp
        layer_parent_transform.hpp
        operation_key.hpp
        animation_sampling.cpp
        grade_curve.cpp
        keyframe_easing.cpp
        cpu_composition_evaluator.cpp
        cpu_composition_resolution.hpp
        layer_bounds_query.cpp
        viewer_pick.cpp
        cpu_gpu_scene_preparation.cpp
        gpu_processing_phase_policy.cpp
        gpu_scene_nested.cpp
        gpu_scene_nested.hpp
        gpu_scene_preparation_builders.hpp
        gpu_scene_preparation_private.hpp
        gpu_scene_preparation_common.hpp
        gpu_scene_coverage.cpp
        gpu_scene_shape_coverage.cpp
        gpu_scene_coverage.hpp
        gpu_scene_coverage_cache.cpp
        gpu_media_preparation.cpp
        gpu_media_preparation.hpp
        gpu_scene_media_layer.cpp
        gpu_scene_media_layer.hpp
        image_effect.cpp
        layer_mask_coverage.cpp
        denoise_image_effect.cpp
        directional_blur_image_effect.cpp
        luma_key_image_effect.cpp
        color_grade_curves.cpp
        color_grade_image_effect.cpp
        vignette_image_effect.cpp
        depth_effect_image_effect.cpp
        lens_flare_image_effect.cpp
        film_grain_image_effect.cpp
        dither_image_effect.cpp
        codec_degrade_image_effect.cpp
        auto_exposure_image_effect.cpp
        pixel_math_image_effect.cpp
        color_tool_image_effect.cpp
        histogram_image_effect.cpp
        scope_frame_measure.cpp
        scope_measure.cpp
        scope_trace_map.cpp
        scope_graticule.cpp
        scope_picture.cpp
        scope_png.cpp
        statistics_image_effect.cpp
        audio_analysis.cpp
        audio_mix_render.cpp
        audio_sound.cpp
        audio_table_analysis.cpp
        audio_table_values.cpp
        image_statistics.cpp
        audio_plot_image_effect.cpp
        motion_blur_image_effect.cpp
        motion_blur_planner.cpp
        sharpen_image_effect.cpp
        field_raster.cpp
        point_tracker.cpp
        point_tracker_job.cpp
        channel_image_effect.cpp
        separable_image_effect.cpp
        image_effect.hpp
        discrete_image_effect.hpp
        cpu_composition_evaluator_support.hpp
        evaluation.cpp
        prepared_preview_frame.cpp
        qualified_display_preparation.cpp
        qualified_display_processor_provider.cpp
        reference_display_preparation.cpp
        gpu_neutral_display_qualification.cpp
        gpu_neutral_display_qualification_fixtures.cpp
        gpu_resident_preview_qualification.cpp
        gpu_resident_preview_qualification_ops.cpp
        gpu_resident_preview_qualification_fixtures.cpp
        gpu_resident_preview_qualification_display.cpp
        gpu_resident_preview_qualification_internal.hpp
        gpu_preview_display_product.cpp
        gpu_prepared_packed_preview_product.cpp
        gpu_resident_preview_product.cpp
        gpu_resident_frame_lease.cpp
        gpu_presentation_coordinator.cpp
        gpu_presentation_coordinator_pump.cpp
        gpu_presentation_coordinator_state.hpp
        gpu_presentation_coordinator_private.hpp
        gpu_presentation_client.cpp
        gpu_preview_display_service.cpp
        gpu_preview_display_service_cache_purge.cpp
        gpu_preview_display_service_jobs.cpp
        gpu_preview_display_service_private.hpp
        gpu_preview_resident_capacity.cpp
        gpu_preview_resident_capacity_resolver.cpp
        gpu_preview_display_service_loop.hpp
        gpu_preview_display_service_presentation.cpp
        gpu_preview_display_service_presentation_private.hpp
        gpu_preview_display_service_packed_transfer.cpp
        gpu_preview_display_service_resident.cpp
        gpu_preview_display_service_resident_jobs.cpp
        gpu_preview_display_service_resident_private.hpp
        gpu_scene_cache.cpp
        gpu_scene_executor.cpp
        gpu_scene_executor_planner.cpp
        gpu_scene_executor_execution.cpp
        gpu_scene_executor_batch.cpp
        gpu_scene_executor_poll.cpp
        gpu_scene_executor_lookahead.cpp
        gpu_scene_executor_coverage.cpp
        gpu_scene_executor_ocio.cpp
        gpu_scene_executor_point_resample.cpp
        gpu_scene_executor_cryptomatte.cpp
        gpu_scene_executor_private.hpp
        gpu_scene_executor_native_mapping.hpp
        gpu_process_frame.cpp
        gpu_process_frame_preparation_private.cpp
        gpu_process_frame_preparation_private.hpp
        gpu_process_frame_readback.cpp
        gpu_process_frame_readback_private.hpp
        gpu_process_frame_scope.cpp
        gpu_process_frame_scope_private.hpp
        gpu_process_frame_support_private.hpp
        gpu_ocio_wrapper.cpp
        gpu_ocio_command.cpp
        gpu_ocio_preparation.cpp
        gpu_ocio_context.cpp
        gpu_ocio_program_executor.cpp
        gpu_ocio_program_retained_bytes.hpp
        gpu_output_color.cpp
        gpu_ocio_display_arm.cpp
        gpu_ocio_display_program_service.cpp
        view_adjust.cpp
        view_adjust_support.hpp
        value_graph_evaluation.cpp
        function_body.cpp
        value_utility_support.hpp
        value_utility_kernels.cpp
        value_utility_data_blocks.cpp
        value_utility_conversions.cpp
        value_utility_time.cpp
        value_utility_strings.cpp
        value_utility_numeric.cpp
        value_utility_plot.cpp
        value_utility_path.cpp
        value_utility_path_stroke.cpp
        value_utility_path_sources.cpp
        value_utility_path_resample.cpp
        path_resample.cpp
        path_morph.cpp
        value_utility_path_warp.cpp
        value_utility_path_morph.cpp
        path_warp.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/operation_cache.hpp
            include/bloom/runtime/text_metrics.hpp
            include/bloom/runtime/animation_sampling.hpp
            include/bloom/runtime/grade_curve.hpp
            include/bloom/runtime/keyframe_easing.hpp
            include/bloom/runtime/audio_analysis.hpp
            include/bloom/runtime/audio_mix_render.hpp
            include/bloom/runtime/audio_sound.hpp
            include/bloom/runtime/audio_table_analysis.hpp
            include/bloom/runtime/image_statistics.hpp
            include/bloom/runtime/audio_stream_key.hpp
            include/bloom/runtime/video_source_report.hpp
            include/bloom/runtime/cpu_composition_evaluator.hpp
            include/bloom/runtime/evaluation.hpp
            include/bloom/runtime/point_tracker.hpp
            include/bloom/runtime/point_tracker_job.hpp
            include/bloom/runtime/layer_bounds_query.hpp
            include/bloom/runtime/gpu_neutral_display_qualification.hpp
            include/bloom/runtime/gpu_coverage_contract.hpp
            include/bloom/runtime/gpu_resident_preview_qualification.hpp
            include/bloom/runtime/gpu_preview_display_product.hpp
            include/bloom/runtime/gpu_prepared_packed_preview_product.hpp
            include/bloom/runtime/gpu_resident_preview_product.hpp
            include/bloom/runtime/gpu_preview_display_service.hpp
            include/bloom/runtime/gpu_preview_resident_capacity.hpp
            include/bloom/runtime/gpu_presentation_coordinator.hpp
            include/bloom/runtime/gpu_resident_frame_lease.hpp
            include/bloom/runtime/gpu_scene_cache.hpp
            include/bloom/runtime/gpu_scene_coverage_cache.hpp
            include/bloom/runtime/image_sequence_read_ahead.hpp
            include/bloom/runtime/gpu_scene_executor.hpp
            include/bloom/runtime/gpu_process_frame.hpp
            include/bloom/runtime/gpu_scope_measure.hpp
            include/bloom/runtime/scope_frame_measure.hpp
            include/bloom/runtime/scope_picture.hpp
            include/bloom/runtime/gpu_processing_phase_policy.hpp
            include/bloom/runtime/packed_host_retirement.hpp
            include/bloom/runtime/gpu_ocio_wrapper.hpp
            include/bloom/runtime/gpu_ocio_command.hpp
            include/bloom/runtime/gpu_ocio_preparation.hpp
            include/bloom/runtime/gpu_ocio_context.hpp
            include/bloom/runtime/gpu_ocio_program_executor.hpp
            include/bloom/runtime/gpu_ocio_display_arm.hpp
            include/bloom/runtime/gpu_output_color.hpp
            include/bloom/runtime/input_color_context.hpp
            include/bloom/runtime/prepared_gpu_scene.hpp
            include/bloom/runtime/prepared_preview_frame.hpp
            include/bloom/runtime/preview_cpu_stage.hpp
            include/bloom/runtime/preview_gpu_scene_stage.hpp
            include/bloom/runtime/qualified_display_preparation.hpp
            include/bloom/runtime/qualified_display_processor_provider.hpp
            include/bloom/runtime/reference_display_preparation.hpp
            include/bloom/runtime/view_adjust.hpp
            include/bloom/runtime/value_graph_evaluation.hpp
            include/bloom/runtime/function_body.hpp
            include/bloom/runtime/value_utility_kernels.hpp
)

target_compile_features(bloom_runtime_evaluator PUBLIC cxx_std_20)
# bloom_color_ocio (issue #97, task C3): the qualified display-preparation stage applies the C2
# qualified Bloom Neutral CPU display processor (bloom/color/ocio_cpu_display_frame.hpp,
# ocio_cpu_display_processor.hpp) to a ProcessFrame. runtime->color is the correct dependency
# direction (docs/architecture/color-management.md's staged-graph "CPU Display Processor Boundary"
# has color own the adapter and runtime own the staged application); PUBLIC because
# qualified_display_preparation.hpp and qualified_display_processor_provider.hpp expose
# bloom::color value types (PreparedCpuDisplayProcessorHandle, PreparedDisplayFrame) directly in
# their own public signatures, exactly like the existing PUBLIC bloom_render dependency below.
target_link_libraries(bloom_runtime_evaluator PUBLIC bloom_runtime_compiler bloom_render
                                                     bloom_color_ocio bloom_media_image
                                                     bloom_media_cache bloom_media_video bloom_color_video)
target_link_libraries(bloom_runtime_evaluator PRIVATE bloom_color_gpu_shader)
# The scope picture's PNG encoder (scope_png.cpp) deflates through the qualified zlib, like the
# export PNG writer and the media image decoder.
bloom_find_dependency(ZLIB)
target_link_libraries(bloom_runtime_evaluator PRIVATE ZLIB::ZLIBSTATIC)

# The vendored polygon library behind Boolean Paths, Merge Paths and Offset Path (#344, #530). It is
# compiled as one private object library without the project's warning set; its headers are system
# headers to the translation unit that wraps it, and no public Bloom header names one of its types.
add_library(
    bloom_runtime_clipper2
    OBJECT
    third_party/clipper2/src/clipper.engine.cpp
    third_party/clipper2/src/clipper.offset.cpp
    third_party/clipper2/src/clipper.rectclip.cpp
)
target_include_directories(
    bloom_runtime_clipper2
    SYSTEM
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clipper2/include"
)
target_compile_features(bloom_runtime_clipper2 PRIVATE cxx_std_20)
set_target_properties(bloom_runtime_clipper2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(bloom_runtime_clipper2 PRIVATE /w)
else()
    target_compile_options(bloom_runtime_clipper2 PRIVATE -w)
endif()
target_include_directories(
    bloom_runtime_evaluator
    SYSTEM
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clipper2/include"
)
target_link_libraries(bloom_runtime_evaluator PRIVATE bloom_runtime_clipper2)
if(MSVC)
    # The vendored clipping headers instantiate templates whose code generation reports
    # unreachable code (C4702) in the including translation unit, where a pragma around the
    # include cannot reach. Only the two files that include them are exempt.
    set_source_files_properties(value_utility_path.cpp value_utility_path_stroke.cpp
                                PROPERTIES COMPILE_OPTIONS /wd4702)
endif()
bloom_enable_warnings(bloom_runtime_evaluator)
bloom_enable_strict_floating_point(bloom_runtime_evaluator)

# #484: the motion kernel is one source compiled as C++ (motion_blur_image_effect.cpp includes it)
# and handed to the GPU program as text. Embed the text byte for byte, regenerated whenever the
# kernel changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/motion_kernel.glsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/motion_kernel.glsl" BLOOM_MOTION_KERNEL_HEX HEX)
string(LENGTH "${BLOOM_MOTION_KERNEL_HEX}" BLOOM_MOTION_KERNEL_HEX_LENGTH)
math(EXPR BLOOM_MOTION_KERNEL_SIZE "${BLOOM_MOTION_KERNEL_HEX_LENGTH} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BLOOM_MOTION_KERNEL_BYTES
    "${BLOOM_MOTION_KERNEL_HEX}")
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_kernel_source.cpp"
    CONTENT "#include <string_view>\n\nnamespace bloom::runtime::detail {\nnamespace {\nconstexpr unsigned char kMotionKernelSource[] = {${BLOOM_MOTION_KERNEL_BYTES}0x00};\n} // namespace\n\nstd::string_view motionKernelGlsl() noexcept {\n    return {reinterpret_cast<const char*>(kMotionKernelSource), ${BLOOM_MOTION_KERNEL_SIZE}};\n}\n\n} // namespace bloom::runtime::detail\n"
    @ONLY)
target_sources(bloom_runtime_evaluator
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_kernel_source.cpp")

# #527: the dither kernel, the same way.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/dither_kernel.glsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dither_kernel.glsl" BLOOM_DITHER_KERNEL_HEX HEX)
string(LENGTH "${BLOOM_DITHER_KERNEL_HEX}" BLOOM_DITHER_KERNEL_HEX_LENGTH)
math(EXPR BLOOM_DITHER_KERNEL_SIZE "${BLOOM_DITHER_KERNEL_HEX_LENGTH} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BLOOM_DITHER_KERNEL_BYTES
    "${BLOOM_DITHER_KERNEL_HEX}")
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/dither_kernel_source.cpp"
    CONTENT "#include <string_view>\n\nnamespace bloom::runtime::detail {\nnamespace {\nconstexpr unsigned char kDitherKernelSource[] = {${BLOOM_DITHER_KERNEL_BYTES}0x00};\n} // namespace\n\nstd::string_view ditherKernelGlsl() noexcept;\nstd::string_view ditherKernelGlsl() noexcept {\n    return {reinterpret_cast<const char*>(kDitherKernelSource), ${BLOOM_DITHER_KERNEL_SIZE}};\n}\n\n} // namespace bloom::runtime::detail\n"
    @ONLY)
target_sources(bloom_runtime_evaluator
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/dither_kernel_source.cpp")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

add_library(bloom_runtime_video_asset STATIC video_asset.cpp)
target_include_directories(bloom_runtime_video_asset PUBLIC include)
target_link_libraries(bloom_runtime_video_asset PUBLIC bloom_document bloom_media_provider)
bloom_enable_warnings(bloom_runtime_video_asset)
target_link_libraries(bloom_runtime_evaluator PUBLIC bloom_runtime_video_asset)

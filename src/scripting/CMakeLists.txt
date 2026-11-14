# CPython is an optional host prerequisite. Explicit OFF avoids discovery; an explicit ON
# requires headers and embedding support instead of silently dropping the feature.
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_UNVERSIONED_NAMES FIRST)
if(NOT DEFINED BLOOM_BUILD_PYTHON)
    find_package(Python3 3.10 QUIET COMPONENTS Interpreter Development.Module Development.Embed)
    option(BLOOM_BUILD_PYTHON "Build the Python bridge and embedded interpreter" ${Python3_FOUND})
endif()
if(BLOOM_BUILD_PYTHON)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Module Development.Embed)
    if(Python3_SOABI MATCHES "[0-9]t")
        message(FATAL_ERROR "Bloom Python v1 requires a GIL-enabled CPython interpreter")
    endif()
endif()

add_library(bloom_scripting STATIC)

target_sources(
    bloom_scripting
    PRIVATE
        agent_guide.cpp
        command_diagnostics.cpp
        export.cpp
        export_headless.cpp
        json_script.cpp
        live_session_host.cpp
        merge_entry_view.cpp
        node_path_query.cpp
        node_reference.cpp
        operation_docs.cpp
        operation_docs.hpp
        operation_reference.cpp
        operation_registry.cpp
        operation_registry_alignment.cpp
        operation_registry_evaluated.cpp
        operation_registry_export.cpp
        operation_frame_times.cpp
        operation_references.cpp
        operation_references.hpp
        operation_registry_parity_entries.cpp
        operation_registry_parity_layers.cpp
        operation_registry_parity_project.cpp
        operation_registry_footage.cpp
        operation_values.hpp
        reference_commands.cpp
        reference_internal.hpp
        reference_session_tools.cpp
        reference_surfaces.cpp
        render.cpp
        session.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/scripting/command_diagnostics.hpp
            include/bloom/scripting/events.hpp
            include/bloom/scripting/export.hpp
            include/bloom/scripting/facade.hpp
            include/bloom/scripting/json_script.hpp
            include/bloom/scripting/live_session_host.hpp
            include/bloom/scripting/merge_entry_view.hpp
            include/bloom/scripting/node_path_query.hpp
            include/bloom/scripting/node_reference.hpp
            include/bloom/scripting/operation_reference.hpp
            include/bloom/scripting/operation_registry.hpp
            include/bloom/scripting/query.hpp
            include/bloom/scripting/readings.hpp
            include/bloom/scripting/render.hpp
            include/bloom/scripting/session.hpp
            include/bloom/scripting/tasks.hpp
            include/bloom/scripting/value.hpp
)

target_compile_features(bloom_scripting PUBLIC cxx_std_20)
target_link_libraries(
    bloom_scripting
    PUBLIC bloom_commands bloom_core bloom_document bloom_host bloom_runtime
    PRIVATE bloom_platform_process
)
bloom_enable_warnings(bloom_scripting)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_scripting_live_session_test tests/live_session_tests.cpp)
    target_link_libraries(bloom_scripting_live_session_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_live_session_test)
    bloom_add_test(NAME bloom.scripting.live-session COMMAND bloom_scripting_live_session_test
        LABELS unit scripting host commands document)
    # ADR 0034: the shared live session marshal (FIFO, bound, gestures, cancellation, revocation).
    add_executable(bloom_scripting_live_session_host_test tests/live_session_host_tests.cpp)
    target_link_libraries(bloom_scripting_live_session_host_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_live_session_host_test)
    bloom_add_test(NAME bloom.scripting.live-session-host
        COMMAND bloom_scripting_live_session_host_test
        LABELS unit scripting commands mcp TIMEOUT 120)
    add_executable(bloom_scripting_mcp_fold_test tests/mcp_fold_tests.cpp)
    target_link_libraries(bloom_scripting_mcp_fold_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_mcp_fold_test)
    bloom_add_test(
        NAME bloom.scripting.mcp-fold
        COMMAND bloom_scripting_mcp_fold_test
        LABELS unit scripting commands document
    )
    add_executable(bloom_scripting_svg_import_modes_test tests/svg_import_modes_tests.cpp)
    target_link_libraries(bloom_scripting_svg_import_modes_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_svg_import_modes_test)
    bloom_add_test(
        NAME bloom.scripting.svg-import-modes
        COMMAND bloom_scripting_svg_import_modes_test
        LABELS unit scripting commands document
    )
    add_executable(bloom_scripting_operation_schema_test tests/operation_schema_tests.cpp)
    target_link_libraries(bloom_scripting_operation_schema_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_operation_schema_test)
    bloom_add_test(
        NAME bloom.scripting.operation-schemas
        COMMAND bloom_scripting_operation_schema_test
        LABELS unit scripting commands document
    )
    # MCP parity operations: every UI edit that had no op runs through the registry, makes one
    # undo step and undoes/redoes.
    # #389: asset reload, anchor presets and Cryptomatte picks read media or evaluate when created.
    # Imported targets are directory-scoped: find OpenEXR here instead of relying on configure order.
    bloom_find_dependency(OpenEXR)
    add_executable(bloom_scripting_evaluated_operations_test tests/evaluated_operations_tests.cpp)
    target_include_directories(bloom_scripting_evaluated_operations_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/runtime/tests" "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_scripting_evaluated_operations_test PRIVATE bloom_scripting
        OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_scripting_evaluated_operations_test)
    bloom_add_test(
        NAME bloom.scripting.evaluated-operations
        COMMAND bloom_scripting_evaluated_operations_test
        LABELS unit scripting commands document media TIMEOUT 120
    )
    add_executable(bloom_scripting_parity_operations_test tests/parity_operations_tests.cpp)
    target_link_libraries(bloom_scripting_parity_operations_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_parity_operations_test)
    bloom_add_test(
        NAME bloom.scripting.parity-operations
        COMMAND bloom_scripting_parity_operations_test
        LABELS unit scripting commands document
    )
    # The agent reference: every operation, tool and argument described, one text everywhere.
    add_executable(bloom_scripting_operation_reference_test tests/operation_reference_tests.cpp)
    target_link_libraries(bloom_scripting_operation_reference_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_operation_reference_test)
    bloom_add_test(
        NAME bloom.scripting.operation-reference
        COMMAND bloom_scripting_operation_reference_test
        LABELS unit scripting mcp
    )
    add_executable(bloom_scripting_layer_alignment_test tests/layer_alignment_tests.cpp)
    target_link_libraries(bloom_scripting_layer_alignment_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_layer_alignment_test)
    bloom_add_test(
        NAME bloom.scripting.layer-alignment
        COMMAND bloom_scripting_layer_alignment_test
        LABELS unit scripting host commands runtime
        TIMEOUT 120
    )
    # ADR 0033 "One runner": Render::run through host::ExportJobRunner writes the same files and
    # bytes as the previous direct attempt/publication route, kept verbatim in the test.
    add_executable(bloom_scripting_render_parity_test tests/render_parity_tests.cpp)
    target_include_directories(bloom_scripting_render_parity_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_scripting_render_parity_test PRIVATE bloom_scripting)
    add_dependencies(bloom_scripting_render_parity_test bloom-media-worker)
    bloom_enable_warnings(bloom_scripting_render_parity_test)
    bloom_add_test(
        NAME bloom.scripting.render-parity
        COMMAND bloom_scripting_render_parity_test
        LABELS unit scripting host output
        TIMEOUT 300
    )
    # #718: a render through the local source mirror. It spans scripting and the media cache, so
    # its source is under tests/integration.
    add_executable(bloom_scripting_render_source_mirror_test
                   "${PROJECT_SOURCE_DIR}/tests/integration/render_source_mirror_tests.cpp")
    target_include_directories(bloom_scripting_render_source_mirror_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_scripting_render_source_mirror_test PRIVATE bloom_scripting
                                                                            bloom_media_cache)
    bloom_enable_warnings(bloom_scripting_render_source_mirror_test)
    bloom_add_test(
        NAME bloom.scripting.render-source-mirror
        COMMAND bloom_scripting_render_source_mirror_test
        LABELS unit scripting host output media
        TIMEOUT 300
    )
    # Project color settings: an external or project-relative OCIO configuration is selected, read
    # back, cleared and undone through the Session (#680). The helper reads it, so this runs on every
    # target that has one.
    bloom_find_dependency(OpenColorIO)
    add_executable(bloom_scripting_project_color_config_file_test
                   tests/project_color_config_file_tests.cpp)
    target_link_libraries(bloom_scripting_project_color_config_file_test
                          PRIVATE bloom_scripting OpenColorIO::OpenColorIO)
    bloom_enable_warnings(bloom_scripting_project_color_config_file_test)
    bloom_add_test(
        NAME bloom.scripting.project-color-config-file
        COMMAND bloom_scripting_project_color_config_file_test
        LABELS unit scripting host color
        TIMEOUT 300
    )
    # #465: PNG, TIFF, flat EXR and ProRes with an external loose config, an external archive and
    # a project-relative archive, resolved through the supervised helper, against the built-in.
    # Built on Linux only: the comparison renders through the media worker and the packaged
    # GPU tools, which the other targets' test setup has not been wired for. The config request
    # group itself is checked on every target by bloom.color.ocio-config-resolution and
    # bloom.scripting.project-color-config-file.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    bloom_find_dependency(OpenColorIO)
    add_executable(bloom_scripting_external_config_render_test
                   tests/external_config_render_tests.cpp)
    target_include_directories(bloom_scripting_external_config_render_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_scripting_external_config_render_test
                          PRIVATE bloom_scripting OpenColorIO::OpenColorIO)
    add_dependencies(bloom_scripting_external_config_render_test bloom-media-worker)
    bloom_enable_warnings(bloom_scripting_external_config_render_test)
    bloom_add_test(
        NAME bloom.scripting.external-config-render
        COMMAND bloom_scripting_external_config_render_test
        LABELS unit scripting host output color
        TIMEOUT 300
    )
    set_tests_properties(bloom.scripting.external-config-render PROPERTIES SKIP_RETURN_CODE 77)
    # The same renders on a real device: the external config's display transform runs on the GPU
    # from the program the helper reflected, with no CPU reference frame. Skips (77) without a
    # device or the packaged tools.
    include(BloomGpuShaderTools)
    bloom_package_gpu_shader_tools(bloom_scripting_external_config_render_test)
    set(_bloom_external_config_test_environment)
    if(UNIX
       AND NOT APPLE
       AND BLOOM_RENDER_ENABLE_VULKAN
       AND DEFINED BLOOM_DEPENDENCY_MODE
       AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        find_package(VulkanLoader QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
        if(TARGET Vulkan::Loader)
            list(APPEND _bloom_external_config_test_environment
                 "BLOOM_TEST_VULKAN_LOADER=$<TARGET_FILE:Vulkan::Loader>")
        endif()
    endif()
    bloom_add_test(
        NAME bloom.scripting.external-config-render-gpu
        COMMAND bloom_scripting_external_config_render_test --device
        LABELS unit scripting host output color gpu
        ENVIRONMENT ${_bloom_external_config_test_environment}
        TIMEOUT 300
    )
    set_tests_properties(bloom.scripting.external-config-render-gpu PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    # #588: scratch-folder cleanup and the changed-media failure of Render::run.
    add_executable(bloom_scripting_render_cleanup_test tests/render_cleanup_tests.cpp)
    target_include_directories(bloom_scripting_render_cleanup_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_scripting_render_cleanup_test PRIVATE bloom_scripting)
    add_dependencies(bloom_scripting_render_cleanup_test bloom-media-worker)
    bloom_enable_warnings(bloom_scripting_render_cleanup_test)
    bloom_add_test(
        NAME bloom.scripting.render-cleanup
        COMMAND bloom_scripting_render_cleanup_test
        LABELS unit scripting host output
        TIMEOUT 300
    )
    add_executable(bloom_scripting_facade_test tests/facade_tests.cpp)
    target_include_directories(bloom_scripting_facade_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_scripting_facade_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_facade_test)
    bloom_add_test(
        NAME bloom.scripting.facade
        COMMAND bloom_scripting_facade_test
        LABELS unit scripting host commands document
    )
    # Render::run's injected-provider contract plus the native headless and sequence-range render
    # integration: an owner-provided GpuExportProvider is reused across renders and never retired by
    # the call, while a locally created one is retired with proof. ZLIB backs the test's own
    # from-scratch PNG reader, so the scripted routes independently decode the published PNG payloads
    # and prove the genuine GPU output-colour (two-payload) arm. The production packaged GPU shader
    # tools are staged beside the executable exactly as the desktop/CLI/MCP roots stage them, so the
    # PNG output-colour command is prepared through the real packaged resolver -- never PATH.
    include(BloomGpuShaderTools)
    bloom_find_dependency(ZLIB)
    # The helper roots the qualified zlib search at the prefix (and caches ZLIB_LIBRARY), but its
    # find_dependency() form does not guarantee the namespaced target exists in this directory
    # scope; mirror the host targets' own explicit guard so ZLIB::ZLIB is always defined here.
    if(NOT TARGET ZLIB::ZLIB)
        find_package(ZLIB REQUIRED)
    endif()
    add_executable(bloom_scripting_render_injected_provider_test
                   tests/render_injected_provider_test.cpp)
    target_link_libraries(bloom_scripting_render_injected_provider_test
                          PRIVATE bloom_scripting ZLIB::ZLIB)
    target_include_directories(bloom_scripting_render_injected_provider_test PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/support"
        "${PROJECT_SOURCE_DIR}/src/runtime/include" "${PROJECT_SOURCE_DIR}/src/core/include"
        "${PROJECT_SOURCE_DIR}/src/document/include" "${PROJECT_SOURCE_DIR}/src/render/include")
    bloom_package_gpu_shader_tools(bloom_scripting_render_injected_provider_test)
    bloom_enable_warnings(bloom_scripting_render_injected_provider_test)
    # CTest runs the actual GPU case when the qualified prefix provides a loader (the test skips
    # honestly when no real device is present, so the CPU stub stays tested on GPU-less hosts).
    set(_bloom_render_injected_test_environment)
    if(UNIX
       AND NOT APPLE
       AND BLOOM_RENDER_ENABLE_VULKAN
       AND DEFINED BLOOM_DEPENDENCY_MODE
       AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        find_package(VulkanLoader QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
        if(TARGET Vulkan::Loader)
            list(APPEND _bloom_render_injected_test_environment
                 "BLOOM_TEST_VULKAN_LOADER=$<TARGET_FILE:Vulkan::Loader>")
        endif()
    elseif((APPLE OR WIN32) AND BLOOM_RENDER_ENABLE_VULKAN)
        # Windows: the locked loader in the prefix, with the system's own driver.
        # The locked loader in the qualified prefix; the driver manifest that sits with it is found
        # from its path (src/render/vulkan/gpu_packaged_runtime.hpp).
        bloom_prefix_vulkan_loader(_bloom_scripting_prefix_loader)
        if(_bloom_scripting_prefix_loader)
            list(APPEND _bloom_render_injected_test_environment
                 "BLOOM_TEST_VULKAN_LOADER=${_bloom_scripting_prefix_loader}")
        endif()
    endif()
    bloom_add_test(
        NAME bloom.scripting.render-injected-provider
        COMMAND bloom_scripting_render_injected_provider_test
        LABELS unit scripting host rendering
        ENVIRONMENT ${_bloom_render_injected_test_environment}
        TIMEOUT 180
    )
    # The headless export queue (bloom-cli export, scripted export.* calls) renders a ProRes output
    # with its display/view transform on the GPU: provenance has GPU frames and GPU display
    # transforms only. Skips (77) without a device or the packaged tools.
    add_dependencies(bloom_scripting_render_injected_provider_test bloom-media-worker)
    bloom_add_test(
        NAME bloom.scripting.headless-export-queue-gpu-display
        COMMAND bloom_scripting_render_injected_provider_test --route queue
        LABELS unit scripting host gpu
        ENVIRONMENT ${_bloom_render_injected_test_environment}
        TIMEOUT 300
    )
    set_tests_properties(bloom.scripting.headless-export-queue-gpu-display PROPERTIES
        SKIP_RETURN_CODE 77)

    # Scripted export route-proof writers: the same production Render::run facade, opt-in
    # --route-proof-dir, publishing only after every parity assertion passed. The sequence/range
    # route exercises the real FrameRangeRunner image-sequence mode; the headless route the
    # single-frame mode. Both share the run nonce written by the gpu_route_proofs fixture.
    set(BLOOM_GPU_ROUTE_PROOF_DIR "${PROJECT_BINARY_DIR}/gpu-route-proofs")
    bloom_add_test(
        NAME bloom.scripting.export-route-proof.headless
        COMMAND bloom_scripting_render_injected_provider_test
                --route-proof-dir "${BLOOM_GPU_ROUTE_PROOF_DIR}" --route headless
        LABELS unit scripting host gpu contract
        ENVIRONMENT ${_bloom_render_injected_test_environment}
        TIMEOUT 300
    )
    set_tests_properties(bloom.scripting.export-route-proof.headless PROPERTIES
        SKIP_RETURN_CODE 77
        FIXTURES_REQUIRED gpu_route_proofs
        FIXTURES_SETUP gpu_route_proof_writers)

    bloom_add_test(
        NAME bloom.scripting.export-route-proof.sequence-range
        COMMAND bloom_scripting_render_injected_provider_test
                --route-proof-dir "${BLOOM_GPU_ROUTE_PROOF_DIR}" --route sequence
        LABELS unit scripting host gpu contract
        ENVIRONMENT ${_bloom_render_injected_test_environment}
        TIMEOUT 300
    )
    set_tests_properties(bloom.scripting.export-route-proof.sequence-range PROPERTIES
        SKIP_RETURN_CODE 77
        FIXTURES_REQUIRED gpu_route_proofs
        FIXTURES_SETUP gpu_route_proof_writers)
endif()

if(BLOOM_BUILD_PYTHON)
    add_subdirectory(python)
endif()

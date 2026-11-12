add_executable(bloom_editor_area_chrome_test
    editor_area_chrome_tests.cpp
)
add_executable(bloom_afterlab_baseline_test
    afterlab_baseline_tests.cpp
)
add_executable(bloom_workspace_presets_test
    workspace_presets_tests.cpp
)
add_executable(bloom_workspace_panel_test
    workspace_panel_tests.cpp
)
add_executable(bloom_kit_button_test
    kit_button_tests.cpp
)
add_executable(bloom_kit_choice_test
    kit_choice_tests.cpp
)
add_executable(bloom_kit_color_test
    kit_color_tests.cpp
)
add_executable(bloom_kit_color_chip_test
    kit_color_chip_tests.cpp
)
add_executable(bloom_kit_color_picker_test
    kit_color_picker_tests.cpp
)
add_executable(bloom_kit_color_sampler_test
    kit_color_sampler_tests.cpp
)
add_executable(bloom_kit_color_swatches_test
    kit_color_swatches_tests.cpp
)
add_executable(bloom_kit_dropdown_test
    kit_dropdown_tests.cpp
)
add_executable(bloom_kit_popup_test
    kit_popup_tests.cpp
)
add_executable(bloom_kit_focus_border_test
    kit_focus_border_tests.cpp
)
add_executable(bloom_kit_fonts_test
    kit_fonts_tests.cpp
)
add_executable(bloom_kit_panel_switcher_test
    kit_panel_switcher_tests.cpp
)
add_executable(bloom_kit_icons_test
    kit_icons_tests.cpp
)
add_executable(bloom_kit_title_bar_test
    kit_title_bar_tests.cpp
)
add_executable(bloom_kit_mnemonic_style_test
    kit_mnemonic_style_tests.cpp
)
add_executable(bloom_third_party_license_catalog_test
    third_party_license_catalog_tests.cpp
)
add_executable(bloom_main_window_chrome_test
    main_window_chrome_tests.cpp
)
add_executable(bloom_frameless_window_support_test
    frameless_window_support_tests.cpp
)
add_executable(bloom_kit_range_test
    kit_range_tests.cpp
)
add_executable(bloom_kit_interactive_controls_test
    kit_interactive_controls_tests.cpp
)
add_executable(bloom_kit_value_field_test
    kit_value_field_tests.cpp
)
add_executable(bloom_kit_value_field_input_test
    kit_value_field_input_tests.cpp
)
add_executable(bloom_kit_edit_session_test
    kit_edit_session_tests.cpp
)
add_executable(bloom_kit_theme_test
    kit_theme_tests.cpp
)
add_executable(bloom_kit_tokens_test
    kit_tokens_tests.cpp
)
add_executable(bloom_kit_surfaces_test
    kit_surfaces_tests.cpp
)
add_executable(bloom_composition_projection_test
    composition_projection_test.cpp
)
add_executable(bloom_properties_editor_test
    properties_editor_tests.cpp
)
add_executable(bloom_composition_preview_controller_test
    composition_preview_controller_tests.cpp
)
add_executable(bloom_composition_preview_cpu_stage_test
    composition_preview_cpu_stage_tests.cpp
)
add_executable(bloom_composition_preview_gpu_scene_stage_test
    composition_preview_gpu_scene_stage_tests.cpp
)
add_executable(bloom_gpu_preview_submitter_test
    gpu_preview_submitter_tests.cpp
)
add_executable(bloom_gpu_preview_app_test
    gpu_preview_app_tests.cpp
)
# bloom_ui_gpu_presentation_target_test adopts the borrowed VkInstance through Qt's public
# QVulkanInstance, so it is created in the capability-gated block below (BLOOM_UI_VULKAN_AVAILABLE
# is the conjunction of the pinned Vulkan headers and a Qt build with the Vulkan feature).
# Independent QPainter oracle for the present-image shader's filtering/channel rules. Qt-only, links
# Qt6::Gui (the app supplies Qt6::Widgets); it never mirrors the shader algebra.
add_executable(bloom_ui_gpu_present_image_qpainter_oracle_test
    gpu_present_image_qpainter_oracle_tests.cpp
)
# Native offscreen proof for the P2/P3 presentation sampling path. Qt supplies the independent
# QPainter oracle; the production render draw is compiled against the render module's private
# Vulkan headers, so the target is created only where that private seam is available (below).
add_executable(bloom_composition_session_animation_test
    composition_session_animation_tests.cpp
)
add_executable(bloom_composition_session_rebind_test
    composition_session_rebind_tests.cpp
)
add_executable(bloom_application_shutdown_test
    application_shutdown_tests.cpp
)
add_executable(bloom_project_host_test
    project_host_tests.cpp
)
add_executable(bloom_task_monitor_test
    task_monitor_test.cpp
)
add_executable(bloom_memory_budget_controller_test memory_budget_controller_tests.cpp)
target_link_libraries(bloom_memory_budget_controller_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_memory_budget_controller_test)
add_executable(bloom_task_completion_bridge_test task_completion_bridge_tests.cpp)
target_link_libraries(bloom_task_completion_bridge_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_task_completion_bridge_test)
add_executable(bloom_viewer_editor_test
    viewer_editor_tests.cpp
)
add_executable(bloom_timeline_frame_math_test
    timeline_frame_math_tests.cpp
)
add_executable(bloom_timeline_ruler_test
    timeline_ruler_tests.cpp
)
add_executable(bloom_timeline_graph_math_test
    timeline_graph_math_tests.cpp
)
add_executable(bloom_timeline_graph_view_test
    timeline_graph_view_tests.cpp
)
add_executable(bloom_composition_session_position_interaction_test
    composition_session_position_interaction_tests.cpp
)
add_executable(bloom_gesture_finiteness_test
    gesture_finiteness_tests.cpp
)
add_executable(bloom_direct_manipulation_test
    direct_manipulation_tests.cpp
)

# QSignalSpy and QTest::keyClick -- issue #105's real key-event dispatch through Qt's own shortcut
# machinery, and issue #117's header-chrome signal assertions -- need Qt6::Test. Scoped to the two
# executables that use it rather than to bloom_ui itself.
if(NOT TARGET Qt6::Test)
    find_package(Qt6 ${BLOOM_QT_VERSION_REQUEST} REQUIRED COMPONENTS Test)
endif()
add_executable(bloom_playback_controller_test
    playback_controller_tests.cpp
)
add_executable(bloom_playback_preroll_test
    playback_preroll_tests.cpp
)
add_executable(bloom_playback_wait_test
    playback_wait_tests.cpp
)

# Links ZLIB::ZLIB directly for its own trimmed hand-rolled ZIP-writer fixture (a preserved-
# read-only archive builder) -- mirroring src/host/CMakeLists.txt's bloom_host_session_open_test,
# which documents the same src-module tests/-directory boundary this duplicate exists to respect.
if(NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB REQUIRED)
endif()
add_executable(bloom_main_window_readonly_placeholder_test
    main_window_readonly_placeholder_tests.cpp
)

# Task U7 (issue #122): TimelineEditor track-row/transport Afterlab restyle tests, appended rather
# than folded into timeline_ruler_tests.cpp (which owns the ruler/keyframe-panel interaction
# contract this task must keep byte-equivalent).
add_executable(bloom_timeline_editor_style_test
    timeline_editor_style_tests.cpp
)

# Task U4 (issue #123): the node editor's navigation, selection, typed connector, context menu and
# in-node editing contracts. Its own executable rather than more cases inside
# composition_projection_test.cpp, which owns the cross-editor projection/selection contract this
# task keeps byte-equivalent.
add_executable(bloom_node_editor_test
    node_editor_tests.cpp
)

target_link_libraries(bloom_editor_area_chrome_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_afterlab_baseline_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_workspace_presets_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_workspace_panel_test PRIVATE bloom_ui)
target_link_libraries(bloom_kit_button_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_choice_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_color_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_color_chip_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_color_picker_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_color_sampler_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_color_swatches_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_dropdown_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_popup_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_focus_border_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_fonts_test PRIVATE bloom_ui_kit)
# The manifest check also reads the vendored files that are not interface resources.
target_compile_definitions(bloom_kit_fonts_test PRIVATE
    BLOOM_KIT_THIRD_PARTY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../kit/third_party")
target_link_libraries(bloom_kit_panel_switcher_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_icons_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_title_bar_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_mnemonic_style_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_third_party_license_catalog_test PRIVATE bloom_ui)
target_link_libraries(bloom_main_window_chrome_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_frameless_window_support_test PRIVATE bloom_ui)
target_link_libraries(bloom_kit_range_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_interactive_controls_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_value_field_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_value_field_input_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_edit_session_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_theme_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_tokens_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_surfaces_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_composition_projection_test PRIVATE bloom_ui)
target_link_libraries(bloom_properties_editor_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_preview_controller_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_preview_cpu_stage_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_preview_gpu_scene_stage_test PRIVATE bloom_ui)
target_include_directories(bloom_composition_preview_gpu_scene_stage_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/tests" "${PROJECT_SOURCE_DIR}/src/runtime")
# Production general-display stage path. It drives the ACTUAL makeSessionRefreshingGpuSceneStage
# with the shared runtime GpuOcioContextResolver over the REAL packaged tools staged beside this
# executable by bloom_package_gpu_shader_tools (no injected tool paths or preparer); without the
# tools the general assertions are compiled out and the test is a clean pass.
add_executable(bloom_composition_preview_gpu_scene_general_test
    composition_preview_gpu_scene_general_tests.cpp)
target_include_directories(bloom_composition_preview_gpu_scene_general_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_composition_preview_gpu_scene_general_test PRIVATE bloom_ui)
target_compile_definitions(bloom_composition_preview_gpu_scene_general_test PRIVATE
    "BLOOM_GPU_GENERAL_TEST_EXECUTABLE=\"$<TARGET_FILE:bloom_composition_preview_gpu_scene_general_test>\"")
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom_composition_preview_gpu_scene_general_test)
bloom_enable_warnings(bloom_composition_preview_gpu_scene_general_test)
bloom_add_test(
    NAME bloom.ui.composition_preview_gpu_scene_general
    COMMAND bloom_composition_preview_gpu_scene_general_test
    LABELS ui gpu color
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
target_link_libraries(bloom_gpu_preview_submitter_test PRIVATE bloom_ui)
target_link_libraries(bloom_gpu_preview_app_test PRIVATE bloom_ui bloom_runtime_evaluator)
# Desktop composition-root regression: the shared host packaged-tool helper supplies the expected
# staged digests and qualifies the REAL tools staged beside this executable, so the default general
# Neutral DisplayRgba8 program is available at 6000x4000. Mirrors apps/bloom/main.cpp exactly; no
# device and no qualification bypass.
add_executable(bloom_ui_gpu_preview_app_packaged_tools_test
    gpu_preview_app_packaged_tools_tests.cpp)
target_include_directories(bloom_ui_gpu_preview_app_packaged_tools_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_ui_gpu_preview_app_packaged_tools_test
    PRIVATE bloom_runtime_evaluator bloom_host)
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom_ui_gpu_preview_app_packaged_tools_test)
bloom_enable_warnings(bloom_ui_gpu_preview_app_packaged_tools_test)
bloom_add_test(
    NAME bloom.ui.gpu_preview_app_packaged_tools
    COMMAND bloom_ui_gpu_preview_app_packaged_tools_test
    LABELS ui gpu color
    TIMEOUT 120
)
set_tests_properties(bloom.ui.gpu_preview_app_packaged_tools
    PROPERTIES SKIP_RETURN_CODE 77)
# bloom_ui_gpu_presentation_target_test adopts the borrowed VkInstance through Qt's public
# QVulkanInstance, so it exists only where this build has both the pinned Vulkan headers and a Qt
# build that shipped the Vulkan feature (BLOOM_UI_VULKAN_AVAILABLE, src/ui/CMakeLists.txt). On a Qt
# without that feature -- the official Qt 6.8 macOS packages -- the target is not created and the
# Vulkan-free UI tests still build and run.
if(BLOOM_UI_VULKAN_AVAILABLE)
    add_executable(bloom_ui_gpu_presentation_target_test
        gpu_presentation_target_tests.cpp
    )
    target_link_libraries(bloom_ui_gpu_presentation_target_test
        PRIVATE bloom_ui bloom_render Vulkan::Headers)
    # QVulkanInstance's public class requires <vulkan/vulkan.h> on the application include path; the
    # pinned dependency prefix supplies it. Only the tests need this; the UI and render modules stay
    # Vulkan-header-free in their public interfaces.
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_ui_gpu_presentation_target_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_ui_gpu_presentation_target_test)
endif()
# The QPainter oracle is Qt-only (Qt6::Gui); it never links render. It uses std::source_location,
# so it needs the project's C++20 level explicitly (Qt's default here is C++17).
target_link_libraries(bloom_ui_gpu_present_image_qpainter_oracle_test PRIVATE Qt6::Gui)
target_compile_features(bloom_ui_gpu_present_image_qpainter_oracle_test PRIVATE cxx_std_20)
# The injected-construction-fault seam lives in bloom_render's private Vulkan sources. A sibling
# directory's BLOOM_RENDER_VULKAN_AVAILABLE is not visible here, so repeat the same mode-aware probe
# before compiling the private-header fault case and adding the private include root. This seam is
# independent of Qt's Vulkan feature: the offscreen proof below drives the render module directly
# and never includes QVulkanInstance, so it stays available wherever the render Vulkan backend is.
set(BLOOM_UI_PRESENTATION_TARGET_PRIVATE_SEAM OFF)
if(BLOOM_RENDER_ENABLE_VULKAN AND DEFINED BLOOM_DEPENDENCY_MODE)
    if(BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        find_package(VulkanHeaders QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
        find_package(VulkanMemoryAllocator QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    elseif(BLOOM_DEPENDENCY_MODE STREQUAL "developer-system")
        find_package(VulkanHeaders QUIET CONFIG)
        find_package(VulkanMemoryAllocator QUIET CONFIG)
    endif()
    if(TARGET Vulkan::Headers AND TARGET GPUOpen::VulkanMemoryAllocator)
        set(BLOOM_UI_PRESENTATION_TARGET_PRIVATE_SEAM ON)
    endif()
endif()
if(BLOOM_UI_PRESENTATION_TARGET_PRIVATE_SEAM)
    if(TARGET bloom_ui_gpu_presentation_target_test)
        target_compile_definitions(bloom_ui_gpu_presentation_target_test
            PRIVATE BLOOM_UI_GPU_PRESENTATION_TARGET_PRIVATE_SEAM=1)
        target_include_directories(bloom_ui_gpu_presentation_target_test
            PRIVATE "${PROJECT_SOURCE_DIR}/src/render/vulkan")
    endif()
    # The offscreen native proof includes the render module's private Vulkan headers and needs the
    # same prefix-provided Vulkan-Hpp/VMA headers. It links bloom_render + bloom_color_ocio (the
    # latter only for the independent CPU OCIO oracle) and Qt6::Gui (the QPainter oracle).
    add_executable(bloom_ui_gpu_present_image_offscreen_test
        gpu_present_image_offscreen_tests.cpp
    )
    target_link_libraries(bloom_ui_gpu_present_image_offscreen_test
        PRIVATE bloom_ui bloom_render bloom_color_ocio Qt6::Gui)
    target_include_directories(bloom_ui_gpu_present_image_offscreen_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render/vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_ui_gpu_present_image_offscreen_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    target_compile_definitions(bloom_ui_gpu_present_image_offscreen_test
        PRIVATE BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
    bloom_enable_warnings(bloom_ui_gpu_present_image_offscreen_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_present_image_offscreen_test)
endif()
target_link_libraries(bloom_composition_session_animation_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_session_rebind_test PRIVATE bloom_ui)
target_link_libraries(bloom_application_shutdown_test PRIVATE bloom_ui)
target_link_libraries(bloom_project_host_test PRIVATE bloom_ui ZLIB::ZLIB)
# The ADR 0032 open-notice test assembles an older-version archive with the project test helper.
target_include_directories(bloom_project_host_test PRIVATE "${PROJECT_SOURCE_DIR}/src/project/tests")
target_link_libraries(bloom_task_monitor_test PRIVATE bloom_ui)
target_link_libraries(bloom_viewer_editor_test PRIVATE bloom_ui Qt6::Test)
target_include_directories(bloom_viewer_editor_test PRIVATE ..)
target_link_libraries(bloom_timeline_frame_math_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_ruler_test PRIVATE bloom_ui)
target_include_directories(bloom_timeline_ruler_test PRIVATE ..)
target_link_libraries(bloom_timeline_graph_math_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_graph_view_test PRIVATE bloom_ui)
# The graph editor's arithmetic is a PRIVATE header of src/ui (it has no public consumers), so
# these two tests reach it through the module's own source root rather than a relative path that
# would bypass the include-root boundary.
target_include_directories(bloom_timeline_graph_math_test PRIVATE ..)
target_include_directories(bloom_timeline_graph_view_test PRIVATE ..)
target_link_libraries(bloom_composition_session_position_interaction_test PRIVATE bloom_ui)
target_link_libraries(bloom_gesture_finiteness_test PRIVATE bloom_ui)
target_link_libraries(bloom_direct_manipulation_test PRIVATE bloom_ui)
target_link_libraries(bloom_main_window_readonly_placeholder_test PRIVATE bloom_ui ZLIB::ZLIB)
target_link_libraries(bloom_playback_controller_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_playback_preroll_test PRIVATE bloom_ui)
target_link_libraries(bloom_playback_wait_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_editor_style_test PRIVATE bloom_ui Qt6::Test)
# Task DRIVE-1's upstream-group pin reads the timeline's own row vocabulary (upstreamGroupKey).
target_include_directories(bloom_timeline_editor_style_test PRIVATE ..)
target_link_libraries(bloom_node_editor_test PRIVATE bloom_ui)
target_include_directories(bloom_node_editor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
bloom_enable_warnings(bloom_editor_area_chrome_test)
bloom_enable_warnings(bloom_afterlab_baseline_test)
bloom_enable_warnings(bloom_workspace_presets_test)
bloom_enable_warnings(bloom_workspace_panel_test)
bloom_enable_warnings(bloom_kit_button_test)
bloom_enable_warnings(bloom_kit_choice_test)
bloom_enable_warnings(bloom_kit_color_test)
bloom_enable_warnings(bloom_kit_color_chip_test)
bloom_enable_warnings(bloom_kit_color_picker_test)
bloom_enable_warnings(bloom_kit_color_sampler_test)
bloom_enable_warnings(bloom_kit_color_swatches_test)
bloom_enable_warnings(bloom_kit_dropdown_test)
bloom_enable_warnings(bloom_kit_popup_test)
bloom_enable_warnings(bloom_kit_focus_border_test)
bloom_enable_warnings(bloom_kit_fonts_test)
bloom_enable_warnings(bloom_kit_panel_switcher_test)
bloom_enable_warnings(bloom_kit_icons_test)
bloom_enable_warnings(bloom_kit_title_bar_test)
bloom_enable_warnings(bloom_kit_mnemonic_style_test)
bloom_enable_warnings(bloom_third_party_license_catalog_test)
bloom_enable_warnings(bloom_main_window_chrome_test)
bloom_enable_warnings(bloom_frameless_window_support_test)
bloom_enable_warnings(bloom_kit_range_test)
bloom_enable_warnings(bloom_kit_interactive_controls_test)
bloom_enable_warnings(bloom_kit_value_field_test)
bloom_enable_warnings(bloom_kit_value_field_input_test)
bloom_enable_warnings(bloom_kit_edit_session_test)
bloom_enable_warnings(bloom_kit_theme_test)
bloom_enable_warnings(bloom_kit_tokens_test)
bloom_enable_warnings(bloom_kit_surfaces_test)
bloom_enable_warnings(bloom_composition_projection_test)
bloom_enable_warnings(bloom_properties_editor_test)
bloom_enable_warnings(bloom_composition_preview_controller_test)
bloom_enable_warnings(bloom_composition_preview_cpu_stage_test)
bloom_enable_warnings(bloom_composition_preview_gpu_scene_stage_test)
# The frozen replay oracle header defines anonymous-namespace helpers this TU does not all use; the
# other consumer uses them all, so only this translation unit quiets unused-function.
bloom_allow_unused_functions(bloom_composition_preview_gpu_scene_stage_test)
bloom_enable_warnings(bloom_gpu_preview_submitter_test)
bloom_enable_warnings(bloom_gpu_preview_app_test)
bloom_enable_warnings(bloom_ui_gpu_present_image_qpainter_oracle_test)
bloom_enable_warnings(bloom_composition_session_animation_test)
bloom_enable_warnings(bloom_composition_session_rebind_test)
bloom_enable_warnings(bloom_application_shutdown_test)
bloom_enable_warnings(bloom_project_host_test)
bloom_enable_warnings(bloom_task_monitor_test)
bloom_enable_warnings(bloom_viewer_editor_test)
bloom_enable_warnings(bloom_timeline_frame_math_test)
bloom_enable_warnings(bloom_timeline_ruler_test)
bloom_enable_warnings(bloom_timeline_graph_math_test)
bloom_enable_warnings(bloom_timeline_graph_view_test)
bloom_enable_warnings(bloom_composition_session_position_interaction_test)
bloom_enable_warnings(bloom_gesture_finiteness_test)
bloom_enable_warnings(bloom_direct_manipulation_test)
bloom_enable_warnings(bloom_main_window_readonly_placeholder_test)
bloom_enable_warnings(bloom_playback_controller_test)
bloom_enable_warnings(bloom_playback_preroll_test)
bloom_enable_warnings(bloom_playback_wait_test)
bloom_enable_warnings(bloom_timeline_editor_style_test)
bloom_enable_warnings(bloom_node_editor_test)

add_executable(bloom_assets_editor_test assets_editor_tests.cpp)
target_include_directories(bloom_assets_editor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_assets_editor_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_assets_editor_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.ui.memory_budget_controller
    COMMAND bloom_memory_budget_controller_test
    LABELS ui runtime unit
    TIMEOUT 30
)
set_tests_properties(bloom.ui.memory_budget_controller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

bloom_add_test(
    NAME bloom.ui.task_completion_bridge
    COMMAND bloom_task_completion_bridge_test
    LABELS runtime unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.ui.editor_area_chrome
    COMMAND bloom_editor_area_chrome_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.afterlab_baseline
    COMMAND bloom_afterlab_baseline_test
    LABELS ui integration kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.workspace_presets
    COMMAND bloom_workspace_presets_test
    LABELS ui integration
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.workspace_panel
    COMMAND bloom_workspace_panel_test
    LABELS ui integration
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_button
    COMMAND bloom_kit_button_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_choice
    COMMAND bloom_kit_choice_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color
    COMMAND bloom_kit_color_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_chip
    COMMAND bloom_kit_color_chip_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_picker
    COMMAND bloom_kit_color_picker_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_sampler
    COMMAND bloom_kit_color_sampler_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_swatches
    COMMAND bloom_kit_color_swatches_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_dropdown
    COMMAND bloom_kit_dropdown_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_popup
    COMMAND bloom_kit_popup_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
)

bloom_add_test(
    NAME bloom.ui.kit_focus_border
    COMMAND bloom_kit_focus_border_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_fonts
    COMMAND bloom_kit_fonts_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_panel_switcher
    COMMAND bloom_kit_panel_switcher_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_icons
    COMMAND bloom_kit_icons_test
    LABELS ui unit kit
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_title_bar
    COMMAND bloom_kit_title_bar_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_mnemonic_style
    COMMAND bloom_kit_mnemonic_style_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.third_party_license_catalog
    COMMAND bloom_third_party_license_catalog_test --root "${PROJECT_SOURCE_DIR}"
    LABELS ui unit quality
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.main_window_chrome
    COMMAND bloom_main_window_chrome_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.frameless_window_support
    COMMAND bloom_frameless_window_support_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_range
    COMMAND bloom_kit_range_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_interactive_controls
    COMMAND bloom_kit_interactive_controls_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_theme
    COMMAND bloom_kit_theme_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_value_field
    COMMAND bloom_kit_value_field_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_value_field_input
    COMMAND bloom_kit_value_field_input_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_edit_session
    COMMAND bloom_kit_edit_session_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_tokens
    COMMAND bloom_kit_tokens_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_surfaces
    COMMAND bloom_kit_surfaces_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_projection
    COMMAND bloom_composition_projection_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.properties_editor
    COMMAND bloom_properties_editor_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_preview_controller
    COMMAND bloom_composition_preview_controller_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_preview_cpu_stage
    COMMAND bloom_composition_preview_cpu_stage_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU-scene preview preparation stage: the CPU-only analogue of the CPU stage. It runs through the
# real SnapshotCompiler, CpuGpuSceneBuilder, QualifiedDisplayProcessorProvider and TaskScheduler and
# checks the prepared scene against a genuine CpuCompositionEvaluator oracle; no device or loader.
bloom_add_test(
    NAME bloom.ui.composition_preview_gpu_scene_stage
    COMMAND bloom_composition_preview_gpu_scene_stage_test
    LABELS ui integration
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.gpu_preview_submitter
    COMMAND bloom_gpu_preview_submitter_test
    LABELS ui integration gpu
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.gpu_preview_app
    COMMAND bloom_gpu_preview_app_test
    LABELS ui integration gpu
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

if(TARGET bloom_ui_gpu_presentation_target_test)
    bloom_add_test(
        NAME bloom.ui.gpu_presentation_target
        COMMAND bloom_ui_gpu_presentation_target_test
        LABELS ui integration gpu
        TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

bloom_add_test(
    NAME bloom.ui.gpu_present_image_qpainter_oracle
    COMMAND bloom_ui_gpu_present_image_qpainter_oracle_test
    LABELS ui unit gpu
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

if(TARGET bloom_ui_gpu_present_image_offscreen_test)
    bloom_add_test(
        NAME bloom.ui.gpu_present_image_offscreen
        COMMAND bloom_ui_gpu_present_image_offscreen_test
        LABELS ui integration gpu
        TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

bloom_add_test(
    NAME bloom.ui.composition_session_animation
    COMMAND bloom_composition_session_animation_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_session_rebind
    COMMAND bloom_composition_session_rebind_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.application_shutdown
    COMMAND bloom_application_shutdown_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.project_host
    COMMAND bloom_project_host_test
    LABELS ui integration host
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.task_monitor
    COMMAND bloom_task_monitor_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.viewer_editor
    COMMAND bloom_viewer_editor_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_viewer_display_view_test viewer_display_view_tests.cpp)
target_link_libraries(bloom_viewer_display_view_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_viewer_display_view_test)
bloom_add_test(
    NAME bloom.ui.viewer_display_view
    COMMAND bloom_viewer_display_view_test
    LABELS ui unit color
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.timeline_frame_math
    COMMAND bloom_timeline_frame_math_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.timeline_ruler
    COMMAND bloom_timeline_ruler_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.timeline_graph_math
    COMMAND bloom_timeline_graph_math_test
    LABELS ui unit animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.timeline_graph_view
    COMMAND bloom_timeline_graph_view_test
    LABELS ui integration animation
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_session_position_interaction
    COMMAND bloom_composition_session_position_interaction_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.gesture_finiteness
    COMMAND bloom_gesture_finiteness_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.direct_manipulation
    COMMAND bloom_direct_manipulation_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.main_window_readonly_placeholder
    COMMAND bloom_main_window_readonly_placeholder_test
    LABELS ui integration host
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.playback_controller
    COMMAND bloom_playback_controller_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# #404: the playback preroll hold, its 0.5 s cap, cancel, seek restart, cached start and audio.
bloom_add_test(
    NAME bloom.ui.playback_preroll
    COMMAND bloom_playback_preroll_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# #497: the playhead waits for uncached frames without stopping; cached frames keep real time,
# the read-ahead fills in order from the playhead, and audio mutes while it waits.
bloom_add_test(
    NAME bloom.ui.playback_wait
    COMMAND bloom_playback_wait_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.timeline_editor_style
    COMMAND bloom_timeline_editor_style_test
    LABELS ui integration kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.node_editor
    COMMAND bloom_node_editor_test
    LABELS ui integration kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.assets-editor
    COMMAND bloom_assets_editor_test
    LABELS ui integration assets
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_interpret_footage_test interpret_footage_tests.cpp)
target_include_directories(bloom_interpret_footage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_interpret_footage_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_interpret_footage_test)
bloom_add_test(
    NAME bloom.ui.interpret_footage
    COMMAND bloom_interpret_footage_test
    LABELS ui integration assets
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_import_options_test import_options_tests.cpp)
target_include_directories(bloom_import_options_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_import_options_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_import_options_test)
bloom_add_test(
    NAME bloom.ui.import-options
    COMMAND bloom_import_options_test
    LABELS ui integration assets
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# #495: the background Verify footage pass and its Assets controller job.
add_executable(bloom_footage_verification_test footage_verification_tests.cpp)
target_include_directories(bloom_footage_verification_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_footage_verification_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_footage_verification_test)
bloom_add_test(
    NAME bloom.ui.footage-verification
    COMMAND bloom_footage_verification_test
    LABELS ui integration assets
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_assets_panel_test assets_panel_tests.cpp)
target_include_directories(bloom_assets_panel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_assets_panel_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_assets_panel_test)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.assets-panel.dpr${scale} COMMAND bloom_assets_panel_test)
    set_tests_properties(bloom.ui.assets-panel.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;integration;assets" TIMEOUT 60)
endforeach()

add_executable(bloom_project_host_empty_test project_host_empty_tests.cpp)
target_link_libraries(bloom_project_host_empty_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_project_host_empty_test)
bloom_add_test(
    NAME bloom.ui.project-host-empty
    COMMAND bloom_project_host_empty_test
    LABELS ui integration
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_composition_duration_test composition_duration_tests.cpp)
target_link_libraries(bloom_composition_duration_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_composition_duration_test)
bloom_add_test(
    NAME bloom.ui.composition-duration
    COMMAND bloom_composition_duration_test
    LABELS ui integration
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_ui_multi_selection_test composition_session_multi_selection_tests.cpp)
target_link_libraries(bloom_ui_multi_selection_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_multi_selection_test)
bloom_add_test(NAME bloom.ui.multi-selection COMMAND bloom_ui_multi_selection_test LABELS unit ui
               ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_interaction_test node_interaction_tests.cpp node_connection_tests.cpp node_search_keyboard_tests.cpp)
target_include_directories(bloom_node_interaction_test PRIVATE ..)
target_link_libraries(bloom_node_interaction_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_interaction_test)
bloom_add_test(NAME bloom.ui.node-interaction COMMAND bloom_node_interaction_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_layout_arrange_test node_layout_arrange_tests.cpp)
target_include_directories(bloom_node_layout_arrange_test PRIVATE ..)
target_link_libraries(bloom_node_layout_arrange_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_node_layout_arrange_test)
bloom_add_test(NAME bloom.ui.node-layout-arrange COMMAND bloom_node_layout_arrange_test LABELS ui unit TIMEOUT 30)

# Task S1: the node card's own primitives (in-card field geometry, the content-derived card
# minimum, socket identity). Its own executable rather than more cases inside
# node_interaction_tests.cpp, which owns the gesture/command contract this task keeps intact.
add_executable(bloom_node_card_test node_card_tests.cpp)
target_include_directories(bloom_node_card_test PRIVATE ..)
target_link_libraries(bloom_node_card_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_card_test)
bloom_add_test(NAME bloom.ui.node-card COMMAND bloom_node_card_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task S2 (issue #132): node group frames -- geometry, frame drag, membership by drop, inline title
# rename, and the two menus. Its own executable for the same reason node_card_tests.cpp is.
# Task N1: the node canvas -- cursor placement, overlap-free adds, the layer-stack Arrange and its
# footer button, collapse, frame colour, constant-width links, Merge's per-slot sockets, source
# cards without previews and the Nodes icon.
# #701: Delete Unused Nodes from the footer button and View > Organize, one undo step.
add_executable(bloom_node_delete_unused_test node_delete_unused_tests.cpp)
target_include_directories(bloom_node_delete_unused_test PRIVATE ..)
target_link_libraries(bloom_node_delete_unused_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_delete_unused_test)
bloom_add_test(NAME bloom.ui.node-delete-unused COMMAND bloom_node_delete_unused_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_canvas_polish_test node_canvas_polish_tests.cpp)
target_include_directories(bloom_node_canvas_polish_test PRIVATE ..)
target_link_libraries(bloom_node_canvas_polish_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_canvas_polish_test)
bloom_add_test(NAME bloom.ui.node-canvas-polish COMMAND bloom_node_canvas_polish_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    # The estimate is held against the cards the application draws. On Windows the offscreen platform
    # lays text out with another font engine than the application, whose labels come out a few pixels
    # wider, so this test runs on the platform the application runs on. It runs alone: in a session
    # without a desktop its windows cannot be created, and the attempts use up window resources the
    # session's other test programs need while it runs.
    set_tests_properties(bloom.ui.node-canvas-polish
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows" RUN_SERIAL TRUE)
endif()

# Issue #359: the Nodes canvas at scale -- per-move work independent of the node count, edits that
# allocate only the items they touch, and the skipped projection. Counts work, never time.
add_executable(bloom_node_canvas_scale_test node_canvas_scale_tests.cpp)
target_include_directories(bloom_node_canvas_scale_test PRIVATE ..)
target_link_libraries(bloom_node_canvas_scale_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_canvas_scale_test)
bloom_add_test(NAME bloom.ui.node-canvas-scale COMMAND bloom_node_canvas_scale_test LABELS ui integration kit TIMEOUT 240 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_group_test node_group_tests.cpp)
target_include_directories(bloom_node_group_test PRIVATE ..)
target_link_libraries(bloom_node_group_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_group_test)
bloom_add_test(NAME bloom.ui.node-groups COMMAND bloom_node_group_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The node canvas's lifetime contract against Qt's scene index: what a card's drag elevation has to
# announce before it shrinks, so that nothing the scene has given up or freed is left behind in the
# BSP leaves a later query or paint walks. Its own executable for the same reason node_card_tests.cpp
# is.
add_executable(bloom_node_canvas_lifetime_test node_canvas_lifetime_tests.cpp)
target_include_directories(bloom_node_canvas_lifetime_test PRIVATE ..)
target_link_libraries(bloom_node_canvas_lifetime_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_canvas_lifetime_test)
bloom_add_test(NAME bloom.ui.node-canvas-lifetime COMMAND bloom_node_canvas_lifetime_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_value_animation_test node_value_animation_tests.cpp)
target_include_directories(bloom_node_value_animation_test PRIVATE ..)
target_link_libraries(bloom_node_value_animation_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_value_animation_test)
bloom_add_test(NAME bloom.ui.node-value-animation COMMAND bloom_node_value_animation_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_path_card_key_test path_card_key_tests.cpp)
target_include_directories(bloom_path_card_key_test PRIVATE ..)
target_link_libraries(bloom_path_card_key_test PRIVATE bloom_ui bloom_commands bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_path_card_key_test)
bloom_add_test(NAME bloom.ui.path-card-key COMMAND bloom_path_card_key_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_vector_artwork_row_test vector_artwork_row_tests.cpp)
target_include_directories(bloom_vector_artwork_row_test PRIVATE ..)
target_link_libraries(bloom_vector_artwork_row_test PRIVATE bloom_ui bloom_commands bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_vector_artwork_row_test)
bloom_add_test(NAME bloom.ui.vector-artwork-row COMMAND bloom_vector_artwork_row_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_delete_key_test node_delete_key_tests.cpp)
target_include_directories(bloom_node_delete_key_test PRIVATE ..)
target_link_libraries(bloom_node_delete_key_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_delete_key_test)
bloom_add_test(NAME bloom.ui.node-delete-key COMMAND bloom_node_delete_key_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_blend_mode_test node_blend_mode_tests.cpp)
target_include_directories(bloom_node_blend_mode_test PRIVATE ..)
target_link_libraries(bloom_node_blend_mode_test PRIVATE bloom_ui bloom_runtime bloom_render bloom_color Qt6::Test)
bloom_enable_warnings(bloom_node_blend_mode_test)
bloom_add_test(NAME bloom.ui.node-blend-mode COMMAND bloom_node_blend_mode_test LABELS ui integration TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_reroute_test node_reroute_tests.cpp)
target_include_directories(bloom_node_reroute_test PRIVATE ..)
target_link_libraries(bloom_node_reroute_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_reroute_test)
bloom_add_test(NAME bloom.ui.node-reroute COMMAND bloom_node_reroute_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_opening_frame_test node_opening_frame_tests.cpp)
target_include_directories(bloom_node_opening_frame_test PRIVATE ..)
target_link_libraries(bloom_node_opening_frame_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_opening_frame_test)
bloom_add_test(NAME bloom.ui.node-opening-frame COMMAND bloom_node_opening_frame_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_placement_test node_placement_tests.cpp)
target_include_directories(bloom_node_placement_test PRIVATE ..)
target_link_libraries(bloom_node_placement_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_placement_test)
bloom_add_test(NAME bloom.ui.node-placement COMMAND bloom_node_placement_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_link_detach_test node_link_detach_tests.cpp)
target_include_directories(bloom_node_link_detach_test PRIVATE ..)
target_link_libraries(bloom_node_link_detach_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_link_detach_test)
bloom_add_test(NAME bloom.ui.node-link-detach COMMAND bloom_node_link_detach_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_explicit_wiring_test node_explicit_wiring_tests.cpp)
target_include_directories(bloom_node_explicit_wiring_test PRIVATE ..)
target_link_libraries(bloom_node_explicit_wiring_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_explicit_wiring_test)
bloom_add_test(NAME bloom.ui.node-explicit-wiring COMMAND bloom_node_explicit_wiring_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_driver_gesture_test node_driver_gesture_tests.cpp)
target_include_directories(bloom_node_driver_gesture_test PRIVATE ..)
target_link_libraries(bloom_node_driver_gesture_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_driver_gesture_test)
bloom_add_test(NAME bloom.ui.node-driver-gesture COMMAND bloom_node_driver_gesture_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task FIX2, deliverable 2: one truth across every surface. Its own executable rather than more
# cases inside composition_projection_test.cpp, which owns the cross-editor projection contract this
# task keeps intact.
add_executable(bloom_surface_synchronization_test surface_synchronization_tests.cpp)
target_include_directories(bloom_surface_synchronization_test PRIVATE ..)
target_link_libraries(bloom_surface_synchronization_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_surface_synchronization_test)
bloom_add_test(NAME bloom.ui.surface-synchronization COMMAND bloom_surface_synchronization_test LABELS ui integration kit TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task FIX2, the owner's known case: the timeline row's own controls under a real pointer. Its own
# executable rather than more cases inside timeline_editor_style_tests.cpp, which owns the row
# CHROME contract this task keeps intact.
add_executable(bloom_timeline_row_controls_test timeline_row_controls_tests.cpp)
target_include_directories(bloom_timeline_row_controls_test PRIVATE ..)
target_link_libraries(bloom_timeline_row_controls_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_timeline_row_controls_test)
bloom_add_test(NAME bloom.ui.timeline-row-controls COMMAND bloom_timeline_row_controls_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Lane T2: the Timeline layer table -- labels, switches, sizing, expanded rows, the
# compositor-standard Timeline keys, names that follow their source, and #401's per-tick cost.
add_executable(bloom_timeline_layer_table_test timeline_layer_table_tests.cpp)
target_include_directories(bloom_timeline_layer_table_test PRIVATE ..)
target_link_libraries(bloom_timeline_layer_table_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_timeline_layer_table_test)
bloom_add_test(NAME bloom.ui.timeline-layer-table COMMAND bloom_timeline_layer_table_test LABELS ui integration kit TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task FIX2, deliverable 1: the owner's "values in nodes parameters are not workable", driven
# through real pointer and key events on the production canvas. Its own executable for the same
# reason node_card_tests.cpp is -- and deliberately separate from node_production_wiring_tests.cpp,
# which owns the "no adapter on setSubmit()" contract rather than the gesture contract.
add_executable(bloom_node_field_controls_test node_field_controls_tests.cpp)
target_include_directories(bloom_node_field_controls_test PRIVATE ..)
target_link_libraries(bloom_node_field_controls_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_field_controls_test)
bloom_add_test(NAME bloom.ui.node-field-controls COMMAND bloom_node_field_controls_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_card_value_gesture_test node_card_value_gesture_tests.cpp)
target_include_directories(bloom_node_card_value_gesture_test PRIVATE ..)
target_link_libraries(bloom_node_card_value_gesture_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_card_value_gesture_test)
bloom_add_test(NAME bloom.ui.node-card-value-gesture COMMAND bloom_node_card_value_gesture_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_production_wiring_test node_production_wiring_tests.cpp)
target_include_directories(bloom_node_production_wiring_test PRIVATE ..)
target_link_libraries(bloom_node_production_wiring_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_production_wiring_test)
bloom_add_test(NAME bloom.ui.node-production-wiring COMMAND bloom_node_production_wiring_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task PERF1: the RAM preview cache, the compiled-plan cache, and the RAM Preview command. Its own
# executable rather than more cases inside composition_preview_controller_tests.cpp or
# playback_controller_tests.cpp, each of which owns a contract this task had to leave intact -- and
# this one needs its own counting/pausable preparation function to prove that a cache hit evaluates
# nothing.
add_executable(bloom_ram_preview_test ram_preview_tests.cpp)
target_link_libraries(bloom_ram_preview_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ram_preview_test)
bloom_add_test(
    NAME bloom.ui.ram-preview
    COMMAND bloom_ram_preview_test
    LABELS ui integration animation
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# PERF-RAM-2: the bounded two-deep RAM preview pipeline. Its own executable because the overlap
# proof needs a deterministic two-worker barrier (preparation 2 must start before preparation 1
# finishes) and a controllable hold/release rendezvous for the mid-run range/provenance edits;
# neither belongs in the existing counting fixture. Links bloom_ui alone, exactly as
# bloom_ram_preview_test does, because the submit seam and the service are in that interface.
add_executable(bloom_ram_preview_pipeline_test ram_preview_pipeline_tests.cpp)
target_link_libraries(bloom_ram_preview_pipeline_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ram_preview_pipeline_test)
bloom_add_test(
    NAME bloom.ui.ram-preview-pipeline
    COMMAND bloom_ram_preview_pipeline_test
    LABELS ui integration animation
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# PERF-RAM-2: the real GpuPreviewDisplayService admission seam the two-deep pipeline submits
# through. Kept apart from the behavior suite so that one can be read as controller behavior and
# this one as service admission. With no loader it asserts the honest CPU-fallback/Unavailable
# path; when a pinned loader is present it also drives the enabled service and proves genuine
# two-stage overlap under the real admission (a two-worker barrier), reporting the gate as pending
# rather than failing when no device is actually available.
add_executable(bloom_ram_preview_service_pipeline_test ram_preview_service_pipeline_tests.cpp)
target_link_libraries(bloom_ram_preview_service_pipeline_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ram_preview_service_pipeline_test)
if(DEFINED BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
    set(bloom_ram_preview_vulkan_loader_args
        --loader "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
else()
    set(bloom_ram_preview_vulkan_loader_args)
endif()
bloom_add_test(
    NAME bloom.ui.ram-preview-service-pipeline
    COMMAND bloom_ram_preview_service_pipeline_test ${bloom_ram_preview_vulkan_loader_args}
    LABELS ui integration animation
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The host-painted preview route proofs: the preview is prepared on the device and painted by the
# host, which is how the Viewer shows a frame on every platform (gpuPreviewRouteDisplay() in the
# route proof contract; issue #675). They need no native presentation surface and no Qt Vulkan
# feature, so they are built outside the Vulkan block and run offscreen. Each is a CTest fixture
# setup the runtime native acceptance gate requires: nonce setup -> writers -> gate.
# Linux owes the resident proofs as well (gpuPreviewRouteAlsoProvesResident()), written by the
# harnesses in the Vulkan block below into their own files, so there a preview route has two
# writers, one per display, and the gate requires both.
set(bloom_preview_proofs_resident ON)
if(APPLE OR WIN32)
    set(bloom_preview_proofs_resident OFF)
endif()
if(BLOOM_RENDER_ENABLE_VULKAN AND TARGET Vulkan::Headers)
    add_executable(bloom_ui_gpu_host_painted_preview_route_proof_test
        gpu_host_painted_preview_route_proof_tests.cpp
        gpu_route_proof_harness_support.hpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_host_painted_preview_route_proof_test
        PRIVATE "${PROJECT_SOURCE_DIR}/tests/support" "${PROJECT_SOURCE_DIR}/src/runtime"
                ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_host_painted_preview_route_proof_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_host_painted_preview_route_proof_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_host_painted_preview_route_proof_test)
    foreach(bloom_host_painted_route IN ITEMS viewer ram-preview)
        if(bloom_host_painted_route STREQUAL "viewer")
            set(bloom_host_painted_fixture gpu_route_proof_viewer)
            set(bloom_host_painted_test bloom.ui.gpu_viewer_host_painted_route_proof)
        else()
            set(bloom_host_painted_fixture gpu_route_proof_ram)
            set(bloom_host_painted_test bloom.ui.gpu_ram_preview_host_painted_route_proof)
        endif()
        bloom_add_test(
            NAME ${bloom_host_painted_test}
            COMMAND bloom_ui_gpu_host_painted_preview_route_proof_test
                    --route ${bloom_host_painted_route}
                    --proof-dir "${PROJECT_BINARY_DIR}/gpu-route-proofs"
                    ${bloom_ram_preview_vulkan_loader_args}
            LABELS ui gpu route-proof
            TIMEOUT 300
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
        set_tests_properties(${bloom_host_painted_test}
            PROPERTIES FIXTURES_REQUIRED gpu_route_proofs
                       FIXTURES_SETUP ${bloom_host_painted_fixture})
    endforeach()
endif()

# Task CACHEFIX-1: the byte accounting every cache in the process owes, exercised at the scale of
# the 38 GB incident (200 synthetic 4K RGBA32F frames) and checked against the process's own RSS.
# Links bloom_ui for Qt and the media/runtime cache libraries it pulls in; deliberately not the
# whole window fixture, because the accounting under test lives in the caches.
add_executable(bloom_ui_cache_accounting_test cache_accounting_tests.cpp)
target_link_libraries(bloom_ui_cache_accounting_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_cache_accounting_test)
bloom_add_test(
    NAME bloom.ui.cache-accounting
    COMMAND bloom_ui_cache_accounting_test
    LABELS ui unit cache
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #673: what a large render leaves in memory, in the types that hold it, goes back to the
# operating system when it is let go. Two gigabytes at its peak, so it does not share the
# machine with another memory-heavy test.
add_executable(bloom_ui_host_memory_return_test host_memory_return_tests.cpp)
target_link_libraries(bloom_ui_host_memory_return_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_host_memory_return_test)
bloom_add_test(
    NAME bloom.ui.host-memory-return
    COMMAND bloom_ui_host_memory_return_test
    LABELS ui unit cache
    TIMEOUT 300
)
set_tests_properties(bloom.ui.host-memory-return PROPERTIES RUN_SERIAL TRUE)

# Task CACHE-2: the media disk cache's QSettings surface and status-bar/menu-facing pure logic,
# independent of a full evaluator or window (see main_window_chrome_tests.cpp and
# window_status_bar's own coverage there for the widget-level wiring).
add_executable(bloom_media_disk_cache_settings_test media_disk_cache_settings_tests.cpp)
target_link_libraries(bloom_media_disk_cache_settings_test PRIVATE bloom_ui)
# The deterministic service-purge timeout test reaches the service's private owner-thread purge
# state machine (no device needed), exactly as the resident GPU tests reach the same private root.
target_include_directories(bloom_media_disk_cache_settings_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
bloom_enable_warnings(bloom_media_disk_cache_settings_test)
bloom_add_test(
    NAME bloom.ui.media-disk-cache-settings
    COMMAND bloom_media_disk_cache_settings_test
    LABELS ui unit cache
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The shared global-preferences model: defaults, round-trip, invalid-value fallback, and the
# absolute-path/zero-budget rules the existing readers rely on. Also covers non-finite grid values.
add_executable(bloom_application_preferences_test application_preferences_tests.cpp)
target_link_libraries(bloom_application_preferences_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_application_preferences_test)
bloom_add_test(
    NAME bloom.ui.application-preferences
    COMMAND bloom_application_preferences_test
    LABELS ui unit
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The acceleration-status mapping: a pure, device-free mapping from one already-published GPU
# service status to the Performance page model, including initializing, ready-without-resident,
# lost-presentation, and post-bootstrap device-loss truths. It never constructs a service or device.
add_executable(bloom_acceleration_status_test acceleration_status_tests.cpp)
target_link_libraries(bloom_acceleration_status_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_acceleration_status_test)
bloom_add_test(
    NAME bloom.ui.acceleration-status
    COMMAND bloom_acceleration_status_test
    LABELS ui unit
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The Settings window: control/preference binding, dirty-state gating, Apply emission, restore
# defaults, restart badges, category navigation, and the read-only Performance page.
add_executable(bloom_preferences_window_test preferences_window_tests.cpp)
target_link_libraries(bloom_preferences_window_test PRIVATE bloom_ui Qt6::Test)
target_include_directories(bloom_preferences_window_test PRIVATE ..)
bloom_enable_warnings(bloom_preferences_window_test)
bloom_add_test(
    NAME bloom.ui.preferences-window
    COMMAND bloom_preferences_window_test
    LABELS ui unit
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# UI sound feedback: the event/interaction mapping, the Preferences switches, the rate limit, the
# notification-when-inactive rule, the sound engine over a recording backend (no audio device), and
# the kit control cues.
add_executable(bloom_ui_feedback_test ui_feedback_tests.cpp)
target_link_libraries(bloom_ui_feedback_test PRIVATE bloom_ui Qt6::Test)
target_include_directories(bloom_ui_feedback_test PRIVATE ..)
bloom_enable_warnings(bloom_ui_feedback_test)
bloom_add_test(
    NAME bloom.ui.ui-feedback
    COMMAND bloom_ui_feedback_test
    LABELS ui unit
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_export_panel_test export_panel_tests.cpp)
target_include_directories(bloom_export_panel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_export_panel_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_export_panel_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_export_panel_test)
bloom_add_test(
    NAME bloom.ui.export-panel
    COMMAND bloom_export_panel_test
    LABELS ui integration export
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
)

add_executable(bloom_export_panel_capture export_panel_capture.cpp)
target_include_directories(bloom_export_panel_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_export_panel_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_export_panel_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_export_panel_capture)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.export_panel_capture.dpr${scale} COMMAND bloom_export_panel_capture)
    set_tests_properties(bloom.ui.export_panel_capture.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 120)
endforeach()

add_executable(bloom_export_queue_host_test export_queue_host_tests.cpp)
target_include_directories(bloom_export_queue_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_export_queue_host_test PRIVATE bloom_ui bloom_host_export_testing
                                                            bloom_runtime_compiler Qt6::Test)
bloom_enable_warnings(bloom_export_queue_host_test)
bloom_add_test(
    NAME bloom.ui.export-queue-host
    COMMAND bloom_export_queue_host_test
    LABELS ui integration export
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
)

add_executable(bloom_background_preview_test background_preview_tests.cpp)
target_link_libraries(bloom_background_preview_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_background_preview_test)
bloom_add_test(
    NAME bloom.ui.background-preview
    COMMAND bloom_background_preview_test
    LABELS ui integration animation
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_curve_graph_test curve_graph_tests.cpp)
target_include_directories(bloom_curve_graph_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_curve_graph_test PRIVATE bloom_ui bloom_commands)
bloom_enable_warnings(bloom_curve_graph_test)
bloom_add_test(NAME bloom.ui.curve-graph COMMAND bloom_curve_graph_test LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(bloom_grade_levels_widget_test grade_levels_widget_tests.cpp)
target_include_directories(bloom_grade_levels_widget_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_grade_levels_widget_test PRIVATE bloom_ui bloom_commands)
bloom_enable_warnings(bloom_grade_levels_widget_test)
bloom_add_test(NAME bloom.ui.grade-levels-widgets COMMAND bloom_grade_levels_widget_test LABELS ui integration TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(bloom_grading_cards_test grading_cards_tests.cpp)
target_include_directories(bloom_grading_cards_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_grading_cards_test PRIVATE bloom_ui bloom_commands)
bloom_enable_warnings(bloom_grading_cards_test)
bloom_add_test(NAME bloom.ui.grading-cards COMMAND bloom_grading_cards_test LABELS ui integration TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_properties_registry_test properties_registry_tests.cpp)
target_link_libraries(bloom_properties_registry_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_properties_registry_test)
bloom_add_test(NAME bloom.ui.properties-registry COMMAND bloom_properties_registry_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# #582: the label audit again at a device pixel ratio of 1.5 (the full registry test above runs at 1).
add_test(NAME bloom.ui.properties-registry.labels.dpr1.5 COMMAND bloom_properties_registry_test --label-audit)
set_tests_properties(bloom.ui.properties-registry.labels.dpr1.5 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5"
    LABELS "ui;integration;kit" TIMEOUT 120)
# #344 #349 #509: the value-node readouts resolve, and the path nodes' stamp, source and Alpha
# Contour rows read whole (BLOOM_READOUT_CAPTURE_DIR saves their Properties captures).
bloom_add_test(NAME bloom.ui.properties-readouts COMMAND bloom_properties_registry_test --readouts LABELS ui integration kit TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_properties_layer_time_test properties_layer_time_tests.cpp)
target_link_libraries(bloom_properties_layer_time_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_properties_layer_time_test)
bloom_add_test(NAME bloom.ui.properties-layer-time COMMAND bloom_properties_layer_time_test LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_text_box_overlay_test text_box_overlay_tests.cpp)
target_link_libraries(bloom_text_box_overlay_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_text_box_overlay_test)
bloom_add_test(NAME bloom.ui.text-box-overlay COMMAND bloom_text_box_overlay_test LABELS ui unit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_properties_mockup_test properties_mockup_tests.cpp)
target_link_libraries(bloom_properties_mockup_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_properties_mockup_test)
bloom_add_test(NAME bloom.ui.properties-mockup COMMAND bloom_properties_mockup_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_ui_grammar_audit_test ui_grammar_audit_tests.cpp)
target_include_directories(bloom_ui_grammar_audit_test PRIVATE ..)
target_link_libraries(bloom_ui_grammar_audit_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_ui_grammar_audit_test PRIVATE BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_ui_grammar_audit_test)
foreach(scale IN ITEMS 1 1.5 2)
    add_test(NAME bloom.ui.grammar_audit.dpr${scale} COMMAND bloom_ui_grammar_audit_test)
    set_tests_properties(bloom.ui.grammar_audit.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 60)
endforeach()

# One control look and one control height: every chrome row, panel header and card row of the
# main-window fixture, and each control kind's resting, hover and checked colours.
add_executable(bloom_control_geometry_test control_geometry_tests.cpp)
target_include_directories(bloom_control_geometry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_control_geometry_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_control_geometry_test PRIVATE BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_control_geometry_test)
foreach(scale IN ITEMS 1 1.5 2)
    add_test(NAME bloom.ui.control_geometry.dpr${scale} COMMAND bloom_control_geometry_test)
    set_tests_properties(bloom.ui.control_geometry.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 60)
endforeach()

# Bloom's type roles win over the platform theme: the main-window fixture and every offscreen
# surface under a hostile theme resolve and paint only the bundled faces.
add_executable(bloom_kit_typography_test kit_typography_tests.cpp)
target_include_directories(bloom_kit_typography_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_kit_typography_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_kit_typography_test PRIVATE BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_kit_typography_test)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.kit_typography.dpr${scale} COMMAND bloom_kit_typography_test)
    set_tests_properties(bloom.ui.kit_typography.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;kit;grammar" TIMEOUT 120)
endforeach()

add_executable(bloom_window_golden_test window_golden_tests.cpp)
target_link_libraries(bloom_window_golden_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_window_golden_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build"
    BLOOM_GRAMMAR_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens")
bloom_enable_warnings(bloom_window_golden_test)
# The whole-window references are platform-specific (glyph rasterization differs between
# FreeType on Linux and CoreText on macOS), so the comparison runs only against the Linux-approved
# references; other platforms build the fixture without registering the comparison until their own
# references exist.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(scale IN ITEMS 1 1.5)
        add_test(NAME bloom.ui.window_golden.dpr${scale} COMMAND bloom_window_golden_test)
        set_tests_properties(bloom.ui.window_golden.dpr${scale} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
            LABELS "ui;golden" TIMEOUT 60)
    endforeach()
endif()

# The maximized Viewer at a small and a large size, with and without a composition: captures for
# review, plus the check that no hosted header or footer control lies outside its row.
# The popup chin check: holds one dropdown popup or menu open for a compositor screenshot.
# Task N1 review captures of the Node editor (a tool, not a test):
# bloom_node_canvas_capture <directory> <1|1.5>.
add_executable(bloom_node_canvas_capture node_canvas_capture.cpp)
target_include_directories(bloom_node_canvas_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_node_canvas_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_node_canvas_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_node_canvas_capture)

# #694 / #703 review captures of the Node editor's framing and Add Node placement (a tool, not a
# test): bloom_node_placement_capture <directory> <1|1.5> <prefix> [new-composition|add-node].
add_executable(bloom_node_placement_capture node_placement_capture.cpp)
target_include_directories(bloom_node_placement_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_node_placement_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_node_placement_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_node_placement_capture)

# Lane C review captures of every dialog (a tool, not a test): bloom_dialog_capture <dir> <1|1.5>.
add_executable(bloom_dialog_capture dialog_capture.cpp)
target_include_directories(bloom_dialog_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_dialog_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_dialog_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_dialog_capture)

# Release review captures (a tool, not a test): bloom_review_capture <dir> <1|1.5>.
add_executable(bloom_review_capture review_capture.cpp)
target_include_directories(bloom_review_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_review_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_review_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_review_capture)

# Lane T1 Graph Editor review captures (a tool, not a test): bloom_timeline_graph_capture <dir>,
# once per QT_SCALE_FACTOR.
add_executable(bloom_timeline_graph_capture timeline_graph_capture.cpp)
target_include_directories(bloom_timeline_graph_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_timeline_graph_capture PRIVATE bloom_ui bloom_runtime_compiler
    Qt6::Test)
target_compile_definitions(bloom_timeline_graph_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_timeline_graph_capture)

# UI bug batch review captures (a tool, not a test): bloom_bug_batch_capture [directory] [parts].
add_executable(bloom_bug_batch_capture bug_batch_capture.cpp)
target_include_directories(bloom_bug_batch_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_bug_batch_capture PRIVATE bloom_ui bloom_project bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_bug_batch_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_bug_batch_capture)

add_executable(bloom_ui_bug_batch_test ui_bug_batch_tests.cpp)
target_include_directories(bloom_ui_bug_batch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_ui_bug_batch_test PRIVATE bloom_ui bloom_project bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_ui_bug_batch_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_ui_bug_batch_test)
bloom_add_test(NAME bloom.ui.bug-batch COMMAND bloom_ui_bug_batch_test LABELS ui integration kit TIMEOUT 240
               ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

# #706: the soft popup shadow and its capture (popup-shadow-<platform>.png in the working directory).
add_executable(bloom_popup_shadow_test popup_shadow_tests.cpp)
target_link_libraries(bloom_popup_shadow_test PRIVATE bloom_ui_kit Qt6::Test)
bloom_enable_warnings(bloom_popup_shadow_test)
bloom_add_test(NAME bloom.ui.popup-shadow COMMAND bloom_popup_shadow_test LABELS ui unit kit TIMEOUT 60
               ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

add_executable(bloom_popup_capture popup_capture.cpp)
target_link_libraries(bloom_popup_capture PRIVATE bloom_ui_kit)
bloom_enable_warnings(bloom_popup_capture)

add_executable(bloom_assets_panel_capture assets_panel_capture.cpp)
target_include_directories(bloom_assets_panel_capture PRIVATE ..)
target_link_libraries(bloom_assets_panel_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_assets_panel_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_assets_panel_capture)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.assets_panel_capture.dpr${scale} COMMAND bloom_assets_panel_capture)
    set_tests_properties(bloom.ui.assets_panel_capture.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 120)
endforeach()

add_executable(bloom_chrome_capture chrome_capture.cpp)
target_include_directories(bloom_chrome_capture PRIVATE ..)
target_link_libraries(bloom_chrome_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_chrome_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_chrome_capture)

# #343: the whole window with an imported SVG, for review captures (not a ctest).
add_executable(bloom_svg_window_capture svg_window_capture.cpp)
target_include_directories(bloom_svg_window_capture PRIVATE ..)
target_link_libraries(bloom_svg_window_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_svg_window_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_svg_window_capture)

# #642: review captures of the SVG import modes (not a ctest).
add_executable(bloom_svg_import_capture svg_import_capture.cpp)
target_include_directories(bloom_svg_import_capture PRIVATE ..)
target_link_libraries(bloom_svg_import_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_svg_import_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_svg_import_capture)

# #645: review captures of drilling into grouped SVG artwork (not a ctest).
add_executable(bloom_svg_drill_capture svg_drill_capture.cpp)
target_include_directories(bloom_svg_drill_capture PRIVATE ..)
target_link_libraries(bloom_svg_drill_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_svg_drill_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_svg_drill_capture)

add_executable(bloom_viewer_chrome_capture viewer_chrome_capture.cpp)
target_link_libraries(bloom_viewer_chrome_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_viewer_chrome_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_viewer_chrome_capture)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.viewer_chrome.dpr${scale} COMMAND bloom_viewer_chrome_capture)
    set_tests_properties(bloom.ui.viewer_chrome.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 90)
endforeach()

add_executable(bloom_media_ui_test media_ui_tests.cpp)
target_include_directories(bloom_media_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_media_ui_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_media_ui_test)
foreach(scale IN ITEMS 1 1.5 2)
    add_test(NAME bloom.ui.media.dpr${scale} COMMAND bloom_media_ui_test)
    set_tests_properties(bloom.ui.media.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;media;integration" TIMEOUT 60)
endforeach()

# The tests from here on write real OpenEXR fixtures. Resolve the package where it is first used,
# not only inside the Vulkan block further down, so the targets exist on every platform.
bloom_find_dependency(OpenEXR)

# #364 Cryptomatte UI: the Properties layer picker and matte list, the Viewer matte preview and the
# Viewer eyedropper on a real multi-rank Cryptomatte EXR generated in-test.
add_executable(bloom_cryptomatte_ui_test cryptomatte_ui_tests.cpp)
target_include_directories(bloom_cryptomatte_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS} "${PROJECT_SOURCE_DIR}/src/runtime/tests"
    "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_cryptomatte_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_runtime_evaluator bloom_commands OpenEXR::OpenEXR Qt6::Test)
bloom_enable_warnings(bloom_cryptomatte_ui_test)
add_test(NAME bloom.ui.cryptomatte COMMAND bloom_cryptomatte_ui_test)
set_tests_properties(bloom.ui.cryptomatte PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;media;integration" TIMEOUT 120)

# #483: Z Defocus focus picking in the Viewer (the click, Shift to keep picking, Escape, undo) and
# the focus point drawn over the frame.
add_executable(bloom_depth_focus_pick_ui_test depth_focus_pick_ui_tests.cpp)
target_include_directories(bloom_depth_focus_pick_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_depth_focus_pick_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_runtime_evaluator bloom_commands Qt6::Test)
bloom_enable_warnings(bloom_depth_focus_pick_ui_test)
add_test(NAME bloom.ui.depth-focus-pick COMMAND bloom_depth_focus_pick_ui_test)
set_tests_properties(bloom.ui.depth-focus-pick PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;integration" TIMEOUT 120)

# #484: the on-Viewer handles of Directional, Spin and Zoom Blur: each drag edits live and is one
# command, Escape confirms it; Spin's turn unwraps past a revolution.
add_executable(bloom_motion_blur_viewer_ui_test motion_blur_viewer_ui_tests.cpp)
target_include_directories(bloom_motion_blur_viewer_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_motion_blur_viewer_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_runtime_evaluator bloom_commands Qt6::Test)
bloom_enable_warnings(bloom_motion_blur_viewer_ui_test)
add_test(NAME bloom.ui.motion-blur-viewer COMMAND bloom_motion_blur_viewer_ui_test)
set_tests_properties(bloom.ui.motion-blur-viewer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;integration" TIMEOUT 180)

# #599: the Viewer point handles of a Line's two ends: a constant end is set, a keyed end gets a key
# at the playhead, a driven end takes no drag; each drag is one command.
add_executable(bloom_line_end_viewer_ui_test line_end_viewer_ui_tests.cpp)
target_include_directories(bloom_line_end_viewer_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_line_end_viewer_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_runtime_evaluator bloom_commands Qt6::Test)
bloom_enable_warnings(bloom_line_end_viewer_ui_test)
add_test(NAME bloom.ui.line-end-viewer COMMAND bloom_line_end_viewer_ui_test)
set_tests_properties(bloom.ui.line-end-viewer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;integration" TIMEOUT 180)

# #510: the on-Viewer handles of Procedural Warp: its points and its radius ring, each drag one
# command, Escape confirms it.
add_executable(bloom_warp_viewer_ui_test warp_viewer_ui_tests.cpp)
target_include_directories(bloom_warp_viewer_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_warp_viewer_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_runtime_evaluator bloom_commands Qt6::Test)
bloom_enable_warnings(bloom_warp_viewer_ui_test)
add_test(NAME bloom.ui.warp-viewer COMMAND bloom_warp_viewer_ui_test)
set_tests_properties(bloom.ui.warp-viewer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;integration" TIMEOUT 180)

# #645: drilling into grouped and SVG artwork in the Viewer: levels, picks on evaluated outlines
# on a worker, Escape/Enter/double-click, keyed point drags, selection sync, MCP level, Break Apart.
add_executable(bloom_viewer_drill_test viewer_drill_tests.cpp)
target_include_directories(bloom_viewer_drill_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_viewer_drill_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_runtime_evaluator bloom_commands Qt6::Test)
bloom_enable_warnings(bloom_viewer_drill_test)
add_test(NAME bloom.ui.viewer-drill COMMAND bloom_viewer_drill_test)
set_tests_properties(bloom.ui.viewer-drill PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;integration" TIMEOUT 180)

# #487: the Shuffle slot pickers, channel/layer checklists, data block and audio stream dropdowns
# on a rootless multi-layer EXR, in Properties and on the node cards.
add_executable(bloom_channel_pickers_ui_test channel_pickers_ui_tests.cpp)
target_include_directories(bloom_channel_pickers_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_channel_pickers_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_commands OpenEXR::OpenEXR Qt6::Test)
bloom_enable_warnings(bloom_channel_pickers_ui_test)
add_test(NAME bloom.ui.channel-pickers COMMAND bloom_channel_pickers_ui_test)
set_tests_properties(bloom.ui.channel-pickers PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;media;integration" TIMEOUT 120)

# Assets validity in the application's AssetController: a Cryptomatte-only EXR sequence and an
# H.264 video are valid assets with thumbnails, not "Missing — Relink".
add_executable(bloom_asset_validity_ui_test asset_validity_ui_tests.cpp)
target_include_directories(bloom_asset_validity_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS} "${PROJECT_SOURCE_DIR}/src/runtime/tests")
target_link_libraries(bloom_asset_validity_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_commands OpenEXR::OpenEXR Qt6::Test)
bloom_enable_warnings(bloom_asset_validity_ui_test)
add_test(NAME bloom.ui.asset_validity COMMAND bloom_asset_validity_ui_test
    --media-fixtures "${PROJECT_BINARY_DIR}/apps/bloom-media-worker/fixtures")
set_tests_properties(bloom.ui.asset_validity PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    FIXTURES_REQUIRED media3
    LABELS "ui;media;integration" TIMEOUT 180)

# #364 the owner's Cryptomatte setup end to end: a beauty and a sidecar Cryptomatte-only EXR
# sequence through Assets, node cards, Properties and the Viewer preview.
add_executable(bloom_cryptomatte_sidecar_ui_test cryptomatte_sidecar_ui_tests.cpp)
target_include_directories(bloom_cryptomatte_sidecar_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS} "${PROJECT_SOURCE_DIR}/src/runtime/tests")
target_link_libraries(bloom_cryptomatte_sidecar_ui_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_runtime_evaluator bloom_commands OpenEXR::OpenEXR Qt6::Test)
bloom_enable_warnings(bloom_cryptomatte_sidecar_ui_test)
add_test(NAME bloom.ui.cryptomatte_sidecar COMMAND bloom_cryptomatte_sidecar_ui_test)
set_tests_properties(bloom.ui.cryptomatte_sidecar PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;media;integration" TIMEOUT 180)

add_executable(bloom_media_window_capture media_window_capture.cpp)
target_include_directories(bloom_media_window_capture PRIVATE ..)
target_link_libraries(bloom_media_window_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_media_window_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_media_window_capture)
add_test(NAME bloom.ui.media-window.dpr1 COMMAND bloom_media_window_capture)
set_tests_properties(bloom.ui.media-window.dpr1 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;media;capture" TIMEOUT 60)

add_executable(bloom_color_boundary_test color_boundary_tests.cpp)
target_include_directories(bloom_color_boundary_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_color_boundary_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_color_boundary_test)
foreach(scale IN ITEMS 1 1.25 1.5 2)
    bloom_add_test(NAME bloom.ui.color-boundary.dpr${scale}
        COMMAND bloom_color_boundary_test LABELS ui integration color TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale}")
endforeach()

# End-to-end audio transport: WAV import -> audio layer -> compiled mix -> PlaybackController ->
# AudioEngine (NullBackend) renders non-silent samples, wired exactly like apps/bloom/main.cpp.
add_executable(bloom_audio_transport_test audio_transport_tests.cpp)
target_include_directories(bloom_audio_transport_test PRIVATE ..)
target_link_libraries(bloom_audio_transport_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_audio_transport_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_audio_transport_test)
add_test(NAME bloom.ui.audio-transport COMMAND bloom_audio_transport_test)
set_tests_properties(bloom.ui.audio-transport PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;BLOOM_REDUCED_MOTION=1")

# #400: looped playback keeps the mixer and the transport on one work-area period.
add_executable(bloom_ui_audio_loop_transport_test audio_loop_transport_tests.cpp)
target_link_libraries(bloom_ui_audio_loop_transport_test PRIVATE bloom_ui bloom_runtime_compiler)
bloom_enable_warnings(bloom_ui_audio_loop_transport_test)
bloom_add_test(NAME bloom.ui.audio-loop-transport COMMAND bloom_ui_audio_loop_transport_test
    LABELS ui audio TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_ui_audio_device_control_test audio_device_control_tests.cpp)
target_link_libraries(bloom_ui_audio_device_control_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_audio_device_control_test)
bloom_add_test(NAME bloom.ui.audio-device-control COMMAND bloom_ui_audio_device_control_test
    LABELS ui audio TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_live_value_edit_test live_value_edit_tests.cpp)
target_include_directories(bloom_live_value_edit_test PRIVATE ..)
target_link_libraries(bloom_live_value_edit_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
bloom_enable_warnings(bloom_live_value_edit_test)
bloom_add_test(NAME bloom.ui.live-value-edit COMMAND bloom_live_value_edit_test
    LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_viewer_text_edit_test viewer_text_edit_tests.cpp)
target_link_libraries(bloom_viewer_text_edit_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_viewer_text_edit_test)
bloom_add_test(
    NAME bloom.ui.viewer_text_edit
    COMMAND bloom_viewer_text_edit_test
    LABELS ui unit
    TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_assets_organization_test asset_organization_tests.cpp)
target_include_directories(bloom_assets_organization_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_assets_organization_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_assets_organization_test)
bloom_add_test(NAME bloom.ui.asset-organization COMMAND bloom_assets_organization_test LABELS unit ui
               ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_import_progress_test import_progress_tests.cpp)
target_include_directories(bloom_import_progress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_import_progress_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_import_progress_test)
bloom_add_test(
    NAME bloom.ui.import-progress
    COMMAND bloom_import_progress_test
    LABELS unit ui media
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_svg_break_apart_ui_test svg_break_apart_ui_tests.cpp)
target_include_directories(bloom_svg_break_apart_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_svg_break_apart_ui_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_svg_break_apart_ui_test)
bloom_add_test(
    NAME bloom.ui.svg-break-apart
    COMMAND bloom_svg_break_apart_ui_test
    LABELS unit ui media
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_svg_readd_ui_test svg_readd_ui_tests.cpp)
target_include_directories(bloom_svg_readd_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_svg_readd_ui_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_svg_readd_ui_test)
bloom_add_test(
    NAME bloom.ui.svg-readd
    COMMAND bloom_svg_readd_ui_test
    LABELS unit ui media
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_svg_import_route_test svg_import_route_tests.cpp)
target_include_directories(bloom_svg_import_route_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_svg_import_route_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_svg_import_route_test)
bloom_add_test(
    NAME bloom.ui.svg-import-route
    COMMAND bloom_svg_import_route_test
    LABELS unit ui media
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_import_progress_window_test import_progress_window_tests.cpp)
target_include_directories(bloom_import_progress_window_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_import_progress_window_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_import_progress_window_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_import_progress_window_test)
bloom_add_test(
    NAME bloom.ui.import-progress-window
    COMMAND bloom_import_progress_window_test
    LABELS ui media integration
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;BLOOM_REDUCED_MOTION=1"
)

# Lane C: the kit dialog primitive (KDialog, form, prompts, chooseFile, task dialog).
add_executable(bloom_kit_dialog_test kit_dialog_tests.cpp)
target_link_libraries(bloom_kit_dialog_test PRIVATE bloom_ui_kit Qt6::Test)
bloom_enable_warnings(bloom_kit_dialog_test)
bloom_add_test(
    NAME bloom.ui.kit_dialog
    COMMAND bloom_kit_dialog_test
    LABELS ui unit kit
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
)

# Lane C: Export Composition's H.264 encoder gate and its worker-only verify/Locate.

add_executable(bloom_network_share_paths_test network_share_paths_tests.cpp)
target_include_directories(bloom_network_share_paths_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_network_share_paths_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_network_share_paths_test)
bloom_add_test(
    NAME bloom.ui.network-share-paths
    COMMAND bloom_network_share_paths_test
    LABELS unit ui media integration
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# ADR 0034: an MCP transaction over Streamable HTTP lands in the artist's undo history.
add_executable(bloom_ui_mcp_live_undo_test mcp_live_undo_tests.cpp)
target_include_directories(bloom_ui_mcp_live_undo_test PRIVATE ..)
target_link_libraries(bloom_ui_mcp_live_undo_test PRIVATE bloom_ui bloom_runtime_compiler bloom_mcp_testing
    Qt6::Test)
target_compile_definitions(bloom_ui_mcp_live_undo_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_ui_mcp_live_undo_test)
bloom_add_test(NAME bloom.ui.mcp-live-undo COMMAND bloom_ui_mcp_live_undo_test
    LABELS ui mcp integration TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

# MCP is a UI for agents: the real binding of mcp::McpLiveUiPort over the real window.
add_executable(bloom_ui_mcp_live_ui_binding_test mcp_live_ui_binding_tests.cpp)
target_include_directories(bloom_ui_mcp_live_ui_binding_test PRIVATE ..)
target_link_libraries(bloom_ui_mcp_live_ui_binding_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_host_export_testing Qt6::Test ZLIB::ZLIB)
target_compile_definitions(bloom_ui_mcp_live_ui_binding_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_ui_mcp_live_ui_binding_test)
bloom_add_test(NAME bloom.ui.mcp-live-ui-binding COMMAND bloom_ui_mcp_live_ui_binding_test
    LABELS ui mcp integration TIMEOUT 240
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

# #389: the live-UI port's application members (Viewer settings, preferences, purge, jobs, cancel
# open, reveal, composition presets) over the real window.
add_executable(bloom_ui_mcp_live_ui_application_test mcp_live_ui_binding_application_tests.cpp)
target_include_directories(bloom_ui_mcp_live_ui_application_test PRIVATE ..)
target_link_libraries(bloom_ui_mcp_live_ui_application_test PRIVATE bloom_ui bloom_runtime_compiler
    Qt6::Test)
target_compile_definitions(bloom_ui_mcp_live_ui_application_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_ui_mcp_live_ui_application_test)
bloom_add_test(NAME bloom.ui.mcp-live-ui-application COMMAND bloom_ui_mcp_live_ui_application_test
    LABELS ui mcp integration TIMEOUT 240
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

if(BLOOM_BUILD_PYTHON)
    # #603: script.run end to end against the live Script runtime, over Streamable HTTP.
    add_executable(bloom_ui_mcp_script_run_test mcp_script_run_tests.cpp)
    target_include_directories(bloom_ui_mcp_script_run_test PRIVATE ..)
    target_link_libraries(bloom_ui_mcp_script_run_test PRIVATE bloom_ui bloom_runtime_compiler
        bloom_mcp_testing Qt6::Test)
    target_compile_definitions(bloom_ui_mcp_script_run_test PRIVATE
        BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
    bloom_enable_warnings(bloom_ui_mcp_script_run_test)
    bloom_add_test(NAME bloom.ui.mcp-script-run COMMAND bloom_ui_mcp_script_run_test
        LABELS ui mcp scripting python integration TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

    add_executable(bloom_script_panel_test script_panel_tests.cpp)
    target_include_directories(bloom_script_panel_test PRIVATE ..)
    target_link_libraries(bloom_script_panel_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
    target_compile_definitions(bloom_script_panel_test PRIVATE
        BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build"
        BLOOM_SCRIPT_CLI="$<TARGET_FILE:bloom-cli>")
    add_dependencies(bloom_script_panel_test bloom-cli)
    bloom_enable_warnings(bloom_script_panel_test)
    bloom_add_test(NAME bloom.ui.script-panel COMMAND bloom_script_panel_test
        LABELS ui scripting python integration TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")
endif()

# Presentation coordinator (real Wayland lifecycle) and ViewerGpuPresenter adapter tests. All three
# translation units adopt the borrowed VkInstance through Qt's public QVulkanInstance and therefore
# need both the pinned Vulkan headers and Qt's Vulkan feature; they are registered only when this
# build has them (BLOOM_UI_VULKAN_AVAILABLE, src/ui/CMakeLists.txt). The runtime coordinator's
# Vulkan-free port/bounds gates live in src/runtime/tests. Without a pinned loader argument the
# fixtures skip cleanly; the supervisor passes --loader (and --require-device for the native runs).
# Each fixture also runs a pre-QApplication native Wayland environment gate
# (gpu_native_test_environment.hpp) and returns the CTest skip code 77 when no compositor is
# reachable, so a headless runner reports Skipped instead of crashing inside Qt's platform plugin.
if(BLOOM_UI_VULKAN_AVAILABLE)
    add_executable(bloom_ui_gpu_presentation_coordinator_test
        gpu_presentation_coordinator_tests.cpp)
    target_link_libraries(bloom_ui_gpu_presentation_coordinator_test
        PRIVATE bloom_ui Vulkan::Headers)
    bloom_enable_warnings(bloom_ui_gpu_presentation_coordinator_test)

    add_executable(bloom_ui_gpu_preview_display_service_presentation_test
        gpu_preview_display_service_presentation_tests.cpp)
    target_include_directories(bloom_ui_gpu_preview_display_service_presentation_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_ui_gpu_preview_display_service_presentation_test
        PRIVATE bloom_ui Vulkan::Headers)
    bloom_enable_warnings(bloom_ui_gpu_preview_display_service_presentation_test)

    add_executable(bloom_ui_gpu_preview_display_service_resident_test
        gpu_preview_display_service_resident_tests.cpp)
    target_include_directories(bloom_ui_gpu_preview_display_service_resident_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_ui_gpu_preview_display_service_resident_test
        PRIVATE bloom_ui Vulkan::Headers)
    bloom_enable_warnings(bloom_ui_gpu_preview_display_service_resident_test)

    add_executable(bloom_ui_viewer_gpu_presenter_test
        viewer_gpu_presenter_fake_port_tests.cpp)
    target_include_directories(bloom_ui_viewer_gpu_presenter_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/ui")
    target_link_libraries(bloom_ui_viewer_gpu_presenter_test
        PRIVATE bloom_ui Vulkan::Headers Qt6::Test)
    bloom_enable_warnings(bloom_ui_viewer_gpu_presenter_test)

    add_executable(bloom_ui_viewer_gpu_presenter_native_test
        viewer_gpu_presenter_native_tests.cpp)
    target_link_libraries(bloom_ui_viewer_gpu_presenter_native_test
        PRIVATE bloom_ui Vulkan::Headers Qt6::Test)
    bloom_enable_warnings(bloom_ui_viewer_gpu_presenter_native_test)

    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
        set(bloom_ui_vulkan_loader_args
            --loader "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
    else()
        set(bloom_ui_vulkan_loader_args)
    endif()
    bloom_add_test(
        NAME bloom.ui.gpu_presentation_coordinator
        COMMAND bloom_ui_gpu_presentation_coordinator_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.gpu_preview_display_service_presentation
        COMMAND bloom_ui_gpu_preview_display_service_presentation_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.gpu_preview_display_service_resident
        COMMAND bloom_ui_gpu_preview_display_service_resident_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.viewer_gpu_presenter
        COMMAND bloom_ui_viewer_gpu_presenter_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.viewer_gpu_presenter_native
        COMMAND bloom_ui_viewer_gpu_presenter_native_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    # The five fixtures above share the pre-QApplication native Wayland environment gate and exit
    # with 77 when it is unavailable; register that code as a skip so CTest lists them as Skipped.
    set_tests_properties(
        bloom.ui.gpu_presentation_coordinator
        bloom.ui.gpu_preview_display_service_presentation
        bloom.ui.gpu_preview_display_service_resident
        bloom.ui.viewer_gpu_presenter
        bloom.ui.viewer_gpu_presenter_native
        PROPERTIES SKIP_RETURN_CODE 77
    )
endif()

# Host retire-before-mutation gate. The first target is a CPU-only, archive-free test of the
# asynchronous gate (including the late-callback-after-gate-destruction lifetime path). The second
# is a focused Qt host fixture that drives the real EditorArea/WorkspaceHost gates with a delayed
# fake native-surface editor; it links the host TUs and therefore exercises the actual integration.
add_executable(bloom_ui_editor_native_surface_gate_test
    editor_native_surface_gate_tests.cpp)
target_link_libraries(bloom_ui_editor_native_surface_gate_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_editor_native_surface_gate_test)
bloom_add_test(
    NAME bloom.ui.editor_native_surface_gate
    COMMAND bloom_ui_editor_native_surface_gate_test
    LABELS ui TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_ui_host_native_surface_retirement_test
    host_native_surface_retirement_tests.cpp)
target_link_libraries(bloom_ui_host_native_surface_retirement_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_host_native_surface_retirement_test)
bloom_add_test(
    NAME bloom.ui.host_native_surface_retirement
    COMMAND bloom_ui_host_native_surface_retirement_test
    LABELS ui TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU-resident preview product: the RAM preview cache's fourth-arm retention and invalidation
# contract. The fixture produces a genuine Solid -> resident-display -> owner-registry lease with
# the real product factory on a device; without one the test skips cleanly. The supervisor supplies
# --loader/--require-device for a native run.
add_executable(bloom_ui_preview_frame_cache_resident_test preview_frame_cache_resident_tests.cpp)
target_link_libraries(bloom_ui_preview_frame_cache_resident_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_preview_frame_cache_resident_test)
bloom_add_test(
    NAME bloom.ui.preview-frame-cache-resident
    COMMAND bloom_ui_preview_frame_cache_resident_test
    LABELS ui unit gpu cache render
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# CPU-only edge cases for the RAM preview cache: same-key live reuse (no double charge, generation
# re-stamp) and other-project retention under a standalone no-policy cache, while same-project older
# revisions are still dropped. It builds genuine CPU reference frames and never creates a GPU device,
# so it registers unconditionally and always runs.
add_executable(bloom_ui_preview_frame_cache_cpu_edge_test preview_frame_cache_cpu_edge_tests.cpp)
add_executable(bloom_ui_preview_frame_trace_test preview_frame_trace_tests.cpp)
target_link_libraries(bloom_ui_preview_frame_trace_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_preview_frame_trace_test)
bloom_add_test(
    NAME bloom.ui.preview-frame-trace
    COMMAND bloom_ui_preview_frame_trace_test
    LABELS ui unit trace
    TIMEOUT 30
)
target_link_libraries(bloom_ui_preview_frame_cache_cpu_edge_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_preview_frame_cache_cpu_edge_test)
bloom_add_test(
    NAME bloom.ui.preview-frame-cache-cpu-edge
    COMMAND bloom_ui_preview_frame_cache_cpu_edge_test
    LABELS ui unit cache
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU-resident viewer: the off-UI premultiplied overlay raster and the controller's inert/no-live
# semantics. Device-free and Vulkan-free, so it registers unconditionally and runs offscreen.
add_executable(bloom_ui_viewer_gpu_resident_controller_test viewer_gpu_resident_controller_tests.cpp)
target_link_libraries(bloom_ui_viewer_gpu_resident_controller_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_viewer_gpu_resident_controller_test)
bloom_add_test(
    NAME bloom.ui.viewer_gpu_resident_controller
    COMMAND bloom_ui_viewer_gpu_resident_controller_test
    LABELS ui unit gpu
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The actual ViewerEditor resident integration gate: injects the private port seam and proves the
# unchanged CPU paint path for a non-resident frame. Device-free; the private src/ui port header is
# reached through the module's own source root.
add_executable(bloom_ui_viewer_editor_resident_test viewer_editor_resident_tests.cpp)
target_include_directories(bloom_ui_viewer_editor_resident_test PRIVATE ..)
target_link_libraries(bloom_ui_viewer_editor_resident_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_viewer_editor_resident_test)
bloom_add_test(
    NAME bloom.ui.viewer_editor_resident
    COMMAND bloom_ui_viewer_editor_resident_test
    LABELS ui integration
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #447: the Viewer's process frame against the exported PNG still for a composition with a
# non-black background (RGB within one code, exact alpha), and the background added only at paint.
# The CPU reference half always runs; the GPU half runs with a Vulkan loader and a qualified device
# and prints NotRun otherwise (--require-device makes that a failure).
add_executable(bloom_ui_viewer_export_background_parity_test
    viewer_export_background_parity_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_background_parity_test PRIVATE bloom_ui bloom_host)
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_background_parity_test
        PRIVATE BLOOM_UI_BACKGROUND_PARITY_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_background_parity_test)
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
        set(bloom_ui_background_parity_loader_args
            --loader "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
    else()
        set(bloom_ui_background_parity_loader_args)
    endif()
endif()
bloom_enable_warnings(bloom_ui_viewer_export_background_parity_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_background_parity
    COMMAND bloom_ui_viewer_export_background_parity_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #589: a keyed compound path on a Path node, driving Shape Source, through the Viewer and the
# still, range and movie export routes. The CPU reference half always runs; the GPU half runs with a
# Vulkan loader and a qualified device and prints NotRun otherwise (--require-device fails instead).
add_executable(bloom_ui_viewer_export_path_value_test viewer_export_path_value_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_path_value_test PRIVATE bloom_ui bloom_host)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_ui_viewer_export_path_value_test bloom-media-worker)
endif()
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_path_value_test
        PRIVATE BLOOM_UI_PATH_VALUE_ROUTES_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_path_value_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_path_value_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_path_value
    COMMAND bloom_ui_viewer_export_path_value_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #599: a Line whose ends are keyed and driven, through the Viewer, a cold and warm RAM preview
# at Half, and the still, range and movie export routes. The CPU reference half always runs; the GPU
# half runs with a Vulkan loader and a qualified device and prints NotRun otherwise
# (--require-device fails instead).
add_executable(bloom_ui_viewer_export_line_ends_test viewer_export_line_ends_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_line_ends_test PRIVATE bloom_ui bloom_host)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_ui_viewer_export_line_ends_test bloom-media-worker)
endif()
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_line_ends_test
        PRIVATE BLOOM_UI_LINE_END_ROUTES_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_line_ends_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_line_ends_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_line_ends
    COMMAND bloom_ui_viewer_export_line_ends_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #608: OCIO File Transform (a channel-rotating 3D LUT) through the Viewer, a cold and warm
# RAM preview at Half, and the still, range and movie export routes, with the transform's bypassed
# and no-LUT states. The CPU half always runs; the GPU half runs with a Vulkan loader and a
# qualified device and prints NotRun otherwise (--require-device fails instead).
add_executable(bloom_ui_viewer_export_lut_test viewer_export_lut_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_lut_test PRIVATE bloom_ui bloom_host)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_ui_viewer_export_lut_test bloom-media-worker)
endif()
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_lut_test
        PRIVATE BLOOM_UI_LUT_ROUTES_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_lut_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_lut_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_lut
    COMMAND bloom_ui_viewer_export_lut_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #648: exports stay on the device while the Viewer and a RAM preview hold their frames and
# the document takes many small revisions: rounds of edits, a RAM preview fill and two exports
# straight after one another at 512x288, each compared with the CPU reference export. The CPU half
# always runs; the GPU half runs with a Vulkan loader and a qualified device and prints NotRun
# otherwise (--require-device fails instead).
add_executable(bloom_ui_viewer_export_revisions_test viewer_export_revisions_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_revisions_test PRIVATE bloom_ui bloom_host)
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_revisions_test
        PRIVATE BLOOM_UI_EXPORT_REVISIONS_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_revisions_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_revisions_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_revisions
    COMMAND bloom_ui_viewer_export_revisions_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 600
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #343: imported SVG artwork (1,200 elements, a keyed path, gradient, clip, nested groups)
# through the Viewer, a cold and warm RAM preview at Half, and the still, range and movie export
# routes. The CPU half always runs; the GPU half runs with a Vulkan loader and a qualified device
# and prints NotRun otherwise (--require-device fails instead).
add_executable(bloom_ui_viewer_export_svg_artwork_test viewer_export_svg_artwork_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_svg_artwork_test PRIVATE bloom_ui bloom_host)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_ui_viewer_export_svg_artwork_test bloom-media-worker)
endif()
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_svg_artwork_test
        PRIVATE BLOOM_UI_SVG_ARTWORK_ROUTES_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_svg_artwork_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_svg_artwork_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_svg_artwork
    COMMAND bloom_ui_viewer_export_svg_artwork_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 600
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# #645: the Viewer's isolation (the outside of an entered group at 30%) through the real Viewer
# route: part of every preview and cache identity, never served across levels, a RAM Preview filled
# at one level never shown at the other, and never in a still, range or scripted render. The CPU
# route always runs; the GPU route (GPU-prepared packed product, 1080p and 4K included) runs with a
# Vulkan loader and a qualified device and prints NotRun otherwise.
add_executable(bloom_ui_viewer_isolation_test viewer_isolation_tests.cpp)
target_link_libraries(bloom_ui_viewer_isolation_test PRIVATE bloom_ui bloom_host)
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_isolation_test PRIVATE BLOOM_UI_ISOLATION_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_isolation_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_isolation_test)
bloom_add_test(
    NAME bloom.ui.viewer_isolation
    COMMAND bloom_ui_viewer_isolation_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Real Wayland device present/ack/resize/resume for the resident controller. Without a packaged
# loader it skips cleanly (no --loader is passed by normal CTest, which stays offscreen); the
# supervisor passes --loader/--require-device under QT_QPA_PLATFORM=wayland for the native run.
add_executable(bloom_ui_viewer_gpu_resident_native_test viewer_gpu_resident_native_tests.cpp)
target_link_libraries(bloom_ui_viewer_gpu_resident_native_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_viewer_gpu_resident_native_test)
bloom_add_test(
    NAME bloom.ui.viewer_gpu_resident_native
    COMMAND bloom_ui_viewer_gpu_resident_native_test
    LABELS ui integration gpu presentation
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The typed app-activation bootstrap helper: budget/cap alignment, additive cache limits, capability
# fallback and session-refreshing media context. Device-free and Vulkan-free.
add_executable(bloom_ui_gpu_viewer_bootstrap_test gpu_viewer_bootstrap_tests.cpp)
target_link_libraries(bloom_ui_gpu_viewer_bootstrap_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_gpu_viewer_bootstrap_test)
bloom_add_test(
    NAME bloom.ui.gpu_viewer_bootstrap
    COMMAND bloom_ui_gpu_viewer_bootstrap_test
    LABELS ui unit gpu
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The resident cache sublimit + LRU eviction regression against the genuine product fixture. It
# creates a device; without a packaged loader it skips cleanly. The supervisor passes
# --loader/--require-device for the native run.
add_executable(bloom_ui_preview_frame_cache_resident_limits_test
    preview_frame_cache_resident_limits_tests.cpp)
target_link_libraries(bloom_ui_preview_frame_cache_resident_limits_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_preview_frame_cache_resident_limits_test)
bloom_add_test(
    NAME bloom.ui.preview-frame-cache-resident-limits
    COMMAND bloom_ui_preview_frame_cache_resident_limits_test
    LABELS ui unit gpu cache
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU end-to-end vertical acceptance over a genuine media document. The two service-chain targets
# adopt the borrowed VkInstance through Qt's public QVulkanInstance and write real OpenEXR fixtures,
# so they need both the pinned Vulkan headers and Qt's Vulkan feature plus the dependency prefix's
# OpenEXR/Imath, and register only when this build has them. Normal CTest runs use
# QT_QPA_PLATFORM=offscreen; the supervisor passes
# --loader/--require-device for the real native gate, and --require-device fails closed.
if(BLOOM_UI_VULKAN_AVAILABLE)
    add_executable(bloom_ui_gpu_resident_service_chain_test
        gpu_resident_service_chain_tests.cpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_resident_service_chain_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_resident_service_chain_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_resident_service_chain_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_resident_service_chain_test)
    bloom_add_test(
        NAME bloom.ui.gpu_resident_service_chain
        COMMAND bloom_ui_gpu_resident_service_chain_test
        LABELS ui gpu resident media
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(bloom_ui_gpu_processing_phase_native_test gpu_processing_phase_native_tests.cpp)
    target_include_directories(bloom_ui_gpu_processing_phase_native_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_processing_phase_native_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_processing_phase_native_test)
    bloom_add_test(NAME bloom.ui.gpu_processing_phase_native
        COMMAND bloom_ui_gpu_processing_phase_native_test LABELS ui gpu memory
        TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(bloom.ui.gpu_processing_phase_native PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(bloom_ui_gpu_packed_viewer_test gpu_packed_viewer_tests.cpp)
    target_include_directories(bloom_ui_gpu_packed_viewer_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render" "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_packed_viewer_test
        PRIVATE bloom_ui bloom_host bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_package_gpu_shader_tools(bloom_ui_gpu_packed_viewer_test)
    bloom_enable_warnings(bloom_ui_gpu_packed_viewer_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_packed_viewer_test)

    # Issue #414: moves between a 1x and a 1.5x screen that flip the Auto divisor (Half/Full),
    # resizes, maximize and hide/show, with the pixel probe live over a 2048x858 composition of
    # 16-bit PNG sequence layers, on the application-shaped workspace and the shipping GPU-prepared
    # packed preview service. After the first round trip it asserts an RSS, ledger, operation and
    # preview cache and GPU-image plateau, a bounded per-cycle transient peak, no re-preparation
    # of the parked frame, no Viewer frame on the CPU fallback, and zero retained presenter
    # records. It installs its own two-screen offscreen layout, uses the GPU device when a loader
    # is pinned, and skips (77) on any other platform.
    add_executable(bloom_ui_window_surface_memory_soak_test window_surface_memory_soak_tests.cpp)
    target_include_directories(bloom_ui_window_surface_memory_soak_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render" "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_window_surface_memory_soak_test
        PRIVATE bloom_ui bloom_host bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_package_gpu_shader_tools(bloom_ui_window_surface_memory_soak_test)
    bloom_enable_warnings(bloom_ui_window_surface_memory_soak_test)

    add_executable(bloom_ui_gpu_viewer_service_chain_test
        gpu_viewer_service_chain_tests.cpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_viewer_service_chain_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_viewer_service_chain_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_viewer_service_chain_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_viewer_service_chain_test)
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
        set(bloom_ui_gpu_viewer_service_chain_loader_args
            --loader "${BLOOM_DEPENDENCY_PREFIX}/${BLOOM_VULKAN_LOADER_PREFIX_DIR}/${BLOOM_VULKAN_LOADER_FILE_NAME}")
    else()
        set(bloom_ui_gpu_viewer_service_chain_loader_args)
    endif()
    foreach(packed_case IN ITEMS paused playing animated half100 full100 partial host_ownership host_retirement
                                 full100_12gib full100_8gib full100_6gib)
        if(packed_case STREQUAL "playing")
            set(packed_args --playing)
        elseif(packed_case STREQUAL "animated")
            set(packed_args --animated-drag)
        elseif(packed_case STREQUAL "half100")
            set(packed_args --cache100-half)
        elseif(packed_case STREQUAL "full100")
            set(packed_args --cache100-full)
        elseif(packed_case STREQUAL "partial")
            set(packed_args --cache100-full --partial)
        elseif(packed_case MATCHES "^full100_([0-9]+)gib$")
            # #682: the 100-frame fill planned for a 12, 8 and 6 GiB card on any machine.
            set(packed_args --cache100-full --device-memory-gib ${CMAKE_MATCH_1})
        elseif(packed_case STREQUAL "host_retirement")
            set(packed_args --host-retirement)
        elseif(packed_case STREQUAL "host_ownership")
            set(packed_args --host-ownership)
        else()
            set(packed_args)
        endif()
        bloom_add_test(
            NAME bloom.ui.gpu_packed_viewer_${packed_case}
            COMMAND bloom_ui_gpu_packed_viewer_test
                    ${bloom_ui_gpu_viewer_service_chain_loader_args} ${packed_args}
            LABELS ui gpu viewer packed native
            TIMEOUT 150
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        set_tests_properties(bloom.ui.gpu_packed_viewer_${packed_case}
            PROPERTIES SKIP_RETURN_CODE 77)
        if(packed_case MATCHES "gib$")
            # A modelled card is the only limit the case measures, so it runs with the device to
            # itself: other tests holding device memory would make it a smaller card than modelled.
            set_tests_properties(bloom.ui.gpu_packed_viewer_${packed_case} PROPERTIES RUN_SERIAL TRUE)
        endif()
    endforeach()

    bloom_add_test(
        NAME bloom.ui.window_surface_memory_soak
        COMMAND bloom_ui_window_surface_memory_soak_test
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu viewer memory soak device
        TIMEOUT 600
        ENVIRONMENT QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=generic BLOOM_REDUCED_MOTION=1)
    set_tests_properties(bloom.ui.window_surface_memory_soak PROPERTIES SKIP_RETURN_CODE 77)

    bloom_add_test(
        NAME bloom.ui.gpu_viewer_service_chain
        COMMAND bloom_ui_gpu_viewer_service_chain_test
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu viewer resident
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    # Genuine GPU route-proof writers. These are the real production viewer and RAM-preview
    # harnesses; each publishes its route proof into the build-owned run-scoped proof directory the
    # runtime native acceptance gate reads. They are CTest fixture SETUP tests (their own fixtures
    # are required by the gate) and they require the runtime `gpu_route_proofs` setup, so the order
    # is: nonce setup -> writer(s) -> final contract gate. The gate lives in the runtime tree and
    # never links the UI shell; only the writer fixtures are appended below. A Wayland platform is
    # inherited (CI sets offscreen, so the writer skips and the gate reports the proof missing).
    set(BLOOM_GPU_ROUTE_PROOF_DIR "${PROJECT_BINARY_DIR}/gpu-route-proofs")

    add_executable(bloom_ui_gpu_viewer_route_proof_test
        gpu_viewer_route_proof_tests.cpp
        gpu_route_proof_harness_support.hpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_viewer_route_proof_test
        PRIVATE "${PROJECT_SOURCE_DIR}/tests/support" "${PROJECT_SOURCE_DIR}/src/runtime"
                ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_viewer_route_proof_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_viewer_route_proof_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_viewer_route_proof_test)
    if(bloom_preview_proofs_resident)
        bloom_add_test(
            NAME bloom.ui.gpu_viewer_route_proof
            COMMAND bloom_ui_gpu_viewer_route_proof_test
                    --proof-dir "${BLOOM_GPU_ROUTE_PROOF_DIR}"
                    ${bloom_ui_gpu_viewer_service_chain_loader_args}
            LABELS ui gpu viewer route-proof
            TIMEOUT 300
        )
        set_tests_properties(bloom.ui.gpu_viewer_route_proof
            PROPERTIES FIXTURES_REQUIRED gpu_route_proofs FIXTURES_SETUP gpu_route_proof_viewer)
    endif()

    # #524 and #514: node-specific interactive-viewer evidence. Each scenario drives the production
    # viewer chain over a composition using one new node and checks the resident frame against the
    # CPU oracle; it writes no route proof, and skips (passes) without a Wayland platform and device.
    add_executable(bloom_ui_gpu_node_viewer_evidence_test
        gpu_node_viewer_evidence_tests.cpp
        gpu_route_proof_harness_support.hpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_node_viewer_evidence_test
        PRIVATE "${PROJECT_SOURCE_DIR}/tests/support" "${PROJECT_SOURCE_DIR}/src/runtime"
                ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_node_viewer_evidence_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    # The viewer's GPU scene builder needs the shared OCIO context (field programs, effects); it
    # consumes the real packaged tools staged beside this executable, as the application does.
    target_compile_definitions(bloom_ui_gpu_node_viewer_evidence_test PRIVATE
        "BLOOM_NODE_EVIDENCE_EXECUTABLE=\"$<TARGET_FILE:bloom_ui_gpu_node_viewer_evidence_test>\"")
    include(BloomGpuShaderTools)
    bloom_package_gpu_shader_tools(bloom_ui_gpu_node_viewer_evidence_test)
    bloom_enable_warnings(bloom_ui_gpu_node_viewer_evidence_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_node_viewer_evidence_test)
    foreach(scenario IN ITEMS four-color-gradient denoise-median denoise-bilateral denoise-spatial
                              denoise-temporal blur-separate blur-per-channel transform-skew hue-vibrance
                              hue-colorize node-group tile-mirror-tile tile-mirror-wrap-offset
                              tile-mirror-mirror tile-mirror-kaleidoscope inner-shadow
                              inner-glow-edge inner-glow-center quantize-threshold
                              quantize-threshold-soft quantize-posterize
                              quantize-posterize-channels dither-blue-noise
                              dither-ordered dither-stored-mono dither-held procedural-warp-twirl
                              procedural-warp-bulge-pinch procedural-warp-spherize
                              procedural-warp-ripple procedural-warp-wave procedural-warp-polar
                              procedural-warp-magnify
                              procedural-warp-warp procedural-warp-bend procedural-warp-split
                              procedural-warp-split-asymmetric field-noise-vector
                              procedural-warp-turbulent procedural-warp-drizzle
                              procedural-warp-magnify-pull procedural-warp-ripple-pulse)
        bloom_add_test(
            NAME bloom.ui.gpu_node_viewer_evidence.${scenario}
            COMMAND bloom_ui_gpu_node_viewer_evidence_test --scenario ${scenario}
                    ${bloom_ui_gpu_viewer_service_chain_loader_args}
            LABELS ui gpu viewer node-evidence
            TIMEOUT 300
        )
    endforeach()
    # #511, #528, #519, #527 and #510: a cold then a warm RAM preview over four keyed frames through the real RAM
    # preview controller, every frame held to the CPU oracle, the warm pass dispatching nothing.
    foreach(scenario IN ITEMS tile-mirror-tile tile-mirror-wrap-offset tile-mirror-mirror
                              tile-mirror-kaleidoscope inner-shadow inner-glow-edge inner-glow-center
                              quantize-threshold quantize-threshold-soft quantize-posterize
                              quantize-posterize-channels dither-blue-noise dither-ordered
                              dither-stored-mono dither-held procedural-warp-twirl
                              procedural-warp-bulge-pinch procedural-warp-spherize
                              procedural-warp-ripple procedural-warp-wave procedural-warp-polar
                              procedural-warp-magnify
                              procedural-warp-warp procedural-warp-bend procedural-warp-split
                              procedural-warp-split-asymmetric field-noise-vector
                              procedural-warp-turbulent procedural-warp-drizzle
                              procedural-warp-magnify-pull procedural-warp-ripple-pulse)
        bloom_add_test(
            NAME bloom.ui.gpu_node_viewer_evidence.ram-${scenario}
            COMMAND bloom_ui_gpu_node_viewer_evidence_test --scenario ${scenario} --ram 4
                    ${bloom_ui_gpu_viewer_service_chain_loader_args}
            LABELS ui gpu viewer node-evidence
            TIMEOUT 300
        )
    endforeach()
    # #524: the node's frame as the process's first, through the application's lazily resolving
    # viewer stage (no baseline, no tools resolved beforehand) and an empty driver shader cache: a
    # cold pipeline compile is host work and must not run out the native dispatch deadline.
    foreach(scenario IN ITEMS four-color-gradient denoise-temporal node-group dither-blue-noise
                              procedural-warp-twirl procedural-warp-turbulent
                              procedural-warp-warp)
        bloom_add_test(
            NAME bloom.ui.gpu_node_viewer_evidence.cold-${scenario}
            COMMAND bloom_ui_gpu_node_viewer_evidence_test --scenario ${scenario} --cold
                    ${bloom_ui_gpu_viewer_service_chain_loader_args}
            LABELS ui gpu viewer node-evidence
            TIMEOUT 300
        )
    endforeach()

    add_executable(bloom_ui_gpu_ram_preview_route_proof_test
        gpu_ram_preview_route_proof_tests.cpp
        gpu_route_proof_harness_support.hpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_ram_preview_route_proof_test
        PRIVATE "${PROJECT_SOURCE_DIR}/tests/support" "${PROJECT_SOURCE_DIR}/src/runtime"
                ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_ram_preview_route_proof_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_ram_preview_route_proof_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_ram_preview_route_proof_test)
    if(bloom_preview_proofs_resident)
        bloom_add_test(
            NAME bloom.ui.gpu_ram_preview_route_proof
            COMMAND bloom_ui_gpu_ram_preview_route_proof_test
                    --proof-dir "${BLOOM_GPU_ROUTE_PROOF_DIR}"
                    ${bloom_ui_gpu_viewer_service_chain_loader_args}
            LABELS ui gpu ram route-proof
            TIMEOUT 300
        )
        set_tests_properties(bloom.ui.gpu_ram_preview_route_proof
            PROPERTIES FIXTURES_REQUIRED gpu_route_proofs FIXTURES_SETUP gpu_route_proof_ram)
    endif()
    # ADR 0028 animated masks through the same real RAM preview chain: a mask path keyed on 24
    # frames, the scene builder carrying the shared preparer from this target's packaged shader
    # tools, cold GPU frames against the CPU oracle with at most one shader compile, and a warm
    # replay with zero preparation, dispatch or compile. It writes no route proof.
    target_compile_definitions(bloom_ui_gpu_ram_preview_route_proof_test PRIVATE
        "BLOOM_GPU_RAM_TEST_EXECUTABLE=\"$<TARGET_FILE:bloom_ui_gpu_ram_preview_route_proof_test>\"")
    bloom_package_gpu_shader_tools(bloom_ui_gpu_ram_preview_route_proof_test)
    bloom_add_test(
        NAME bloom.ui.gpu_ram_preview_animated_mask
        COMMAND bloom_ui_gpu_ram_preview_route_proof_test --animated-mask
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu ram
        TIMEOUT 300
    )
    # #344 #530 #509: the path nodes (a keyed Trim Path, Boolean Paths, a dashed Path Stroke and its
    # core through Glow) in the interactive viewer and RAM preview on the real service: GPU-resident
    # frames against the CPU oracle, a warm replay with no work, and an operand edit re-rendered.
    bloom_add_test(
        NAME bloom.ui.gpu_ram_preview_path_nodes
        COMMAND bloom_ui_gpu_ram_preview_route_proof_test --path-nodes
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu ram
        TIMEOUT 300
    )
    # #349: Image Statistics (driving Levels) and Denoise with Auto strength in the interactive
    # viewer and RAM preview on the real service, each frame's statistics measured once on the
    # application's device route and equal to the CPU reference's; a warm replay measures nothing,
    # an edit measures again, and a refused device route takes the CPU path labelled CPU.
    bloom_add_test(
        NAME bloom.ui.gpu_ram_preview_image_statistics
        COMMAND bloom_ui_gpu_ram_preview_route_proof_test --image-statistics
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu ram
        TIMEOUT 300
    )

    # Independent native acceptance for direct manipulation ON the genuine GPU-resident viewer
    # route: real blank project -> real first-composition/Solid commands -> real service resident
    # frame -> viewer mouse select/drag/commit/undo, plus the resident overlay border and empty
    # invitation pixel checks. It is not a proof writer and does not join the route-proof fixture
    # order. Under normal CTest (offscreen) it skips with 77; the native supervisor passes
    # --loader/--require-device under QT_QPA_PLATFORM=wayland.
    add_executable(bloom_ui_viewer_resident_interaction_native_test
        viewer_resident_interaction_native_tests.cpp
        viewer_resident_interaction_support.hpp
        viewer_resident_drill_proof.ipp
        viewer_resident_transition_support.hpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_viewer_resident_interaction_native_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render" "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_viewer_resident_interaction_native_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR bloom_render_ocio_test_faults)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_resident_interaction_native_test)
    bloom_enable_warnings(bloom_ui_viewer_resident_interaction_native_test)
    bloom_enable_strict_floating_point(bloom_ui_viewer_resident_interaction_native_test)
    bloom_add_test(
        NAME bloom.ui.viewer_resident_interaction_native
        COMMAND bloom_ui_viewer_resident_interaction_native_test
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu viewer resident interaction native
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(bloom.ui.viewer_resident_interaction_native
        PROPERTIES SKIP_RETURN_CODE 77)

    # These native modes use real EditorArea controls and real submitted-work barriers. Device-free
    # CTest skips explicitly; native required-device supervision executes them with validation.
    foreach(resident_transition_arm IN ITEMS neutral general)
        set(resident_transition_args --transitions)
        if(resident_transition_arm STREQUAL "general")
            list(APPEND resident_transition_args --general)
        endif()
        bloom_add_test(
            NAME bloom.ui.viewer_resident_transitions_${resident_transition_arm}
            COMMAND bloom_ui_viewer_resident_interaction_native_test
                    ${bloom_ui_gpu_viewer_service_chain_loader_args} ${resident_transition_args}
            LABELS ui gpu viewer resident interaction native
            TIMEOUT 150
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        set_tests_properties(bloom.ui.viewer_resident_transitions_${resident_transition_arm}
            PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    # #645: native double-clicks drill three groups deep into imported SVG shapes on the resident
    # route; every level is a resident frame with that level's isolation, picks read its geometry.
    bloom_add_test(
        NAME bloom.ui.viewer_resident_drill_native
        COMMAND bloom_ui_viewer_resident_interaction_native_test
                ${bloom_ui_gpu_viewer_service_chain_loader_args} --drill
        LABELS ui gpu viewer resident interaction native
        TIMEOUT 150
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(bloom.ui.viewer_resident_drill_native PROPERTIES SKIP_RETURN_CODE 77)
    bloom_add_test(
        NAME bloom.ui.viewer_resident_shutdown_inflight
        COMMAND bloom_ui_viewer_resident_interaction_native_test
                ${bloom_ui_gpu_viewer_service_chain_loader_args} --shutdown-in-flight
        LABELS ui gpu viewer resident native
        TIMEOUT 150
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(bloom.ui.viewer_resident_shutdown_inflight PROPERTIES SKIP_RETURN_CODE 77)

    # Bounded native acceptance for the resident viewer surface across a DOCUMENT REPLACEMENT
    # (File > Open / New) while a live native target exists: genuine applied present for the swapped
    # frame, cover hidden after ack, container inside the viewer, native input activation before and
    # after, and the CPU-only hide/show transition through a composition-less rebind. Skips with 77
    # off a real device.
    add_executable(bloom_ui_viewer_resident_project_swap_native_test
        viewer_resident_project_swap_native_tests.cpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_viewer_resident_project_swap_native_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_viewer_resident_project_swap_native_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_viewer_resident_project_swap_native_test)
    bloom_enable_strict_floating_point(bloom_ui_viewer_resident_project_swap_native_test)
    bloom_add_test(
        NAME bloom.ui.viewer_resident_project_swap_native
        COMMAND bloom_ui_viewer_resident_project_swap_native_test
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu viewer resident native
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(bloom.ui.viewer_resident_project_swap_native
        PROPERTIES SKIP_RETURN_CODE 77)

    # Final contract dependency order: the runtime native acceptance gate lists these two writer
    # fixtures in its own FIXTURES_REQUIRED (src/runtime/tests/CMakeLists.txt), because a test may
    # not set a property on a test that lives in another directory. CTest then runs, in order, the
    # runtime nonce setup, these writers, and the gate that reads their proofs.
endif()

# Device-free ViewerEditor view-coordinate oracle: fit / zoom-about-cursor / pan pinned against an
# independent CPU reference built from the production fitDisplayRect()/actualPixelRect() primitives.
add_executable(bloom_ui_gpu_viewer_end_to_end_test gpu_viewer_end_to_end_tests.cpp)
target_link_libraries(bloom_ui_gpu_viewer_end_to_end_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_gpu_viewer_end_to_end_test)
bloom_enable_strict_floating_point(bloom_ui_gpu_viewer_end_to_end_test)
bloom_add_test(
    NAME bloom.ui.gpu_viewer_end_to_end
    COMMAND bloom_ui_gpu_viewer_end_to_end_test
    LABELS ui gpu viewer
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_ui_processing_capacity_retry_test processing_capacity_retry_tests.cpp)
target_link_libraries(bloom_ui_processing_capacity_retry_test PRIVATE bloom_ui bloom_runtime_evaluator)
bloom_enable_warnings(bloom_ui_processing_capacity_retry_test)
bloom_enable_strict_floating_point(bloom_ui_processing_capacity_retry_test)
bloom_add_test(NAME bloom.ui.processing_capacity_retry COMMAND bloom_ui_processing_capacity_retry_test
    LABELS ui runtime memory TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_packed_host_shutdown_test packed_host_shutdown_tests.cpp)
target_include_directories(bloom_packed_host_shutdown_test PRIVATE "${PROJECT_SOURCE_DIR}/apps/bloom")
target_link_libraries(bloom_packed_host_shutdown_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_packed_host_shutdown_test)
bloom_add_test(NAME bloom.ui.packed_host_shutdown COMMAND bloom_packed_host_shutdown_test
    LABELS ui memory shutdown TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ADR 0024 checkpoint (#353): nested-composition layers on their own frames.
add_executable(bloom_ui_layer_time_checkpoint_test layer_time_checkpoint_tests.cpp)
target_link_libraries(bloom_ui_layer_time_checkpoint_test PRIVATE bloom_ui)
target_include_directories(bloom_ui_layer_time_checkpoint_test PRIVATE ..)
bloom_enable_warnings(bloom_ui_layer_time_checkpoint_test)
bloom_add_test(NAME bloom.ui.layer-time-checkpoint COMMAND bloom_ui_layer_time_checkpoint_test LABELS ui unit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_ui_timeline_key_projection_test timeline_key_projection_tests.cpp)
target_link_libraries(bloom_ui_timeline_key_projection_test PRIVATE bloom_ui)
target_include_directories(bloom_ui_timeline_key_projection_test PRIVATE ..)
bloom_enable_warnings(bloom_ui_timeline_key_projection_test)
bloom_add_test(NAME bloom.ui.timeline-key-projection COMMAND bloom_ui_timeline_key_projection_test LABELS ui unit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# #380: Assets and file drops on the Timeline and the Viewer create layers, as in a layer-based
# compositor.
add_executable(bloom_ui_layer_drop_test layer_drop_tests.cpp)
target_link_libraries(bloom_ui_layer_drop_test PRIVATE bloom_ui bloom_commands)
target_include_directories(bloom_ui_layer_drop_test PRIVATE ..)
bloom_enable_warnings(bloom_ui_layer_drop_test)
bloom_add_test(NAME bloom.ui.layer-drop COMMAND bloom_ui_layer_drop_test LABELS ui integration
    TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

# #380 in the whole window: the Assets header for a video and a composition, and Timeline drags
# from Assets (row insertion, time marker, a sequenced multi-item drop).
add_executable(bloom_ui_layer_drop_window_test layer_drop_window_tests.cpp)
target_include_directories(bloom_ui_layer_drop_window_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_ui_layer_drop_window_test PRIVATE bloom_ui bloom_runtime_compiler
    bloom_commands Qt6::Test)
target_compile_definitions(bloom_ui_layer_drop_window_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_ui_layer_drop_window_test)
add_test(NAME bloom.ui.layer-drop-window COMMAND bloom_ui_layer_drop_window_test
    --media-fixtures "${PROJECT_BINARY_DIR}/apps/bloom-media-worker/fixtures")
set_tests_properties(bloom.ui.layer-drop-window PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;BLOOM_REDUCED_MOTION=1"
    FIXTURES_REQUIRED media3
    LABELS "ui;media;integration" TIMEOUT 180)

# #490: the Image Preview node's thumbnail and node editor background.
add_executable(bloom_ui_image_preview_test image_preview_tests.cpp)
target_link_libraries(bloom_ui_image_preview_test PRIVATE bloom_ui)
target_include_directories(bloom_ui_image_preview_test PRIVATE ..)
bloom_enable_warnings(bloom_ui_image_preview_test)
bloom_add_test(NAME bloom.ui.image-preview COMMAND bloom_ui_image_preview_test
    LABELS ui integration TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

# #380: the Assets header's composition frames, prepared on a worker and kept per revision.
add_executable(bloom_ui_composition_thumbnail_test composition_thumbnail_tests.cpp)
target_link_libraries(bloom_ui_composition_thumbnail_test PRIVATE bloom_ui)
target_include_directories(bloom_ui_composition_thumbnail_test PRIVATE ..)
bloom_enable_warnings(bloom_ui_composition_thumbnail_test)
bloom_add_test(NAME bloom.ui.composition-thumbnail COMMAND bloom_ui_composition_thumbnail_test
    LABELS ui integration TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")

add_executable(bloom_ui_timeline_layer_timing_test timeline_layer_timing_gesture_tests.cpp)
target_link_libraries(bloom_ui_timeline_layer_timing_test PRIVATE bloom_ui)
target_include_directories(bloom_ui_timeline_layer_timing_test PRIVATE ..)
bloom_enable_warnings(bloom_ui_timeline_layer_timing_test)
bloom_add_test(NAME bloom.ui.timeline-layer-timing COMMAND bloom_ui_timeline_layer_timing_test LABELS ui unit TIMEOUT 30)

add_executable(bloom_properties_layer_masks_test properties_layer_masks_tests.cpp)
target_link_libraries(bloom_properties_layer_masks_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_properties_layer_masks_test)
bloom_add_test(NAME bloom.ui.properties-layer-masks COMMAND bloom_properties_layer_masks_test LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ADR 0028 Timeline authoring: Masks group rows and the Track Matte column.
add_executable(bloom_timeline_mask_test timeline_mask_tests.cpp)
target_include_directories(bloom_timeline_mask_test PRIVATE ..)
target_link_libraries(bloom_timeline_mask_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_timeline_mask_test)
bloom_add_test(NAME bloom.ui.timeline-masks COMMAND bloom_timeline_mask_test LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ADR 0028: content <-> composition mapping, primitive mask paths and outline geometry.
add_executable(bloom_layer_mask_support_test layer_mask_support_tests.cpp)
target_include_directories(bloom_layer_mask_support_test PRIVATE ..)
target_link_libraries(bloom_layer_mask_support_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_layer_mask_support_test)
bloom_add_test(NAME bloom.ui.layer-mask-support COMMAND bloom_layer_mask_support_test LABELS ui unit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Shared post-confirmation path editing for masks and Pen shape paths: handle modes, conversions,
# Auto recomputation, the shape-preserving split, deletion minimums, closing and reversing.
add_executable(bloom_path_editing_test path_editing_tests.cpp)
target_link_libraries(bloom_path_editing_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_path_editing_test)
bloom_add_test(NAME bloom.ui.path-editing COMMAND bloom_path_editing_test LABELS ui unit TIMEOUT 60)

# #376 Composition Settings: the shared create/settings modal, presets, timecode labels and the one
# command each mode commits. BLOOM_SCREENSHOT_DIR also writes review screenshots.
add_executable(bloom_composition_settings_dialog_test composition_settings_dialog_tests.cpp)
target_include_directories(bloom_composition_settings_dialog_test PRIVATE ..)
target_link_libraries(bloom_composition_settings_dialog_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_composition_settings_dialog_test)
bloom_add_test(NAME bloom.ui.composition-settings COMMAND bloom_composition_settings_dialog_test
               LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ADR 0031: the delivered preview frame's layer bounds equal the headless CPU reference query's.
add_executable(bloom_layer_alignment_parity_test layer_alignment_parity_tests.cpp)
target_link_libraries(bloom_layer_alignment_parity_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_layer_alignment_parity_test)
bloom_add_test(NAME bloom.ui.layer-alignment-parity COMMAND bloom_layer_alignment_parity_test
               LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The MCP Server panel (ADR 0034, section 7) over a scripted port.
add_executable(bloom_mcp_server_panel_test mcp_server_panel_tests.cpp)
target_include_directories(bloom_mcp_server_panel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_mcp_server_panel_test PRIVATE bloom_ui bloom_mcp_testing bloom_runtime_compiler Qt6::Test)
bloom_enable_warnings(bloom_mcp_server_panel_test)
bloom_add_test(
    NAME bloom.ui.mcp-server-panel
    COMMAND bloom_mcp_server_panel_test
    LABELS ui unit
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
)

# Compile-failure details: status bar disclosure, popup, Select, Copy details; DPR captures.
add_executable(bloom_compile_failure_test compile_failure_tests.cpp)
target_link_libraries(bloom_compile_failure_test PRIVATE bloom_ui bloom_runtime_compiler bloom_runtime Qt6::Test)
bloom_enable_warnings(bloom_compile_failure_test)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.compile_failure.dpr${scale} COMMAND bloom_compile_failure_test)
    set_tests_properties(bloom.ui.compile_failure.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;unit" TIMEOUT 120)
endforeach()

# #418: hovering the status bar cells and the kit's tool-tip labels again and again keeps memory
# bounded; the tool tip never becomes its own style-sheet parent.
add_executable(bloom_tooltip_memory_test tooltip_memory_tests.cpp)
target_include_directories(bloom_tooltip_memory_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_tooltip_memory_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_tooltip_memory_test)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.tooltip_memory.dpr${scale} COMMAND bloom_tooltip_memory_test)
    set_tests_properties(bloom.ui.tooltip_memory.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;unit" TIMEOUT 120)
endforeach()

add_executable(bloom_mcp_panel_capture mcp_panel_capture.cpp)
target_include_directories(bloom_mcp_panel_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_mcp_panel_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_mcp_panel_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_mcp_panel_capture)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.mcp_panel_capture.dpr${scale} COMMAND bloom_mcp_panel_capture)
    set_tests_properties(bloom.ui.mcp_panel_capture.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 120)
endforeach()

# The Profiler panel (#423): registration and glyph, feed subscription with a refcount, fixed
# history rings, absent versus zero, stale ages, unattributed math, the trivially copyable snapshot.
add_executable(bloom_profiler_panel_test profiler_panel_tests.cpp)
target_link_libraries(bloom_profiler_panel_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_profiler_panel_test)
bloom_add_test(
    NAME bloom.ui.profiler-panel
    COMMAND bloom_profiler_panel_test
    LABELS ui unit
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
)

# Profiler captures over the fake feed at DPR 1 and 1.5; BLOOM_PROFILER_CAPTURE_DIR keeps them.
add_executable(bloom_profiler_panel_capture profiler_panel_capture.cpp)
target_link_libraries(bloom_profiler_panel_capture PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_profiler_panel_capture)
foreach(scale IN ITEMS 1 1.5)
    add_test(NAME bloom.ui.profiler_panel_capture.dpr${scale} COMMAND bloom_profiler_panel_capture)
    set_tests_properties(bloom.ui.profiler_panel_capture.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 120)
endforeach()

# #536: secondary workspace windows, tear-off, pinned Viewers and Fit without padding.
add_executable(bloom_workspace_window_test workspace_window_tests.cpp)
target_include_directories(bloom_workspace_window_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_workspace_window_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_workspace_window_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_workspace_window_test)
bloom_add_test(
    NAME bloom.ui.workspace_window
    COMMAND bloom_workspace_window_test
    LABELS ui integration
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1"
)

# The names and settings store Bloom starts with, and the package check's own profile.
add_executable(bloom_application_identity_test application_identity_tests.cpp)
target_link_libraries(bloom_application_identity_test PRIVATE bloom_ui Qt6::Core)
bloom_enable_warnings(bloom_application_identity_test)
bloom_add_test(NAME bloom.ui.application-identity COMMAND bloom_application_identity_test LABELS ui unit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# #489: the font picker and field, and the Qt network transport against a loopback server.
add_executable(bloom_font_picker_test font_picker_tests.cpp)
target_include_directories(bloom_font_picker_test PRIVATE ..)
target_link_libraries(bloom_font_picker_test PRIVATE bloom_ui bloom_runtime_compiler bloom_platform Qt6::Test Qt6::Network)
bloom_enable_warnings(bloom_font_picker_test)
bloom_add_test(NAME bloom.ui.font-picker COMMAND bloom_font_picker_test LABELS ui integration kit TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# #513 Stabilize: its card and the tracking job end to end through the controller.
add_executable(bloom_stabilize_test stabilize_tests.cpp)
target_include_directories(bloom_stabilize_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_stabilize_test PRIVATE bloom_ui bloom_commands)
bloom_enable_warnings(bloom_stabilize_test)
bloom_add_test(NAME bloom.ui.stabilize COMMAND bloom_stabilize_test LABELS ui integration TIMEOUT 180 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Issue #637: Resample Path (a keyed Path node feeding it, feeding a Path Shape Source) through the
# Viewer, a cold and warm RAM preview at Half, and the still, range and movie export routes. The CPU
# half always runs; the GPU half runs with a Vulkan loader and a qualified device and prints NotRun
# otherwise (--require-device fails instead).
add_executable(bloom_ui_viewer_export_resample_path_test viewer_export_resample_path_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_resample_path_test PRIVATE bloom_ui bloom_host)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_ui_viewer_export_resample_path_test bloom-media-worker)
endif()
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_resample_path_test
        PRIVATE BLOOM_UI_RESAMPLE_ROUTES_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_resample_path_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_resample_path_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_resample_path
    COMMAND bloom_ui_viewer_export_resample_path_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #597: Shape Morph and the whole Shape animation group (Line ends, Resample Path, Warp Path and
# Shape Morph in one composition) through the Viewer, a cold and warm RAM preview at Half, and the
# still, range and movie export routes. The CPU half always runs; the GPU half runs with a Vulkan
# loader and a qualified device and prints NotRun otherwise (--require-device fails instead).
add_executable(bloom_ui_viewer_export_shape_morph_test viewer_export_shape_morph_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_shape_morph_test PRIVATE bloom_ui bloom_host)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_ui_viewer_export_shape_morph_test bloom-media-worker)
endif()
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_shape_morph_test
        PRIVATE BLOOM_UI_MORPH_ROUTES_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_shape_morph_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_shape_morph_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_shape_morph
    COMMAND bloom_ui_viewer_export_shape_morph_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Issue #598: Warp Path (a keyed Path node feeding it, feeding a Path Shape Source) through the
# Viewer, a cold and warm RAM preview at Half, and the still, range and movie export routes. The CPU
# half always runs; the GPU half runs with a Vulkan loader and a qualified device and prints NotRun
# otherwise (--require-device fails instead).
add_executable(bloom_ui_viewer_export_warp_path_test viewer_export_warp_path_tests.cpp)
target_link_libraries(bloom_ui_viewer_export_warp_path_test PRIVATE bloom_ui bloom_host)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_ui_viewer_export_warp_path_test bloom-media-worker)
endif()
if(BLOOM_UI_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_ui_viewer_export_warp_path_test
        PRIVATE BLOOM_UI_WARP_ROUTES_GPU=1)
    bloom_package_gpu_shader_tools(bloom_ui_viewer_export_warp_path_test)
endif()
bloom_enable_warnings(bloom_ui_viewer_export_warp_path_test)
bloom_add_test(
    NAME bloom.ui.viewer_export_warp_path
    COMMAND bloom_ui_viewer_export_warp_path_test ${bloom_ui_background_parity_loader_args}
    LABELS ui integration
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

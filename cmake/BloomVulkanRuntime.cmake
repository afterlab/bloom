# The Vulkan runtime Bloom packages, per docs/architecture/gpu-backend.md.
#
# BLOOM_VULKAN_LOADER_FILE_NAME is the loader's file name on the target platform.
#
# bloom_prefix_vulkan_loader(<out>) returns the loader in the qualified dependency prefix, or an
# empty string when the build is not qualified or the prefix has none. Tests pass it as --loader.
#
# bloom_package_vulkan_runtime(<target>) packages the runtime with an executable on macOS, where
# there is no system Vulkan: the locked loader, the locked MoltenVK driver and a driver manifest
# that names the driver relative to itself. The layouts are the two that
# src/render/vulkan/gpu_packaged_runtime.hpp looks for:
#
#   an executable         <dir>/bloom-vulkan/{libvulkan.1.dylib,libMoltenVK.dylib,MoltenVK_icd.json}
#   an application bundle Contents/Frameworks/{libvulkan.1.dylib,libMoltenVK.dylib}
#                         Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
#
# bloom_stage_test_vulkan_runtime(<target>) gives a test executable the same runtime the same way:
# its directory gets a `bloom-vulkan` link to one copy staged in the build tree, so a test reaches
# a device through the lookup a packaged executable uses, with nothing set in the environment.
# bloom_add_test calls it for every test program.
#
# Only the qualified prefix supplies these files; a developer-system or CPU-stub build packages
# nothing and the target renders on the CPU reference path. When it packages, the target gets the
# compile definition BLOOM_PACKAGED_VULKAN_RUNTIME=1. On other platforms the function does
# nothing: Linux and Windows use the system's driver, and the Linux application's loader is
# packaged where it always was, in apps/bloom/CMakeLists.txt.

include_guard(GLOBAL)

if(WIN32)
    set(BLOOM_VULKAN_LOADER_FILE_NAME "vulkan-1.dll")
elseif(APPLE)
    set(BLOOM_VULKAN_LOADER_FILE_NAME "libvulkan.1.dylib")
else()
    set(BLOOM_VULKAN_LOADER_FILE_NAME "libvulkan.so.1")
endif()

function(bloom_prefix_vulkan_loader output)
    set(${output} "" PARENT_SCOPE)
    if(NOT DEFINED BLOOM_DEPENDENCY_MODE OR NOT BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        return()
    endif()
    set(bloom_loader "${BLOOM_DEPENDENCY_PREFIX}/lib/${BLOOM_VULKAN_LOADER_FILE_NAME}")
    if(EXISTS "${bloom_loader}")
        set(${output} "${bloom_loader}" PARENT_SCOPE)
    endif()
endfunction()

# The loader, driver and manifest in the qualified prefix, as real files; all empty when the build
# packages no runtime (not macOS, no Vulkan backend, not qualified, or a file is missing).
function(_bloom_vulkan_runtime_files out_loader out_driver out_manifest)
    set(${out_loader} "" PARENT_SCOPE)
    set(${out_driver} "" PARENT_SCOPE)
    set(${out_manifest} "" PARENT_SCOPE)
    if(NOT APPLE OR NOT BLOOM_RENDER_ENABLE_VULKAN)
        return()
    endif()
    bloom_prefix_vulkan_loader(bloom_loader)
    set(bloom_driver "${BLOOM_DEPENDENCY_PREFIX}/lib/libMoltenVK.dylib")
    set(bloom_manifest "${BLOOM_DEPENDENCY_PREFIX}/share/vulkan/icd.d/MoltenVK_icd.json")
    if(NOT bloom_loader OR NOT EXISTS "${bloom_driver}" OR NOT EXISTS "${bloom_manifest}")
        return()
    endif()
    # The prefix names are links to the versioned files; package the files themselves.
    file(REAL_PATH "${bloom_loader}" bloom_loader)
    file(REAL_PATH "${bloom_driver}" bloom_driver)
    set(${out_loader} "${bloom_loader}" PARENT_SCOPE)
    set(${out_driver} "${bloom_driver}" PARENT_SCOPE)
    set(${out_manifest} "${bloom_manifest}" PARENT_SCOPE)
endfunction()

# Writes the prefix's manifest to <output> with only the driver path changed.
function(_bloom_write_vulkan_manifest manifest driver_path output)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
    file(READ "${manifest}" bloom_manifest_json)
    string(JSON bloom_manifest_json SET "${bloom_manifest_json}" "ICD" "library_path"
        "\"${driver_path}\"")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${bloom_manifest_json}\n" @ONLY)
endfunction()

function(bloom_package_vulkan_runtime target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "bloom_package_vulkan_runtime: unknown target '${target}'")
    endif()
    if(NOT APPLE OR NOT BLOOM_RENDER_ENABLE_VULKAN)
        return()
    endif()
    _bloom_vulkan_runtime_files(bloom_loader bloom_driver bloom_manifest)
    if(NOT bloom_loader)
        message(STATUS "Bloom Vulkan runtime: '${target}' packages none (no qualified loader, "
            "driver and manifest); it renders on the CPU reference path.")
        return()
    endif()

    get_target_property(bloom_is_bundle ${target} MACOSX_BUNDLE)
    if(bloom_is_bundle)
        set(bloom_library_stage "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks")
        set(bloom_manifest_stage "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/vulkan/icd.d")
        set(bloom_manifest_to_driver "../../../Frameworks/libMoltenVK.dylib")
        set(bloom_library_install "$<TARGET_BUNDLE_DIR_NAME:${target}>/Contents/Frameworks")
        set(bloom_manifest_install
            "$<TARGET_BUNDLE_DIR_NAME:${target}>/Contents/Resources/vulkan/icd.d")
    else()
        set(bloom_library_stage "$<TARGET_FILE_DIR:${target}>/bloom-vulkan")
        set(bloom_manifest_stage "${bloom_library_stage}")
        # A path with a directory part is read relative to the manifest; a bare file name would
        # be searched for like any library.
        set(bloom_manifest_to_driver "./libMoltenVK.dylib")
        set(bloom_library_install "bin/bloom-vulkan")
        set(bloom_manifest_install "bin/bloom-vulkan")
    endif()

    set(bloom_generated "${CMAKE_CURRENT_BINARY_DIR}/${target}-vulkan-runtime")
    file(MAKE_DIRECTORY "${bloom_generated}")
    _bloom_write_vulkan_manifest("${bloom_manifest}" "${bloom_manifest_to_driver}"
        "${bloom_generated}/MoltenVK_icd.json")

    # Always-run refresh, like the shader tools: a changed prefix file is copied again even when
    # the executable does not relink, and an unchanged one is left alone.
    add_custom_target(${target}-vulkan-runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${bloom_library_stage}"
            "${bloom_manifest_stage}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_loader}"
            "${bloom_library_stage}/${BLOOM_VULKAN_LOADER_FILE_NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_driver}"
            "${bloom_library_stage}/libMoltenVK.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_generated}/MoltenVK_icd.json"
            "${bloom_manifest_stage}/MoltenVK_icd.json"
        COMMENT "Refreshing the Vulkan loader and driver packaged with ${target}"
        VERBATIM)
    add_dependencies(${target} ${target}-vulkan-runtime)

    install(FILES "${bloom_loader}" DESTINATION "${bloom_library_install}"
        RENAME "${BLOOM_VULKAN_LOADER_FILE_NAME}")
    install(FILES "${bloom_driver}" DESTINATION "${bloom_library_install}"
        RENAME "libMoltenVK.dylib")
    install(FILES "${bloom_generated}/MoltenVK_icd.json" DESTINATION "${bloom_manifest_install}")

    target_compile_definitions(${target} PRIVATE BLOOM_PACKAGED_VULKAN_RUNTIME=1)
    set_property(TARGET ${target} PROPERTY BLOOM_VULKAN_RUNTIME_PACKAGED TRUE)
    message(STATUS "Bloom Vulkan runtime: '${target}' packages the locked loader and MoltenVK")
endfunction()

function(bloom_stage_test_vulkan_runtime target)
    if(NOT APPLE OR NOT BLOOM_RENDER_ENABLE_VULKAN OR NOT TARGET "${target}")
        return()
    endif()
    get_target_property(bloom_type ${target} TYPE)
    get_target_property(bloom_imported ${target} IMPORTED)
    get_target_property(bloom_packaged ${target} BLOOM_VULKAN_RUNTIME_PACKAGED)
    if(NOT bloom_type STREQUAL "EXECUTABLE" OR bloom_imported OR bloom_packaged)
        return()
    endif()
    _bloom_vulkan_runtime_files(bloom_loader bloom_driver bloom_manifest)
    if(NOT bloom_loader)
        return()
    endif()

    # One staged copy for the whole build tree.
    set(bloom_stage "${PROJECT_BINARY_DIR}/bloom-test-vulkan-runtime")
    if(NOT TARGET bloom-test-vulkan-runtime)
        file(MAKE_DIRECTORY "${bloom_stage}")
        _bloom_write_vulkan_manifest("${bloom_manifest}" "./libMoltenVK.dylib"
            "${PROJECT_BINARY_DIR}/bloom-test-vulkan-runtime.manifest.json")
        add_custom_target(bloom-test-vulkan-runtime ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_loader}"
                "${bloom_stage}/${BLOOM_VULKAN_LOADER_FILE_NAME}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_driver}"
                "${bloom_stage}/libMoltenVK.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_BINARY_DIR}/bloom-test-vulkan-runtime.manifest.json"
                "${bloom_stage}/MoltenVK_icd.json"
            COMMENT "Refreshing the Vulkan loader and driver staged for tests"
            VERBATIM)
    endif()
    add_dependencies(${target} bloom-test-vulkan-runtime)

    get_target_property(bloom_directory ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT bloom_directory)
        get_target_property(bloom_directory ${target} BINARY_DIR)
    endif()
    set(bloom_link "${bloom_directory}/bloom-vulkan")
    if(NOT EXISTS "${bloom_link}" AND NOT IS_SYMLINK "${bloom_link}")
        file(MAKE_DIRECTORY "${bloom_directory}")
        file(CREATE_LINK "${bloom_stage}" "${bloom_link}" SYMBOLIC)
    endif()
endfunction()

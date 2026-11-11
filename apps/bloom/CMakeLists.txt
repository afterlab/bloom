qt_add_executable(bloom
    main.cpp
)

target_link_libraries(bloom PRIVATE bloom_runtime bloom_runtime_compiler bloom_ui)
bloom_enable_warnings(bloom)

set_target_properties(
    bloom
    PROPERTIES
        OUTPUT_NAME Bloom
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
)

if(APPLE)
    # The bundle's identity (issue #661). The minimum system version is the deployment target the
    # binaries were built for: the dependency lock's in qualified mode. A developer build without
    # one runs on the build machine's system and later, so it states that.
    set(BLOOM_MACOS_MINIMUM_SYSTEM_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT BLOOM_MACOS_MINIMUM_SYSTEM_VERSION)
        execute_process(COMMAND sw_vers -productVersion
                        OUTPUT_VARIABLE BLOOM_MACOS_MINIMUM_SYSTEM_VERSION
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    target_sources(bloom PRIVATE macos/Bloom.icns)
    set_source_files_properties(macos/Bloom.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(
        bloom
        PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.kinetik.bloom"
            MACOSX_BUNDLE_BUNDLE_NAME "Bloom"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_ICON_FILE "Bloom"
            MACOSX_BUNDLE_COPYRIGHT "Licensed under the Apache License, Version 2.0"
    )
endif()

install(
    TARGETS bloom
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

# Stage the qualified GPU shader tools into the executable-relative private directory (inside the
# .app bundle on macOS) so desktop colour compilation works out of the box; an unqualified or
# CPU-stub build gets a typed unavailable definition and the CPU reference path.
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom)

# --- Bundled compute-only Vulkan loader (Linux native-backend builds only) -----------------------
#
# The device path loads the loader dynamically instead of linking it, so no DT_NEEDED entry makes
# it a hard runtime requirement. Only the validated qualified prefix may supply the bundled loader:
# this imports VulkanLoader from BLOOM_DEPENDENCY_PREFIX with NO_DEFAULT_PATH and never resolves a
# host or developer-workspace loader. When the native backend is built and that exact prefix
# provides one, the loader is copied into the app-relative "bloom-vulkan" directory and installed
# to the same relative place. main.cpp resolves it from the executable's own directory and nothing
# else. macOS, Windows, stub, developer-system, and missing-loader builds get no definition, so
# main.cpp takes the explicit CPU fallback and never sets up a GPU preference.
set(BLOOM_APP_BUNDLED_VULKAN_LOADER OFF)
if(UNIX
   AND NOT APPLE
   AND BLOOM_RENDER_ENABLE_VULKAN
   AND DEFINED BLOOM_DEPENDENCY_MODE
   AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
    find_package(VulkanHeaders QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    find_package(VulkanMemoryAllocator QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}"
        NO_DEFAULT_PATH)
    find_package(VulkanLoader QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    if(TARGET Vulkan::Headers AND TARGET GPUOpen::VulkanMemoryAllocator AND TARGET Vulkan::Loader)
        set(BLOOM_APP_BUNDLED_VULKAN_LOADER ON)
    endif()
endif()

if(BLOOM_APP_BUNDLED_VULKAN_LOADER)
    set(BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR "bloom-vulkan")
    set(BLOOM_BUNDLED_VULKAN_LOADER_NAME "libvulkan.so.1")
    # Always-run refresh (not only POST_BUILD): the bundled loader is re-copied whenever its
    # content changes, even when the application itself does not relink. copy_if_different keeps
    # an unchanged loader from touching the app directory.
    add_custom_target(
        bloom-bundled-vulkan-loader ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:bloom>/${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Vulkan::Loader>"
            "$<TARGET_FILE_DIR:bloom>/${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}/${BLOOM_BUNDLED_VULKAN_LOADER_NAME}"
        DEPENDS Vulkan::Loader
        COMMENT "Refreshing the validated Vulkan loader beside the Bloom executable"
        VERBATIM
    )
    add_dependencies(bloom bloom-bundled-vulkan-loader)
    install(
        FILES "$<TARGET_FILE:Vulkan::Loader>"
        DESTINATION "bin/${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}"
        RENAME "${BLOOM_BUNDLED_VULKAN_LOADER_NAME}"
    )
    target_compile_definitions(
        bloom
        PRIVATE
            BLOOM_BUNDLED_VULKAN_LOADER=1
            BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR="${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}"
            BLOOM_BUNDLED_VULKAN_LOADER_NAME="${BLOOM_BUNDLED_VULKAN_LOADER_NAME}"
    )
endif()

# macOS has no system Vulkan: the locked loader and the MoltenVK driver go inside the bundle
# (Contents/Frameworks) with the driver manifest in Contents/Resources/vulkan/icd.d. main.cpp asks
# the renderer for that loader.
bloom_package_vulkan_runtime(bloom)

if(APPLE)
    # The worker processes inside the bundle, where the application looks for them first
    # (media_workers.cpp, ocio_gpu_program_worker.cpp): the media workers with the FFmpeg
    # worker's private libraries under Contents/libexec, the color worker beside the executable.
    # Always-run, like the shader tools and the Vulkan runtime: a rebuilt worker is copied again
    # even when the application does not relink. A bundle without these would run the workers
    # from the build tree, so a moved or copied bundle would lose media and color processing.
    set(bloom_bundle_media "$<TARGET_BUNDLE_CONTENT_DIR:bloom>/libexec/bloom/media")
    set(bloom_bundle_worker_commands "")
    set(bloom_bundle_worker_targets "")
    get_target_property(bloom_ffmpeg_worker_type bloom-media-worker TYPE)
    if(bloom_ffmpeg_worker_type STREQUAL "EXECUTABLE")
        # The library folder is replaced whole with cp -R, which keeps the versioned names as the
        # links they are; CMake's copy_directory would write every link as another full copy.
        list(APPEND bloom_bundle_worker_commands
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${bloom_bundle_media}/ffmpeg-v1/lib"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${bloom_bundle_media}/ffmpeg-v1"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bloom-media-worker>"
                "${bloom_bundle_media}/ffmpeg-v1/"
            COMMAND cp -R "$<TARGET_FILE_DIR:bloom-media-worker>/lib"
                "${bloom_bundle_media}/ffmpeg-v1/lib")
        list(APPEND bloom_bundle_worker_targets bloom-media-worker)
    endif()
    if(TARGET bloom-media-videotoolbox-worker)
        list(APPEND bloom_bundle_worker_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory "${bloom_bundle_media}/videotoolbox-v1"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:bloom-media-videotoolbox-worker>"
                "${bloom_bundle_media}/videotoolbox-v1/")
        list(APPEND bloom_bundle_worker_targets bloom-media-videotoolbox-worker)
    endif()
    if(TARGET bloom-color-worker)
        list(APPEND bloom_bundle_worker_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bloom-color-worker>"
                "$<TARGET_FILE_DIR:bloom>/")
        list(APPEND bloom_bundle_worker_targets bloom-color-worker)
    endif()
    if(bloom_bundle_worker_commands)
        add_custom_target(
            bloom-bundle-workers ALL
            ${bloom_bundle_worker_commands}
            DEPENDS ${bloom_bundle_worker_targets}
            COMMENT "Refreshing the worker processes inside Bloom.app"
            VERBATIM
        )
        # The bundle directory exists once the application has linked.
        add_dependencies(bloom-bundle-workers bloom)
    endif()

    # The locked interpreter, its standard library and Bloom's Python package inside the bundle
    # (macos/bundle_python.cmake), so scripting starts on a Mac without Python (issue #668). The
    # executable finds the interpreter through the bundle's Frameworks directory.
    set(bloom_bundle_python OFF)
    if(BLOOM_BUILD_PYTHON AND BLOOM_LOCKED_PYTHON_HOME)
        set(bloom_bundle_python ON)
        set_property(TARGET bloom APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
        add_custom_target(
            bloom-bundle-python ALL
            COMMAND ${CMAKE_COMMAND} "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>"
                "-DPYTHON_HOME=${BLOOM_LOCKED_PYTHON_HOME}"
                "-DPACKAGE_DIR=${CMAKE_BINARY_DIR}/python"
                "-DLICENSES=${PROJECT_SOURCE_DIR}/dependencies/licenses"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/macos/bundle_python.cmake"
            DEPENDS _bloom
            COMMENT "Refreshing the Python interpreter inside Bloom.app"
            VERBATIM
        )
        add_dependencies(bloom-bundle-python bloom)
    elseif(BLOOM_BUILD_PYTHON)
        message(STATUS "Bloom.app links the host's Python and does not carry it: a "
            "developer-system build is for this machine only")
    endif()

    # Self-contained .app: copy the Qt frameworks (and plugins) the desktop needs beside the
    # executable, give the finished bundle its ad hoc signature (macos/sign_bundle.cmake; no
    # Developer ID signing, an ad hoc signature is what a local build can carry), then check that
    # nothing in it needs the build machine (macos/verify_bundle.cmake): every library resolves
    # inside the bundle or in the operating system, and nothing was built for a newer system than
    # the bundle declares. macdeployqt reports a library it cannot find and still succeeds, so
    # the check is what makes the target fail. Optional -- it requires Qt's macdeployqt, which is
    # present in a normal Qt install but not in a headless CI checkout that only links Qt -- so
    # the target appears only when found.
    get_target_property(bloom_qmake Qt6::qmake IMPORTED_LOCATION)
    if(bloom_qmake)
        get_filename_component(bloom_qt_bin "${bloom_qmake}" DIRECTORY)
        find_program(BLOOM_MACDEPLOYQT NAMES macdeployqt HINTS "${bloom_qt_bin}")
        if(BLOOM_MACDEPLOYQT)
            add_custom_target(
                bloom-deploy-macos
                COMMAND "${BLOOM_MACDEPLOYQT}" "$<TARGET_BUNDLE_DIR:bloom>" -always-overwrite
                COMMAND ${CMAKE_COMMAND} "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/macos/sign_bundle.cmake"
                COMMAND ${CMAKE_COMMAND} "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>"
                    "-DREQUIRE_PYTHON=${bloom_bundle_python}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/macos/verify_bundle.cmake"
                DEPENDS bloom
                COMMENT "Bundling Qt into Bloom.app, signing it ad hoc and checking it is self-contained"
                VERBATIM
            )
            if(TARGET bloom-bundle-workers)
                add_dependencies(bloom-deploy-macos bloom-bundle-workers)
            endif()
            if(TARGET bloom-bundle-python)
                add_dependencies(bloom-deploy-macos bloom-bundle-python)
            endif()
        endif()
    endif()
endif()

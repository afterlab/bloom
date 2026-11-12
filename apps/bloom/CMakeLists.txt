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

# Where the parts of a package go: cmake/BloomPackageLayout.cmake.
install(
    TARGETS bloom
    BUNDLE DESTINATION .
    RUNTIME DESTINATION "${BLOOM_PACKAGE_PROGRAM_DIR}"
)
bloom_package_search_path(bloom "${BLOOM_PACKAGE_PROGRAM_DIR}")

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
        DESTINATION "${BLOOM_PACKAGE_PROGRAM_DIR}/${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}"
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
    # The command-line programs, beside the application (macos/bundle_tool.cmake): a package
    # is the bundle, and scripts and agents run the same programs on every platform.
    foreach(bloom_bundle_tool IN ITEMS bloom-cli bloom-mcp)
        list(APPEND bloom_bundle_worker_commands
            COMMAND ${CMAKE_COMMAND} "-DTOOL=$<TARGET_FILE:${bloom_bundle_tool}>"
                "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/macos/bundle_tool.cmake")
        list(APPEND bloom_bundle_worker_targets ${bloom_bundle_tool})
    endforeach()
    if(bloom_bundle_worker_commands)
        add_custom_target(
            bloom-bundle-workers ALL
            ${bloom_bundle_worker_commands}
            DEPENDS ${bloom_bundle_worker_targets}
            COMMENT "Refreshing the workers and command-line programs inside Bloom.app"
            VERBATIM
        )
        # The bundle directory exists once the application has linked.
        add_dependencies(bloom-bundle-workers bloom)
    endif()

    # A qualified build fills the bundle with the same scripts that fill the Linux and Windows
    # packages (cmake/package, cmake/BloomPackageLayout.cmake), every time it builds, so the
    # bundle in the build tree is the package: the locked interpreter with its standard library
    # and Bloom's Python package (issue #668), the locked Qt's frameworks and plugins exactly as
    # the Qt Project published and signed them (issue #679), and the license texts, notices and
    # dependency lock. The executable then looks for its libraries only in the bundle's
    # Frameworks directory.
    get_filename_component(bloom_package_scripts "${PROJECT_SOURCE_DIR}/cmake/package" ABSOLUTE)
    set(bloom_bundle_root "$<TARGET_BUNDLE_DIR:bloom>/..")
    set(bloom_bundle_complete OFF)
    set(bloom_bundle_python OFF)
    if(BLOOM_BUILD_PYTHON AND BLOOM_LOCKED_PYTHON_HOME)
        set(bloom_bundle_python ON)
        set_property(TARGET bloom APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
        add_custom_target(
            bloom-bundle-python ALL
            COMMAND ${CMAKE_COMMAND} "-DROOT=${bloom_bundle_root}"
                "-DPYTHON_HOME=${BLOOM_LOCKED_PYTHON_HOME}"
                "-DBLOOM_PYTHON=${CMAKE_BINARY_DIR}/python"
                "-DPYTHON_DIR=${BLOOM_PACKAGE_PYTHON_DIR}"
                "-DBLOOM_PYTHON_DIR=${BLOOM_PACKAGE_BLOOM_PYTHON_DIR}"
                "-DLIBRARY_DIR=${BLOOM_PACKAGE_LIBRARY_DIR}"
                -P "${bloom_package_scripts}/stage_python.cmake"
            DEPENDS _bloom
            COMMENT "Refreshing the Python interpreter inside Bloom.app"
            VERBATIM
        )
        add_dependencies(bloom-bundle-python bloom)
    elseif(BLOOM_BUILD_PYTHON)
        message(STATUS "Bloom.app links the host's Python and does not carry it: a "
            "developer-system build is for this machine only")
    endif()

    set(bloom_bundle_qt OFF)
    if(DEFINED BLOOM_DEPENDENCY_MODE AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified"
       AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/qt/bloom-prebuilt.txt")
        set(bloom_bundle_qt ON)
        # Only the bundle's own Frameworks directory: the frameworks are there in the build tree
        # too, and a path into the dependency prefix would be a path outside the package.
        set_target_properties(bloom PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path/../Frameworks"
            INSTALL_RPATH_USE_LINK_PATH FALSE)
        add_custom_target(
            bloom-bundle-qt ALL
            COMMAND ${CMAKE_COMMAND} "-DROOT=${bloom_bundle_root}"
                "-DQT=${BLOOM_DEPENDENCY_PREFIX}/qt"
                "-DMODULES=${PROJECT_SOURCE_DIR}/dependencies/qt-modules.txt"
                -DOPERATING_SYSTEM=macos
                "-DQT_DIR=${BLOOM_PACKAGE_QT_DIR}"
                "-DLIBRARY_DIR=${BLOOM_PACKAGE_LIBRARY_DIR}"
                "-DPROGRAM_DIR=${BLOOM_PACKAGE_PROGRAM_DIR}"
                -P "${bloom_package_scripts}/stage_qt.cmake"
            COMMENT "Refreshing the locked Qt inside Bloom.app"
            VERBATIM
        )
        add_dependencies(bloom-bundle-qt bloom)
        add_custom_target(
            bloom-bundle-records ALL
            COMMAND ${CMAKE_COMMAND} "-DROOT=${bloom_bundle_root}"
                "-DSOURCE=${PROJECT_SOURCE_DIR}"
                -DOPERATING_SYSTEM=macos
                "-DRECORD_DIR=${BLOOM_PACKAGE_RECORD_DIR}"
                -P "${bloom_package_scripts}/stage_records.cmake"
            COMMENT "Refreshing the license texts and the dependency lock inside Bloom.app"
            VERBATIM
        )
        add_dependencies(bloom-bundle-records bloom)
        if(bloom_bundle_python)
            set(bloom_bundle_complete ON)
        endif()
    endif()

    # bloom-deploy-macos finishes the bundle: it gives it its ad hoc signature
    # (macos/sign_bundle.cmake; no Developer ID signing, an ad hoc signature is what a local
    # build can carry) and then checks that nothing in it needs the build machine
    # (cmake/package/check_package.cmake): every library resolves inside the bundle or in the
    # operating system, and nothing was built for a newer system than the bundle declares.
    #
    # A developer build has no locked Qt to stage: there macdeployqt copies the host's Qt into
    # the bundle first. It reports a library it cannot find and still succeeds, so the check is
    # what makes the target fail. It is present in a normal Qt install but not in every
    # checkout that only links Qt, so in such a build the target appears only when it is found.
    set(bloom_deploy_commands "")
    set(bloom_deploy_available ON)
    if(NOT bloom_bundle_qt)
        get_target_property(bloom_qmake Qt6::qmake IMPORTED_LOCATION)
        set(BLOOM_MACDEPLOYQT "BLOOM_MACDEPLOYQT-NOTFOUND")
        if(bloom_qmake)
            get_filename_component(bloom_qt_bin "${bloom_qmake}" DIRECTORY)
            find_program(BLOOM_MACDEPLOYQT NAMES macdeployqt HINTS "${bloom_qt_bin}")
        endif()
        if(BLOOM_MACDEPLOYQT)
            set(bloom_deploy_commands
                COMMAND "${BLOOM_MACDEPLOYQT}" "$<TARGET_BUNDLE_DIR:bloom>" -always-overwrite)
        else()
            set(bloom_deploy_available OFF)
        endif()
    endif()
    if(bloom_deploy_available)
        add_custom_target(
            bloom-deploy-macos
            ${bloom_deploy_commands}
            COMMAND ${CMAKE_COMMAND} "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/macos/sign_bundle.cmake"
            COMMAND ${CMAKE_COMMAND} "-DPACKAGE=$<TARGET_BUNDLE_DIR:bloom>"
                "-DCOMPLETE=${bloom_bundle_complete}" "-DONLY=contents;resolution"
                -P "${bloom_package_scripts}/check_package.cmake"
            DEPENDS bloom
            COMMENT "Signing Bloom.app ad hoc and checking it is self-contained"
            VERBATIM
        )
        foreach(bloom_bundle_part IN ITEMS bloom-bundle-workers bloom-bundle-python
                                           bloom-bundle-qt bloom-bundle-records)
            if(TARGET ${bloom_bundle_part})
                add_dependencies(bloom-deploy-macos ${bloom_bundle_part})
            endif()
        endforeach()
    endif()
endif()

# The application itself refuses a package-check profile that is not an absolute directory,
# instead of starting under Bloom's own names (src/ui/include/bloom/ui/application_identity.hpp).
if(BUILD_TESTING)
    include(BloomTesting)
    bloom_add_test(
        NAME bloom.ui.application-identity-refusal
        COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:bloom>"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/identity-refusal"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/application_refuses_relative_profile.cmake"
        LABELS ui integration
        TIMEOUT 90)
endif()

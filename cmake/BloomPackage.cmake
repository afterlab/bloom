# Builds a Bloom package from the install rules (docs/architecture/platform-support.md,
# "Packages").
#
# `cmake --install <build> --prefix <directory>` writes the whole package: the programs and
# workers through the targets' own install rules, and then, through the shared scripts in
# cmake/package, the locked Qt (stage_qt.cmake), the carried interpreter (stage_python.cmake) and
# the license texts, notices and dependency lock (stage_records.cmake). The scripts are the same
# for every operating system; where each part goes is cmake/BloomPackageLayout.cmake's. A Linux
# package also carries a few distribution libraries Qt loads (stage_linux_runtime.cmake), a
# Windows package the Visual C++ runtime beside each program (stage_msvc_runtime.cmake).
#
# Only a qualified build with the locked interpreter and the desktop application can be
# packaged: a developer build links the host's Qt and Python, which a package cannot carry. Such
# a build installs its programs and nothing else, and says so.
#
#   bloom-package         installs the package into <build>/package/<name> and checks it
#                         (cmake/package/check_package.cmake, with a run of the packaged programs)
#   bloom-package-archives   that tree in its published form: on Linux a tar and an AppImage
#                         (cmake/package/make_archives.cmake), on Windows a zip
#   bloom.package.check   the same check as a test, with the cases it must reject
#                         (cmake/package/check_package_test.cmake)
#
# macOS: the bundle is filled in the build tree by apps/bloom/CMakeLists.txt with the same
# scripts, so that it can be signed once it is complete; installing copies it.
include_guard(GLOBAL)
include(BloomPackageLayout)

function(bloom_add_package_rules)
    set(reason "")
    if(NOT DEFINED BLOOM_DEPENDENCY_MODE OR NOT BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        set(reason "it is not a qualified build")
    elseif(NOT BLOOM_BUILD_DESKTOP)
        set(reason "it does not build the desktop application")
    elseif(NOT BLOOM_BUILD_PYTHON OR NOT BLOOM_LOCKED_PYTHON_HOME)
        set(reason "it does not embed the locked interpreter")
    elseif(NOT EXISTS "${BLOOM_DEPENDENCY_PREFIX}/qt/bloom-prebuilt.txt")
        set(reason "the dependency prefix has no locked Qt")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux"
           AND NOT EXISTS "${BLOOM_DEPENDENCY_PREFIX}/linux-runtime/bloom-linux-runtime.txt")
        set(reason "the dependency prefix has no Linux runtime libraries")
    endif()
    if(reason)
        message(STATUS "Bloom package: this build cannot be packaged (${reason})")
        return()
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" processor)
    if(processor MATCHES "^(arm64|aarch64)$")
        set(processor "arm64")
    elseif(processor MATCHES "^(x86_64|amd64)$")
        set(processor "x86_64")
    endif()
    set(BLOOM_PACKAGE_NAME
        "Bloom-${BLOOM_PACKAGE_VERSION}-${BLOOM_PACKAGE_OPERATING_SYSTEM}-${processor}"
        CACHE INTERNAL "The name of the package directory and archives")
    # Linux: the oldest system a published package is for, as the newest symbol versions of
    # the C and C++ libraries a file may need (cmake/package/check_package.cmake, FLOOR). Empty
    # on a developer's machine, whose own libraries are newer; the build of a published
    # package sets it (tools/ci/linux-package/build.sh).
    set(BLOOM_PACKAGE_FLOOR "" CACHE STRING
        "Symbol version floors the package check enforces, as <family>=<version>;...")
    set(scripts "${PROJECT_SOURCE_DIR}/cmake/package")

    if(NOT APPLE)
        # After the targets' own rules, so the directories exist. The root is the install
        # prefix of the moment, DESTDIR included.
        install(CODE "
            set(bloom_package_root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
            execute_process(
                COMMAND \"${CMAKE_COMMAND}\" \"-DROOT=\${bloom_package_root}\"
                    \"-DQT=${BLOOM_DEPENDENCY_PREFIX}/qt\"
                    \"-DMODULES=${PROJECT_SOURCE_DIR}/dependencies/qt-modules.txt\"
                    -DOPERATING_SYSTEM=${BLOOM_PACKAGE_OPERATING_SYSTEM}
                    \"-DQT_DIR=${BLOOM_PACKAGE_QT_DIR}\"
                    \"-DLIBRARY_DIR=${BLOOM_PACKAGE_LIBRARY_DIR}\"
                    \"-DPROGRAM_DIR=${BLOOM_PACKAGE_PROGRAM_DIR}\"
                    -P \"${scripts}/stage_qt.cmake\"
                COMMAND_ERROR_IS_FATAL ANY)
            execute_process(
                COMMAND \"${CMAKE_COMMAND}\" \"-DROOT=\${bloom_package_root}\"
                    \"-DPYTHON_HOME=${BLOOM_LOCKED_PYTHON_HOME}\"
                    \"-DBLOOM_PYTHON=${CMAKE_BINARY_DIR}/python\"
                    \"-DPYTHON_DIR=${BLOOM_PACKAGE_PYTHON_DIR}\"
                    \"-DBLOOM_PYTHON_DIR=${BLOOM_PACKAGE_BLOOM_PYTHON_DIR}\"
                    \"-DLIBRARY_DIR=${BLOOM_PACKAGE_LIBRARY_DIR}\"
                    -P \"${scripts}/stage_python.cmake\"
                COMMAND_ERROR_IS_FATAL ANY)
            execute_process(
                COMMAND \"${CMAKE_COMMAND}\" \"-DROOT=\${bloom_package_root}\"
                    \"-DSOURCE=${PROJECT_SOURCE_DIR}\"
                    -DOPERATING_SYSTEM=${BLOOM_PACKAGE_OPERATING_SYSTEM}
                    \"-DRECORD_DIR=${BLOOM_PACKAGE_RECORD_DIR}\"
                    -P \"${scripts}/stage_records.cmake\"
                COMMAND_ERROR_IS_FATAL ANY)
        ")
        if(BLOOM_PACKAGE_OPERATING_SYSTEM STREQUAL "windows")
            # The redistributable Visual C++ runtime of this toolset, named by CMake; the
            # universal C runtime is Windows' own.
            set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
            set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
            set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
            include(InstallRequiredSystemLibraries)
            if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
                message(FATAL_ERROR
                    "Bloom package: the Visual C++ redistributable libraries were not found; a "
                    "Windows package cannot be made without them (VCToolsRedistDir).")
            endif()
            list(JOIN CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "|" bloom_package_msvc_runtime)
            install(CODE "
                execute_process(
                    COMMAND \"${CMAKE_COMMAND}\" \"-DROOT=\${bloom_package_root}\"
                        \"-DRUNTIME=${bloom_package_msvc_runtime}\"
                        \"-DVERSION=MSVC ${CMAKE_CXX_COMPILER_VERSION}\"
                        \"-DPYTHON_DIR=${BLOOM_PACKAGE_PYTHON_DIR}\"
                        \"-DRECORD_DIR=${BLOOM_PACKAGE_RECORD_DIR}\"
                        -P \"${scripts}/stage_msvc_runtime.cmake\"
                    COMMAND_ERROR_IS_FATAL ANY)
            ")
        endif()
        if(BLOOM_PACKAGE_OPERATING_SYSTEM STREQUAL "linux")
            install(CODE "
                execute_process(
                    COMMAND \"${CMAKE_COMMAND}\" \"-DROOT=\${bloom_package_root}\"
                        \"-DRUNTIME=${BLOOM_DEPENDENCY_PREFIX}/linux-runtime\"
                        \"-DINVENTORY=${PROJECT_SOURCE_DIR}/dependencies/linux-runtime/packages.json\"
                        \"-DQT_DIR=${BLOOM_PACKAGE_QT_DIR}\"
                        \"-DRECORD_DIR=${BLOOM_PACKAGE_RECORD_DIR}\"
                        -P \"${scripts}/stage_linux_runtime.cmake\"
                    COMMAND_ERROR_IS_FATAL ANY)
            ")
        endif()

        set(programs bloom bloom-cli bloom-mcp bloom-color-worker)
        if(TARGET bloom-media-worker)
            list(APPEND programs bloom-media-worker)
        endif()
        set(package "${CMAKE_BINARY_DIR}/package/${BLOOM_PACKAGE_NAME}")
        add_custom_target(
            bloom-package
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${package}"
            COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${package}"
            COMMAND ${CMAKE_COMMAND} "-DPACKAGE=${package}" -DCOMPLETE=ON -DRUN=ON
                "-DWORK=${CMAKE_BINARY_DIR}/package/check" "-DFLOOR=${BLOOM_PACKAGE_FLOOR}"
                -P "${scripts}/check_package.cmake"
            DEPENDS ${programs}
            COMMENT "Installing ${BLOOM_PACKAGE_NAME} and checking it is self-contained"
            VERBATIM
        )
        if(BLOOM_PACKAGE_OPERATING_SYSTEM STREQUAL "windows")
            # The published form: the folder as a zip. No installer.
            add_custom_target(
                bloom-package-archives
                COMMAND ${CMAKE_COMMAND} -E rm -f
                    "${CMAKE_BINARY_DIR}/package/${BLOOM_PACKAGE_NAME}.zip"
                COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/package"
                    ${CMAKE_COMMAND} -E tar cf "${BLOOM_PACKAGE_NAME}.zip" --format=zip
                    "${BLOOM_PACKAGE_NAME}"
                DEPENDS bloom-package
                COMMENT "Making ${BLOOM_PACKAGE_NAME}.zip"
                VERBATIM
            )
        endif()
        if(BLOOM_PACKAGE_OPERATING_SYSTEM STREQUAL "linux")
            # The two published forms of that tree. Not part of bloom-package's own work, so
            # the check never waits for the compression.
            add_custom_target(
                bloom-package-archives
                COMMAND ${CMAKE_COMMAND} "-DTREE=${package}"
                    "-DOUTPUT=${CMAKE_BINARY_DIR}/package"
                    "-DWORK=${CMAKE_BINARY_DIR}/package/archive-work"
                    "-DSOURCE=${PROJECT_SOURCE_DIR}"
                    "-DRUNTIME=${BLOOM_DEPENDENCY_PREFIX}/appimage-runtime"
                    -P "${scripts}/make_archives.cmake"
                DEPENDS bloom-package
                COMMENT "Making ${BLOOM_PACKAGE_NAME}.tar.xz and ${BLOOM_PACKAGE_NAME}.AppImage"
                VERBATIM
            )
        endif()
    endif()

    if(BUILD_TESTING)
        include(BloomTesting)
        # Checks the package with a run of the packaged programs, and then breaks a copy of it
        # in each way the check must refuse. The package is installed from the build tree; on
        # macOS it is the bundle the build filled.
        if(APPLE)
            set(source "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>")
        else()
            find_program(BLOOM_PACKAGE_PATCHELF patchelf)
            set(source "-DBUILD=${CMAKE_BINARY_DIR}" "-DPATCHELF=${BLOOM_PACKAGE_PATCHELF}")
        endif()
        bloom_add_test(
            NAME bloom.package.check
            COMMAND ${CMAKE_COMMAND} ${source} "-DWORK=${CMAKE_BINARY_DIR}/package-test"
                -P "${scripts}/check_package_test.cmake"
            LABELS package integration
            TIMEOUT 1200)
    endif()
    message(STATUS "Bloom package: ${BLOOM_PACKAGE_NAME}")
endfunction()

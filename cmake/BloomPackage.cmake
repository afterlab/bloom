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
#                         (cmake/package/make_archives.cmake), on Windows a zip and, when the
#                         dependency prefix has the Inno Setup compiler, the installer
#                         (cmake/package/make_windows_installer.cmake, packaging/windows/bloom.iss)
#   bloom.package.check   the same check as a test, with the cases it must reject
#                         (cmake/package/check_package_test.cmake)
#   bloom.package.archive_update_information   (Linux) the AppImage's update information and
#                         control file, on a stand-in tree (cmake/package/make_archives_test.cmake)
#   bloom.package.installers   (Windows) installs, upgrades and uninstalls the installer under a
#                         test identity (cmake/package/check_windows_installer.cmake)
#   bloom.package.installers   (macOS) makes the disk image and the installer package from a
#                         signed copy of the bundle and looks inside both, without installing
#                         anything (cmake/package/check_macos_installers.cmake)
#
# macOS: the bundle is filled in the build tree by apps/bloom/CMakeLists.txt with the same
# scripts, so that it can be signed once it is complete; installing copies it. Its
# bloom-package-archives makes the zip, the disk image and the installer package
# (cmake/package/make_macos_installers.cmake, packaging/macos/postinstall).
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
            # The platform's updater (dependencies/update-runtime), when the dependency prefix
            # has it: WinSparkle.dll beside the programs, which Bloom loads from there when an
            # installed copy shows an update, and its license files.
            if(EXISTS "${BLOOM_DEPENDENCY_PREFIX}/winsparkle/x64/Release/WinSparkle.dll")
                install(CODE "
                    execute_process(
                        COMMAND \"${CMAKE_COMMAND}\" \"-DROOT=\${bloom_package_root}\"
                            \"-DRUNTIME=${BLOOM_DEPENDENCY_PREFIX}/winsparkle\"
                            \"-DRECORD=${PROJECT_SOURCE_DIR}/dependencies/update-runtime/winsparkle.json\"
                            \"-DSOURCE=${PROJECT_SOURCE_DIR}\"
                            -DOPERATING_SYSTEM=windows
                            \"-DLIBRARY_DIR=${BLOOM_PACKAGE_LIBRARY_DIR}\"
                            \"-DRECORD_DIR=${BLOOM_PACKAGE_RECORD_DIR}\"
                            -P \"${scripts}/stage_update_runtime.cmake\"
                        COMMAND_ERROR_IS_FATAL ANY)
                ")
            else()
                message(STATUS "Bloom package: no platform updater (the dependency prefix has no "
                               "WinSparkle); an installed copy says where the next version is")
            endif()
            # The program of the `cli` folder, which the installer's PATH option adds to the
            # user's PATH (apps/bloom-cli-launcher/main.cpp). Installed after the runtime is
            # staged, so that no runtime library is copied into `cli` beside it: that folder
            # holds this one file.
            install(TARGETS bloom-cli-launcher RUNTIME DESTINATION "cli")
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

        set(programs bloom bloom-cli bloom-mcp bloom-color-worker bloom-update-helper)
        if(BLOOM_PACKAGE_OPERATING_SYSTEM STREQUAL "windows")
            list(APPEND programs bloom-cli-launcher)
        endif()
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
            # The published forms: the folder as a zip and, with the Inno Setup compiler in the
            # dependency prefix, the installer beside it. A developer build has no compiler and
            # makes the zip only.
            set(bloom_package_iscc "${BLOOM_DEPENDENCY_PREFIX}/innosetup/ISCC.exe")
            set(bloom_package_installer_command "")
            set(bloom_package_installer_comment "")
            if(EXISTS "${bloom_package_iscc}")
                set(bloom_package_installer_command
                    COMMAND ${CMAKE_COMMAND} "-DISCC=${bloom_package_iscc}"
                        "-DPACKAGE=${package}" "-DSOURCE=${PROJECT_SOURCE_DIR}"
                        "-DVERSION=${BLOOM_PACKAGE_VERSION}"
                        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/package"
                        "-DOUTPUT_NAME=${BLOOM_PACKAGE_NAME}-setup"
                        -P "${scripts}/make_windows_installer.cmake")
                set(bloom_package_installer_comment " and ${BLOOM_PACKAGE_NAME}-setup.exe")
            else()
                message(STATUS "Bloom package: the Windows installer is not made (the "
                                "dependency prefix has no Inno Setup compiler); a developer "
                                "build makes the zip only")
            endif()
            add_custom_target(
                bloom-package-archives
                COMMAND ${CMAKE_COMMAND} -E rm -f
                    "${CMAKE_BINARY_DIR}/package/${BLOOM_PACKAGE_NAME}.zip"
                COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/package"
                    ${CMAKE_COMMAND} -E tar cf "${BLOOM_PACKAGE_NAME}.zip" --format=zip
                    "${BLOOM_PACKAGE_NAME}"
                ${bloom_package_installer_command}
                DEPENDS bloom-package
                COMMENT "Making ${BLOOM_PACKAGE_NAME}.zip${bloom_package_installer_comment}"
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
                    "-DUPDATE_INFORMATION=${BLOOM_UPDATE_APPIMAGE_INFORMATION}"
                    "-DZSYNC_MAKE=$<TARGET_FILE:bloom-zsync-make>"
                    -P "${scripts}/make_archives.cmake"
                DEPENDS bloom-package bloom-zsync-make
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
            set(source "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>" "-DVERSION=${BLOOM_PACKAGE_VERSION}")
        else()
            find_program(BLOOM_PACKAGE_PATCHELF patchelf)
            set(source "-DBUILD=${CMAKE_BINARY_DIR}" "-DPATCHELF=${BLOOM_PACKAGE_PATCHELF}")
        endif()
        # A build with the Vulkan backend also requires the packaged application to show a
        # Viewer frame evaluated on the GPU (check_package.cmake, rule `gpu`): the test then
        # needs a computer with a GPU, like the other tests that require a device.
        set(gpu "")
        set(labels package integration)
        if(BLOOM_RENDER_ENABLE_VULKAN)
            set(gpu "-DGPU=ON")
            list(APPEND labels gpu)
        endif()
        bloom_add_test(
            NAME bloom.package.check
            COMMAND ${CMAKE_COMMAND} ${source} ${gpu} "-DWORK=${CMAKE_BINARY_DIR}/package-test"
                -P "${scripts}/check_package_test.cmake"
            LABELS ${labels}
            TIMEOUT 1800)
        if(BLOOM_PACKAGE_OPERATING_SYSTEM STREQUAL "linux")
            # Linux only: the AppImage is a Linux form of the package, and its runtime is a
            # Linux program. The tree is a stand-in of a few files; without readelf or the
            # runtime the script says "SKIPPED:".
            bloom_add_test(
                NAME bloom.package.archive_update_information
                COMMAND ${CMAKE_COMMAND} "-DSOURCE=${PROJECT_SOURCE_DIR}"
                    "-DRUNTIME=${BLOOM_DEPENDENCY_PREFIX}/appimage-runtime"
                    "-DZSYNC_MAKE=$<TARGET_FILE:bloom-zsync-make>"
                    "-DWORK=${CMAKE_BINARY_DIR}/package-archive-test"
                    -P "${scripts}/make_archives_test.cmake"
                LABELS package
                TIMEOUT 120)
            set_tests_properties(bloom.package.archive_update_information PROPERTIES
                SKIP_REGULAR_EXPRESSION "SKIPPED:")
        endif()
        if(BLOOM_PACKAGE_OPERATING_SYSTEM STREQUAL "windows")
            # Windows only: the installer is a Windows program. macOS registers its own test
            # of the same name for the disk image, and a Linux package has no installer file.
            # Without the Inno Setup compiler the script says "SKIPPED:", which ctest reports
            # as skipped (a cmake script cannot end with a skip return code).
            bloom_add_test(
                NAME bloom.package.installers
                COMMAND ${CMAKE_COMMAND} "-DBUILD=${CMAKE_BINARY_DIR}"
                    "-DISCC=${BLOOM_DEPENDENCY_PREFIX}/innosetup/ISCC.exe"
                    "-DSOURCE=${PROJECT_SOURCE_DIR}" "-DVERSION=${BLOOM_PACKAGE_VERSION}"
                    "-DWORK=${CMAKE_BINARY_DIR}/package-installer-test"
                    -P "${scripts}/check_windows_installer.cmake"
                LABELS package integration
                RUN_SERIAL
                TIMEOUT 1800)
            set_tests_properties(bloom.package.installers PROPERTIES
                SKIP_REGULAR_EXPRESSION "SKIPPED:")
        endif()
        if(APPLE)
            # macOS only: the disk image and the installer package are made and read by macOS
            # programs (hdiutil, pkgbuild, pkgutil, tiffutil, codesign). Same name as the Windows
            # test: one platform registers one. It makes both installers from a signed copy of
            # the built bundle and never runs `installer -pkg`, which needs an administrator.
            # The disk image's window layout is written and read by tools/release/dmg_layout.py.
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            bloom_add_test(
                NAME bloom.package.installers
                COMMAND ${CMAKE_COMMAND} "-DBUNDLE=$<TARGET_BUNDLE_DIR:bloom>"
                    "-DSOURCE=${PROJECT_SOURCE_DIR}"
                    "-DWORK=${CMAKE_BINARY_DIR}/package-installer-test"
                    "-DPYTHON=${Python3_EXECUTABLE}"
                    -P "${scripts}/check_macos_installers.cmake"
                LABELS package integration
                RUN_SERIAL
                TIMEOUT 1800)
            set_tests_properties(bloom.package.installers PROPERTIES
                SKIP_REGULAR_EXPRESSION "SKIPPED:")
        endif()
    endif()
    message(STATUS "Bloom package: ${BLOOM_PACKAGE_NAME}")
endfunction()

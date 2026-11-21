cmake_minimum_required(VERSION 3.25)

# macOS: the deployment target comes from the dependency lock and has to be known before the first
# project() call examines the toolchain.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/BloomAppleDeploymentTarget.cmake")
bloom_apply_apple_deployment_target()

project(
    Bloom
    VERSION 0.1.0
    DESCRIPTION "Artist-focused motion graphics and visual effects software"
    LANGUAGES CXX C
)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Sources are UTF-8 and use the standard __cplusplus value; the Windows headers must not define
    # the min/max macros (std::numeric_limits<T>::max() is written throughout), and the C runtime's
    # getenv/strerror family is used deliberately.
    add_compile_options(/utf-8 /Zc:__cplusplus)
    # Executables get the 8 MiB main-thread stack Linux and macOS give by default; the Windows
    # default of 1 MiB overflows in the application and in the GPU and scripting test programs
    # (exit 0xC00000FD).
    add_link_options("$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:/STACK:8388608>")
    # Every executable carries cmake/windows/bloom.manifest: a UTF-8 narrow-string code page, so
    # file names outside ASCII survive every std::string path in Bloom and in the libraries it
    # links, and long-path awareness.
    add_link_options(
        "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:/MANIFEST:EMBED>"
        "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/bloom.manifest>")
    # NOMINMAX and WIN32_LEAN_AND_MEAN are defined empty, the value several translation units
    # define them with, so those definitions are identical redefinitions.
    add_compile_definitions(NOMINMAX= WIN32_LEAN_AND_MEAN= _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()

# The macOS media worker's VideoToolbox/AVFoundation provider is Objective-C++ (.mm). Enable the
# language only where it exists; every other target stays pure C++.
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

option(BLOOM_BUILD_DESKTOP "Build the Qt desktop application and UI tests" ON)
# Throwaway measurement programs under tools/spikes. OFF by default: a spike is evidence for a
# design decision, never a shipped target, so the product build must not compile or link one.
option(BLOOM_BUILD_SPIKES "Build the throwaway design spikes under tools/spikes" OFF)
# The label of the release this source tree builds: the version in the names of the package
# files, and the version the application, bloom-cli and bloom-mcp report (bloom::core::version()).
# project(VERSION) stays the numeric part. A build of another label passes its own.
set(BLOOM_PACKAGE_VERSION "0.1.0-alpha.2" CACHE STRING
    "The version the programs report and the package files carry")
if(NOT BLOOM_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-+][0-9A-Za-z.]+)?$")
    message(FATAL_ERROR
        "BLOOM_PACKAGE_VERSION '${BLOOM_PACKAGE_VERSION}' is not a version such as 0.1.0 or "
        "0.1.0-alpha.1")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(BloomQuality)
include(BloomWarnings)
include(BloomDependencyPrefix)
include(BloomVulkanRuntime)
include(BloomPackageLayout)
include(BloomVersion)
include(BloomPathDefinition)
# Before any target is added: a release build's debug information reaches all of them.
include(BloomDebugSymbols)

# Before any target is added, so it reaches all of them.
bloom_disable_floating_point_contraction()

# The prefix reaches compile definitions and generated files, so it is spelled the way CMake spells
# paths (forward slashes) before anything reads it; a hosted Windows runner passes backslashes
# (cmake/BloomPathDefinition.cmake).
if(DEFINED BLOOM_DEPENDENCY_PREFIX)
    bloom_cmake_path(BLOOM_DEPENDENCY_PREFIX "${BLOOM_DEPENDENCY_PREFIX}")
endif()

# Dependency-prefix consumption is implemented, so an explicit mode is required: 'qualified'
# validates the reviewed lock and resolves dependency packages only from the superbuild prefix;
# 'developer-system' iterates against host packages and is labeled Unqualified. CI, release,
# package, and conformance builds use qualified mode.
bloom_consume_dependency_prefix()

# A qualified build takes CPython from the dependency prefix, where the superbuild puts the locked
# interpreter that every package carries (issue #677): every part of the build (the embedded
# runtime, the extension module, the tests that run scripts) must agree on it.
#
# macOS has no other interpreter to take, so a qualified macOS build without it refuses to
# configure. A qualified Linux or Windows build against a prefix that was built before the
# interpreter was locked still configures, with the host's CPython, and says so; such a build
# cannot be packaged (the package rules require the carried interpreter).
if(BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
    if(WIN32)
        set(bloom_locked_python_program "${BLOOM_DEPENDENCY_PREFIX}/python/python.exe")
    else()
        set(bloom_locked_python_program "${BLOOM_DEPENDENCY_PREFIX}/python/bin/python3")
    endif()
    if(EXISTS "${bloom_locked_python_program}")
        set(BLOOM_LOCKED_PYTHON_HOME "${BLOOM_DEPENDENCY_PREFIX}/python")
        set(Python3_ROOT_DIR "${BLOOM_LOCKED_PYTHON_HOME}")
        set(Python3_FIND_STRATEGY LOCATION)
        set(Python3_FIND_FRAMEWORK NEVER)
        set(Python3_FIND_REGISTRY NEVER)
        set(Python3_FIND_VIRTUALENV STANDARD)
        message(STATUS "Bloom Python: the locked interpreter in ${BLOOM_LOCKED_PYTHON_HOME}")
    elseif(APPLE AND BLOOM_BUILD_PYTHON)
        message(FATAL_ERROR
            "BLOOM_BUILD_PYTHON is ON, but the dependency prefix has no locked CPython "
            "(${BLOOM_DEPENDENCY_PREFIX}/python). Build the superbuild's bloom_dependency_python "
            "target; a qualified macOS build does not use another interpreter.")
    elseif(APPLE)
        # No discovery: a host interpreter must not be embedded by accident.
        set(BLOOM_BUILD_PYTHON OFF CACHE BOOL "Build the Python bridge and embedded interpreter")
    else()
        message(STATUS "Bloom Python: the dependency prefix has no locked CPython "
            "(${BLOOM_DEPENDENCY_PREFIX}/python); this build uses the host's interpreter and "
            "cannot be packaged.")
    endif()
endif()

bloom_configure_quality_tools()

# Tests that read media made by the FFmpeg worker's fixture generator. They are registered where
# that worker is always built and its process isolation is supported: Linux, and a qualified macOS
# or Windows build. Set here because src/runtime's tests need it before apps/bloom-media-worker is added.
set(BLOOM_MEDIA_FIXTURE_TESTS OFF)
if(BUILD_TESTING
   AND (CMAKE_SYSTEM_NAME STREQUAL "Linux"
        OR ((APPLE OR WIN32) AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")))
    set(BLOOM_MEDIA_FIXTURE_TESTS ON)
endif()

# The spelling of a path in a compile definition, run on every system with the Windows form of a
# path as its input (cmake/BloomPathDefinition.cmake).
if(BUILD_TESTING)
    include(BloomTesting)
    bloom_add_test(
        NAME bloom.build.path-definition
        COMMAND ${CMAKE_COMMAND} "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/path_definition.cmake"
        LABELS unit build
        TIMEOUT 60)
    # The link launcher that keeps an unchanged test program's file on macOS (#727). It is used
    # only there, but it is a CMake script, so the same check runs on every system.
    bloom_add_test(
        NAME bloom.build.keep-unchanged-link
        COMMAND ${CMAKE_COMMAND} "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
            "-DWORK=${CMAKE_BINARY_DIR}/keep-unchanged-link-test"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/keep_unchanged_link.cmake"
        LABELS unit build
        TIMEOUT 60)
    # #750: every registered test runs with the fixed memory reading, or is a stress test that
    # names why it measures the real machine (cmake/BloomTesting.cmake). The second test proves the
    # check rejects each way a test can miss the reading.
    bloom_add_test(
        NAME bloom.quality.test_memory_reading
        COMMAND ${CMAKE_COMMAND} "-DTREE=${CMAKE_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/test_memory_reading.cmake"
        LABELS quality build
        TIMEOUT 60)
    set(bloom_memory_check_rejected fixture.gpu-unstated fixture.no-properties
        fixture.live-default-suite fixture.stress-unnamed fixture.empty-reason
        fixture.empty-reading fixture.sub-unstated)
    list(JOIN bloom_memory_check_rejected "," bloom_memory_check_rejected)
    bloom_add_test(
        NAME bloom.quality.test_memory_reading_rejects
        COMMAND ${CMAKE_COMMAND} "-DTREE=${PROJECT_SOURCE_DIR}/cmake/tests/test_memory_reading"
            "-DEXPECT_REJECTED=${bloom_memory_check_rejected}"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/test_memory_reading.cmake"
        LABELS quality build unit
        TIMEOUT 60)
    # #763: every variable a registration names reaches its test on its own, and a scale variant
    # (<name>.dpr<scale>, <name>-scale-<scale>) runs at its scale. The first test reads this build's
    # CTest files; the second proves that check rejects the joined form CMake wrote before; the
    # third registers through the real bloom_add_test, which must refuse the joined form when the
    # build is configured and hand each variable of the split form to the test.
    bloom_add_test(
        NAME bloom.quality.test_environment_entries
        COMMAND ${CMAKE_COMMAND} "-DTREE=${CMAKE_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/test_environment_entries.cmake"
        LABELS quality build
        TIMEOUT 60)
    set(bloom_environment_check_rejected fixture.joined-dpr1.5 fixture.joined-quoted
        fixture.view.dpr1 fixture.view.dpr2 fixture.view-scale-1.5 fixture.sub-joined.dpr2)
    list(JOIN bloom_environment_check_rejected "," bloom_environment_check_rejected)
    bloom_add_test(
        NAME bloom.quality.test_environment_entries_rejects
        COMMAND ${CMAKE_COMMAND}
            "-DTREE=${PROJECT_SOURCE_DIR}/cmake/tests/test_environment_entries"
            "-DEXPECT_REJECTED=${bloom_environment_check_rejected}"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/test_environment_entries.cmake"
        LABELS quality build unit
        TIMEOUT 60)
    bloom_add_test(
        NAME bloom.build.test-environment-registration
        COMMAND ${CMAKE_COMMAND} "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
            "-DWORK=${CMAKE_BINARY_DIR}/test-environment-registration"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/test_environment_registration.cmake"
        LABELS unit build
        TIMEOUT 120)
    # #749: on macOS every registered test runs with a home of its own, so no test reaches the
    # user's real per-user folders there (cmake/BloomTesting.cmake). Registered on macOS only:
    # Linux and Windows find those folders from XDG_* and LOCALAPPDATA, which the runner and the
    # fixtures set, not from the home, so no test there has or needs a home of its own. The second
    # test proves the check rejects each way a test can miss it, on every system.
    if(APPLE)
        bloom_add_test(
            NAME bloom.quality.test_private_home
            COMMAND ${CMAKE_COMMAND} "-DTREE=${CMAKE_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
                -P "${PROJECT_SOURCE_DIR}/cmake/tests/test_private_home.cmake"
            LABELS quality build
            TIMEOUT 60)
    endif()
    set(bloom_home_check_rejected fixture.no-properties fixture.no-home fixture.another-home
        fixture.home-elsewhere fixture.no-fixed-home fixture.other-fixed-home
        fixture.replaced-later fixture.sub-no-home)
    list(JOIN bloom_home_check_rejected "," bloom_home_check_rejected)
    bloom_add_test(
        NAME bloom.quality.test_private_home_rejects
        COMMAND ${CMAKE_COMMAND} "-DTREE=${PROJECT_SOURCE_DIR}/cmake/tests/test_private_home"
            "-DEXPECT_REJECTED=${bloom_home_check_rejected}"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/test_private_home.cmake"
        LABELS quality build unit
        TIMEOUT 60)
endif()

add_subdirectory(tools/quality/json)
add_subdirectory(tools/quality/project_schema_checks)
add_subdirectory(tools/quality/repository_checks)

add_subdirectory(src/core)
add_subdirectory(tools/quality/dependency_artifact_checks)
add_subdirectory(src/platform)
add_subdirectory(src/crash)
add_subdirectory(src/update)
add_subdirectory(src/document)
add_subdirectory(src/project)
# src/color must precede src/runtime: bloom_runtime_evaluator's qualified display-preparation
# stage (issue #97, task C3) links bloom_color_ocio, and target_link_libraries() requires that
# target to already exist. src/color itself only depends on src/core/src/render (both already
# added above), never on src/runtime, so this reordering introduces no cycle.
add_subdirectory(src/render)
add_subdirectory(src/color)
add_subdirectory(src/media)
add_subdirectory(src/svgartwork)
add_subdirectory(src/runtime)
add_subdirectory(src/output)
add_subdirectory(src/commands)
add_subdirectory(src/host)
add_subdirectory(src/scripting)
add_subdirectory(src/mcp)
add_subdirectory(apps/bloom-crash-handler)
add_subdirectory(apps/bloom-media-worker)
add_subdirectory(apps/bloom-cli)
add_subdirectory(apps/bloom-mcp)
add_subdirectory(apps/bloom-update-helper)
if(WIN32)
    # The program of the installer's `cli` folder (apps/bloom-cli-launcher/main.cpp).
    add_subdirectory(apps/bloom-cli-launcher)
endif()
if(BUILD_TESTING)
    # After the crash handler and bloom-cli, which the crash tests run.
    add_subdirectory(src/crash/tests)
endif()

if(BLOOM_BUILD_SPIKES)
    add_subdirectory(tools/spikes/instance0)
endif()

add_custom_target(
    bloom-quality-check
    DEPENDS
        bloom-format-check
        bloom-hygiene-check
        bloom-architecture-check
        bloom-ui-grammar-check
        bloom-product-names-check
        bloom-project-schema-check
        bloom-dependency-artifact-check
)

if(BLOOM_BUILD_DESKTOP)
    # One exact Qt, the dependency lock's, in a qualified build (issue #679).
    include(BloomLockedQt)
    bloom_find_locked_qt(Network Svg Widgets)
    qt_standard_project_setup()

    # Qt 6.8 links the AGL framework into every OpenGL consumer on macOS. The macOS 26 SDK no
    # longer has that framework, and nothing in Qt or Bloom calls it, so the link line is the only
    # thing that needs it. Drop the entry when the SDK in use cannot satisfy it; an SDK that still
    # has AGL is left exactly as Qt set it up. The question is asked of the linker: the running
    # system can still have an AGL.framework directory that no SDK links against, so a file
    # search answers it wrongly.
    if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
        include(CheckCXXSourceCompiles)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_LIBRARIES "-framework AGL")
        check_cxx_source_compiles("int main() { return 0; }" BLOOM_APPLE_AGL_LINKS)
        cmake_pop_check_state()
        if(NOT BLOOM_APPLE_AGL_LINKS)
            get_target_property(bloom_wrap_opengl_libraries
                WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
            if(bloom_wrap_opengl_libraries)
                list(FILTER bloom_wrap_opengl_libraries EXCLUDE REGEX "(^|[ /])AGL(\\.framework)?$")
                set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY
                    INTERFACE_LINK_LIBRARIES "${bloom_wrap_opengl_libraries}")
                message(STATUS "Bloom: this macOS SDK has no AGL framework; removed it from "
                    "Qt's OpenGL link interface.")
            endif()
        endif()
    endif()

    add_subdirectory(src/ui)
    add_subdirectory(apps/bloom)

    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()

# The package: what `cmake --install` adds to the programs' own install rules.
include(BloomPackage)
bloom_add_package_rules()

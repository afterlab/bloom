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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(BloomQuality)
include(BloomWarnings)
include(BloomDependencyPrefix)
include(BloomVulkanRuntime)
include(BloomPackageLayout)

# Before any target is added, so it reaches all of them.
bloom_disable_floating_point_contraction()

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

add_subdirectory(tools/quality/json)
add_subdirectory(tools/quality/project_schema_checks)
add_subdirectory(tools/quality/repository_checks)

add_subdirectory(src/core)
add_subdirectory(tools/quality/dependency_artifact_checks)
add_subdirectory(src/platform)
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
add_subdirectory(apps/bloom-media-worker)
add_subdirectory(apps/bloom-cli)
add_subdirectory(apps/bloom-mcp)

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

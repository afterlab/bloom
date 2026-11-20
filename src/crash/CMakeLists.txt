# Issue #736: crash reports. Qt-free: the crash folder and its reports, the minidump summary, and
# starting Crashpad's out-of-process handler in a program (docs/architecture/platform-support.md,
# "Crash Reports"). The application, bloom-cli and bloom-mcp link it; the application's notice
# (src/ui) reads the folder through it.
#
# Crashpad is a locked dependency (dependencies/superbuild/projects/crashpad.cmake). A qualified
# build requires it. A developer build without it in its prefix still builds: the reporter then
# reports itself unavailable and records no crash, and the folder code works the same.
set(BLOOM_CRASH_HAVE_CRASHPAD OFF)
if(BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
    bloom_find_dependency(crashpad)
    set(BLOOM_CRASH_HAVE_CRASHPAD ON)
elseif(BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/lib/cmake/crashpad")
    find_package(crashpad CONFIG QUIET PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    if(crashpad_FOUND)
        set(BLOOM_CRASH_HAVE_CRASHPAD ON)
    endif()
endif()
set(BLOOM_CRASH_HAVE_CRASHPAD ${BLOOM_CRASH_HAVE_CRASHPAD} CACHE INTERNAL
    "Whether this build has Crashpad and builds the crash handler")
if(BLOOM_CRASH_HAVE_CRASHPAD)
    # The handler program (apps/bloom-crash-handler) links Crashpad's handler library too.
    get_directory_property(bloom_crash_imported IMPORTED_TARGETS)
    foreach(bloom_crash_target IN LISTS bloom_crash_imported)
        if(bloom_crash_target MATCHES "^crashpad::")
            set_target_properties(${bloom_crash_target} PROPERTIES IMPORTED_GLOBAL TRUE)
        endif()
    endforeach()
    message(STATUS "Bloom crash reports: Crashpad from the dependency prefix")
else()
    message(STATUS "Bloom crash reports: no Crashpad in this build; crashes are not recorded")
endif()

# The source revision a release build names in its text reports; the release workflow passes it.
set(BLOOM_SOURCE_REVISION "" CACHE STRING
    "The source revision this build was made from, for crash reports (empty: not named)")
if(BLOOM_SOURCE_REVISION AND NOT BLOOM_SOURCE_REVISION MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "BLOOM_SOURCE_REVISION must be a hexadecimal revision")
endif()

# Crashpad's headers name each other from include/crashpad (for example "util/file/file_io.h"),
# and its exported targets list only the client and mini_chromium directories.
add_library(bloom_crashpad INTERFACE)
if(BLOOM_CRASH_HAVE_CRASHPAD)
    get_filename_component(bloom_crashpad_include "${crashpad_DIR}/../../../include/crashpad"
        ABSOLUTE)
    target_include_directories(bloom_crashpad SYSTEM INTERFACE "${bloom_crashpad_include}")
    target_link_libraries(bloom_crashpad INTERFACE crashpad::client)
    if(WIN32)
        # Crashpad's headers are written for these (its own build defines them for every file).
        target_compile_definitions(bloom_crashpad INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endif()

add_library(bloom_crash STATIC
    crash_collect.cpp
    crash_collect.hpp
    crash_internal.hpp
    crash_reporter.cpp
    crash_reports.cpp
    held_file.cpp
    minidump_summary.cpp
    system_facts.cpp)
target_sources(bloom_crash PUBLIC FILE_SET HEADERS BASE_DIRS include
    FILES include/bloom/crash/crash_reporter.hpp
          include/bloom/crash/crash_reports.hpp
          include/bloom/crash/minidump_summary.hpp)
target_compile_features(bloom_crash PUBLIC cxx_std_20)
target_link_libraries(bloom_crash PUBLIC bloom_core PRIVATE bloom_platform bloom_crashpad)
target_compile_definitions(bloom_crash PRIVATE
    BLOOM_CRASH_HAVE_CRASHPAD=$<BOOL:${BLOOM_CRASH_HAVE_CRASHPAD}>)
if(BLOOM_SOURCE_REVISION)
    set_property(SOURCE system_facts.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "BLOOM_CRASH_SOURCE_REVISION=\"${BLOOM_SOURCE_REVISION}\"")
endif()
bloom_enable_warnings(bloom_crash)

# What a program that starts the reporter links: the module, and on Linux the thread wrapper that
# gives every thread an alternate signal stack (thread_signal_stack_linux.cpp), compiled into the
# program itself.
add_library(bloom_crash_program INTERFACE)
target_link_libraries(bloom_crash_program INTERFACE bloom_crash)
if(BLOOM_CRASH_HAVE_CRASHPAD AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(bloom_crash_thread_stack OBJECT thread_signal_stack_linux.cpp)
    target_link_libraries(bloom_crash_thread_stack PRIVATE bloom_crashpad)
    bloom_enable_warnings(bloom_crash_thread_stack)
    target_sources(bloom_crash_program INTERFACE $<TARGET_OBJECTS:bloom_crash_thread_stack>)
    target_link_libraries(bloom_crash_program INTERFACE bloom_crashpad ${CMAKE_DL_LIBS})
endif()

# The handler program a reporter starts: beside the program in the build tree as well as in a
# package, where the reporter looks for it. bloom_crash_stage_handler(<target>) copies it beside
# <target> after <target> is built.
function(bloom_crash_stage_handler target)
    if(NOT TARGET bloom-crash-handler)
        return()
    endif()
    add_dependencies(${target} bloom-crash-handler)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:bloom-crash-handler>" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# The tests (tests/) are added by the root CMakeLists.txt after the programs they run.

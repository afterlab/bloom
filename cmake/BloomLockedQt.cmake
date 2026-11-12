# The one Qt a qualified build uses, per docs/architecture/dependency-intake.md.
#
# The dependency lock's `qt` component records one exact Qt version and, for every profile, the
# official binary archives of it by SHA-256. The dependency superbuild's `bloom_dependency_qt`
# target unpacks those archives, unmodified, into `<prefix>/qt` and records there what it
# unpacked.
#
# bloom_find_locked_qt(<components>...) finds Qt for the build:
#
#   qualified         Qt is taken from `<prefix>/qt` and nowhere else, and must be the lock's
#                     version. A prefix without it, or with another version, stops the configure
#                     with the reason; a Qt location an earlier configure cached is dropped.
#   developer-system  Any Qt 6.8 or later the host offers, as before (Unqualified).
#
# BLOOM_QT_VERSION_REQUEST is set for later find_package(Qt6 ...) calls of further components:
# `<version> EXACT` in qualified mode, `6.8` otherwise.

include_guard(GLOBAL)

# The lock's Qt version, or an empty string when the lock has no `qt` component.
function(bloom_locked_qt_version output)
    set(${output} "" PARENT_SCOPE)
    set(bloom_lock "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../dependencies/dependencies.lock.json")
    if(NOT EXISTS "${bloom_lock}")
        return()
    endif()
    file(READ "${bloom_lock}" bloom_lock_json)
    string(JSON bloom_component_count LENGTH "${bloom_lock_json}" "components")
    math(EXPR bloom_component_last "${bloom_component_count} - 1")
    foreach(bloom_component_index RANGE 0 ${bloom_component_last})
        string(JSON bloom_component_name GET "${bloom_lock_json}"
            "components" ${bloom_component_index} "name")
        if(bloom_component_name STREQUAL "qt")
            string(JSON bloom_version GET "${bloom_lock_json}"
                "components" ${bloom_component_index} "version")
            set(${output} "${bloom_version}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Why the directory `root` cannot serve a qualified build as Qt `version`, or an empty string
# when it can: it must hold the record the superbuild writes, and the record must name that
# version.
function(bloom_locked_qt_problem output root version)
    set(${output} "" PARENT_SCOPE)
    set(bloom_qt_record "${root}/bloom-prebuilt.txt")
    if(NOT EXISTS "${bloom_qt_record}")
        set(${output}
            "${root} does not hold the locked Qt ${version}. Build the dependency superbuild's \
bloom_dependency_qt target, which unpacks the lock's official Qt binaries there; a qualified \
build uses no other Qt. Use developer-system mode to build against the host's Qt."
            PARENT_SCOPE)
        return()
    endif()
    file(STRINGS "${bloom_qt_record}" bloom_qt_record_lines LIMIT_COUNT 1)
    if(NOT bloom_qt_record_lines STREQUAL "qt ${version}")
        set(${output}
            "${root} holds '${bloom_qt_record_lines}', but the dependency lock records Qt \
${version}. Build the dependency superbuild's bloom_dependency_qt target to replace it; a \
qualified build refuses a Qt that is not the locked version."
            PARENT_SCOPE)
    endif()
endfunction()

# A macro: find_package must run in the caller's scope for Qt's variables and commands.
macro(bloom_find_locked_qt)
    if(DEFINED BLOOM_DEPENDENCY_MODE AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        bloom_locked_qt_version(bloom_qt_locked_version)
        if(NOT bloom_qt_locked_version)
            message(FATAL_ERROR "qualified mode: the dependency lock records no Qt.")
        endif()
        set(bloom_qt_root "${BLOOM_DEPENDENCY_PREFIX}/qt")
        cmake_path(NORMAL_PATH bloom_qt_root)
        bloom_locked_qt_problem(bloom_qt_problem "${bloom_qt_root}" "${bloom_qt_locked_version}")
        if(bloom_qt_problem)
            message(FATAL_ERROR "qualified mode: ${bloom_qt_problem}")
        endif()
        # A Qt location cached by an earlier configure (the host's Qt, before Qt was locked)
        # is dropped, so the prefix's Qt is found instead of silently keeping the other one.
        get_cmake_property(bloom_qt_cache_variables CACHE_VARIABLES)
        foreach(bloom_qt_cache_variable IN LISTS bloom_qt_cache_variables)
            if(bloom_qt_cache_variable MATCHES "^Qt6[A-Za-z0-9_]*_DIR$")
                set(bloom_qt_cached "${${bloom_qt_cache_variable}}")
                cmake_path(IS_PREFIX bloom_qt_root "${bloom_qt_cached}" NORMALIZE
                    bloom_qt_cached_inside)
                if(NOT bloom_qt_cached_inside)
                    unset(${bloom_qt_cache_variable} CACHE)
                endif()
            endif()
        endforeach()
        set(Qt6_ROOT "${bloom_qt_root}")
        set(BLOOM_QT_VERSION_REQUEST ${bloom_qt_locked_version} EXACT)
        find_package(Qt6 ${BLOOM_QT_VERSION_REQUEST} REQUIRED COMPONENTS ${ARGN}
            PATHS "${bloom_qt_root}" NO_DEFAULT_PATH)
        cmake_path(IS_PREFIX bloom_qt_root "${Qt6_DIR}" NORMALIZE bloom_qt_found_inside)
        if(NOT bloom_qt_found_inside OR NOT Qt6_VERSION VERSION_EQUAL bloom_qt_locked_version)
            message(FATAL_ERROR
                "qualified mode: found Qt ${Qt6_VERSION} at ${Qt6_DIR}; the build takes Qt "
                "${bloom_qt_locked_version} from ${bloom_qt_root} only.")
        endif()
        # Qt's own packages find each other, and its tools, from this directory.
        list(PREPEND CMAKE_PREFIX_PATH "${bloom_qt_root}")
        message(STATUS "Bloom Qt: ${Qt6_VERSION} from ${bloom_qt_root} (the dependency lock's)")
    else()
        set(BLOOM_QT_VERSION_REQUEST 6.8)
        find_package(Qt6 ${BLOOM_QT_VERSION_REQUEST} REQUIRED COMPONENTS ${ARGN})
        message(STATUS "Bloom Qt: ${Qt6_VERSION} from the host (Unqualified)")
    endif()
endmacro()

# The macOS deployment target, per docs/architecture/platform-support.md.
#
# The dependency lock's Apple profile records one deployment target (consumerAbi
# .appleDeploymentTarget). The dependency superbuild and the Bloom build both take it from there,
# so the dependencies, the application and the bundle's LSMinimumSystemVersion cannot disagree.
#
# bloom_locked_apple_deployment_target(<out>) returns that version, or an empty string when the
# lock has no macOS profile. It only reads the lock, so it is usable before project().

include_guard(GLOBAL)

function(bloom_locked_apple_deployment_target output)
    set(${output} "" PARENT_SCOPE)
    set(bloom_lock "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../dependencies/dependencies.lock.json")
    if(NOT EXISTS "${bloom_lock}")
        return()
    endif()
    file(READ "${bloom_lock}" bloom_lock_json)
    string(JSON bloom_profile_count LENGTH "${bloom_lock_json}" "profiles")
    math(EXPR bloom_profile_last "${bloom_profile_count} - 1")
    foreach(bloom_profile_index RANGE 0 ${bloom_profile_last})
        string(JSON bloom_profile_system GET "${bloom_lock_json}"
            "profiles" ${bloom_profile_index} "target" "operatingSystem")
        if(bloom_profile_system STREQUAL "macos")
            string(JSON bloom_target GET "${bloom_lock_json}"
                "profiles" ${bloom_profile_index} "consumerAbi" "appleDeploymentTarget")
            set(${output} "${bloom_target}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# bloom_apply_apple_deployment_target() sets CMAKE_OSX_DEPLOYMENT_TARGET from the lock on a macOS
# host. Call it before project(). A value given on the command line must agree with the lock in
# qualified mode; developer-system mode may build for another target and stays Unqualified. A value
# this function set earlier follows the lock when the lock changes, so an existing build tree
# reconfigures instead of reporting its own cached value as a disagreement.
function(bloom_apply_apple_deployment_target)
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        return()
    endif()
    bloom_locked_apple_deployment_target(bloom_locked_target)
    if(NOT bloom_locked_target)
        return()
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET
       OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "$CACHE{BLOOM_APPLE_DEPLOYMENT_TARGET_FROM_LOCK}")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${bloom_locked_target}" CACHE STRING
            "macOS deployment target (from the dependency lock's Apple profile)" FORCE)
        set(BLOOM_APPLE_DEPLOYMENT_TARGET_FROM_LOCK "${bloom_locked_target}" CACHE INTERNAL
            "The deployment target this build took from the dependency lock")
    elseif(NOT CMAKE_OSX_DEPLOYMENT_TARGET VERSION_EQUAL bloom_locked_target
           AND DEFINED BLOOM_DEPENDENCY_MODE AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        message(FATAL_ERROR
            "qualified mode: CMAKE_OSX_DEPLOYMENT_TARGET is ${CMAKE_OSX_DEPLOYMENT_TARGET}, but the "
            "dependency lock's macOS profile records ${bloom_locked_target}. The lock, not the "
            "command line, is the authority; use developer-system mode for another target.")
    endif()
endfunction()

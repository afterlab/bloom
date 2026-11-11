# Bloom dependency superbuild — the dependency-only composition root defined by
# docs/architecture/dependency-intake.md. It acquires locked archives, builds them with
# networking disabled, and installs them into a staging prefix that the normal Bloom build
# consumes. The application build tree never runs ExternalProject or FetchContent.
#
# The reviewed production lock at dependencies/dependencies.lock.json is the authority for every
# component's source URL, digest, and option set; the offline dependency-artifact checker
# validates it, including that recipes and lock agree is a human-review obligation until recipes
# read the lock mechanically. Prefixes produced here are consumed through
# cmake/BloomDependencyPrefix.cmake; they are not fully qualified until the qualify phase writes
# and validates the production prefix manifest.
cmake_minimum_required(VERSION 3.25)

project(BloomDependencySuperbuild LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Build the dependency superbuild out of source.")
endif()

set(BLOOM_DEPENDENCY_PREFIX "${CMAKE_BINARY_DIR}/prefix" CACHE PATH
    "Staging installation prefix that receives every dependency.")
set(BLOOM_DEPENDENCY_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads" CACHE PATH
    "Content-addressed archive cache. Populate it offline to build without network access.")
option(BLOOM_DEPENDENCY_ALLOW_DOWNLOADS
    "Permit the acquire phase to download missing locked archives. OFF builds strictly offline
from BLOOM_DEPENDENCY_DOWNLOAD_DIR: configuring fails, naming each archive, when one is missing
from the cache or does not match its locked digest."
    ON)

# The build phase is offline by construction: every recipe verifies its archive digest before
# extraction, disables in-recipe network features, and never uses FetchContent. Downloads happen
# only through ExternalProject's download step against the exact locked URL and hash.
include(ExternalProject)

# Reproducibility baseline shared by every recipe. SOURCE_DATE_EPOCH and locale pinning keep
# component builds independent of the invoking user's environment.
set(BLOOM_DEPENDENCY_COMMON_ENV
    "SOURCE_DATE_EPOCH=0"
    "LC_ALL=C"
    "TZ=UTC")

set(BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX:PATH=${BLOOM_DEPENDENCY_PREFIX}
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
    -DBUILD_SHARED_LIBS:BOOL=OFF
    -DCMAKE_POLICY_DEFAULT_CMP0074:STRING=NEW
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=ON
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY:BOOL=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY:BOOL=OFF)

# macOS: every component is built for the deployment target the lock's Apple profile records, the
# same one the Bloom build takes from it. The lock is the authority; a different value given on
# the command line is an error rather than a second source of truth.
set(BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG)
if(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/BloomAppleDeploymentTarget.cmake")
    bloom_locked_apple_deployment_target(bloom_locked_deployment_target)
    if(NOT bloom_locked_deployment_target)
        message(FATAL_ERROR "The dependency lock has no macOS profile to take a deployment "
            "target from.")
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET
       AND NOT CMAKE_OSX_DEPLOYMENT_TARGET VERSION_EQUAL bloom_locked_deployment_target)
        message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET is ${CMAKE_OSX_DEPLOYMENT_TARGET}, but "
            "the dependency lock's macOS profile records ${bloom_locked_deployment_target}.")
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${bloom_locked_deployment_target}")
    list(APPEND BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS
        -DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET})
    # For the recipes that are not CMake projects (Meson, configure scripts).
    set(BLOOM_DEPENDENCY_APPLE_MINIMUM_FLAG "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    message(STATUS "Bloom dependencies: macOS deployment target "
        "${CMAKE_OSX_DEPLOYMENT_TARGET} (from the dependency lock)")
endif()

if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    set(BLOOM_DEPENDENCY_OFFLINE_ARGS DOWNLOAD_NO_PROGRESS ON)
endif()

# A component's own CMake build can fetch sources while it configures (OpenColorIO cloned sse2neon
# that way), which the archive check below cannot see. With downloads off, FetchContent in every
# CMake component is disconnected, so such a fetch gets nothing instead of reaching the network.
# This covers CMake's FetchContent only, and so far the macOS build only: the other platforms'
# superbuilds have not been run with it.
if(APPLE AND NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    list(APPEND BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS -DFETCHCONTENT_FULLY_DISCONNECTED:BOOL=ON)
endif()

# The configure scripts and Makefiles of the codec libraries and FFmpeg split their arguments on
# whitespace, so they cannot build in, install to, or compile against a path that contains any
# (a volume or folder name with a space is ordinary on macOS and Windows). Those recipes reach
# such a path through a symbolic link in a directory without whitespace. A path without
# whitespace is returned unchanged, so nothing differs where there is none.
set(BLOOM_DEPENDENCY_PLAIN_ROOT "" CACHE PATH
    "Directory without whitespace in its path that holds the links used to reach a build tree,
prefix or SDK whose path contains whitespace. Empty selects the system temporary directory.")
function(bloom_dependency_plain_path output path)
    if(NOT path MATCHES "[ \t]")
        set(${output} "${path}" PARENT_SCOPE)
        return()
    endif()
    set(bloom_plain_root "${BLOOM_DEPENDENCY_PLAIN_ROOT}")
    if(NOT bloom_plain_root)
        if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{TMPDIR}" bloom_plain_root)
        elseif(WIN32)
            file(TO_CMAKE_PATH "$ENV{TEMP}" bloom_plain_root)
        else()
            set(bloom_plain_root /tmp)
        endif()
    endif()
    string(REGEX REPLACE "/+$" "" bloom_plain_root "${bloom_plain_root}")
    if(bloom_plain_root STREQUAL "" OR bloom_plain_root MATCHES "[ \t]")
        message(FATAL_ERROR
            "'${path}' contains whitespace, and so does the directory for the links that reach "
            "it ('${bloom_plain_root}'). Set BLOOM_DEPENDENCY_PLAIN_ROOT to a directory whose "
            "path has no whitespace.")
    endif()
    string(SHA256 bloom_plain_digest "${path}")
    string(SUBSTRING "${bloom_plain_digest}" 0 16 bloom_plain_digest)
    set(bloom_plain_link "${bloom_plain_root}/bloom-dependency-${bloom_plain_digest}")
    file(MAKE_DIRECTORY "${bloom_plain_root}")
    if(NOT IS_DIRECTORY "${path}")
        file(MAKE_DIRECTORY "${path}")
    endif()
    file(CREATE_LINK "${path}" "${bloom_plain_link}" SYMBOLIC)
    message(STATUS "Bloom dependencies: '${path}' contains whitespace; the codec recipes reach "
        "it through ${bloom_plain_link}")
    set(${output} "${bloom_plain_link}" PARENT_SCOPE)
endfunction()

add_subdirectory(projects)

# Strict offline mode. ExternalProject downloads an archive whenever the cached file is missing or
# its digest differs, so DOWNLOAD_NO_PROGRESS alone does not stop a fetch. With downloads off,
# every recipe's archive must already be in the cache with its locked SHA-256; the check covers
# every ExternalProject target the recipes declare, so a recipe added later is covered too.
if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    get_property(bloom_recipe_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/projects"
        PROPERTY BUILDSYSTEM_TARGETS)
    set(bloom_offline_problems)
    foreach(bloom_recipe_target IN LISTS bloom_recipe_targets)
        get_property(bloom_recipe_url TARGET ${bloom_recipe_target} PROPERTY _EP_URL)
        if(NOT bloom_recipe_url)
            continue()
        endif()
        get_property(bloom_recipe_directory TARGET ${bloom_recipe_target} PROPERTY _EP_DOWNLOAD_DIR)
        get_property(bloom_recipe_name TARGET ${bloom_recipe_target} PROPERTY _EP_DOWNLOAD_NAME)
        get_property(bloom_recipe_hash TARGET ${bloom_recipe_target} PROPERTY _EP_URL_HASH)
        if(NOT bloom_recipe_name)
            get_filename_component(bloom_recipe_name "${bloom_recipe_url}" NAME)
        endif()
        set(bloom_recipe_archive "${bloom_recipe_directory}/${bloom_recipe_name}")
        if(NOT EXISTS "${bloom_recipe_archive}")
            list(APPEND bloom_offline_problems
                "${bloom_recipe_target}: ${bloom_recipe_name} is not in the cache")
        elseif(bloom_recipe_hash MATCHES "^SHA256=([0-9a-fA-F]+)$")
            file(SHA256 "${bloom_recipe_archive}" bloom_recipe_actual)
            string(TOLOWER "${CMAKE_MATCH_1}" bloom_recipe_expected)
            if(NOT bloom_recipe_actual STREQUAL bloom_recipe_expected)
                list(APPEND bloom_offline_problems
                    "${bloom_recipe_target}: ${bloom_recipe_name} does not match its locked SHA-256")
            endif()
        endif()
    endforeach()
    if(bloom_offline_problems)
        list(JOIN bloom_offline_problems "\n  " bloom_offline_report)
        message(FATAL_ERROR
            "BLOOM_DEPENDENCY_ALLOW_DOWNLOADS is OFF and the archive cache "
            "${BLOOM_DEPENDENCY_DOWNLOAD_DIR} is incomplete:\n  ${bloom_offline_report}\n"
            "Populate the cache with the locked archives, or configure with "
            "-DBLOOM_DEPENDENCY_ALLOW_DOWNLOADS=ON.")
    endif()
endif()

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

if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    set(BLOOM_DEPENDENCY_OFFLINE_ARGS DOWNLOAD_NO_PROGRESS ON)
endif()

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

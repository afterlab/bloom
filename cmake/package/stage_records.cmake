# Puts the license texts, the notices and the dependency lock into a Bloom package.
#
#   cmake -DROOT=<package root> -DSOURCE=<source tree>
#         -DOPERATING_SYSTEM=<linux|macos|windows>
#         -DRECORD_DIR=<record directory, relative to the root>
#         -P stage_records.cmake
#
#   <RECORD_DIR>/LICENSE, NOTICE, THIRD_PARTY_NOTICES.md     Bloom's own
#   <RECORD_DIR>/dependencies.lock.json                      what the package was built from
#   <RECORD_DIR>/licenses/<component>/...                    one directory per component
#
# The components are
#
#   * every component the dependency lock builds for this operating system, with exactly the
#     license, copyright and notice files the lock names for it (each is compared with the
#     lock's SHA-256 before it is copied), under the path it has in the component's record
#     directory, so Qt's notice finds its `texts` directory beside it;
#   * the components whose bytes are checked in rather than built by the superbuild (a
#     `third_party/<name>` directory under src), with the license files kept beside their
#     sources and every file of dependencies/licenses/<name> that is not an intake record.
#
# A component of this operating system for which the lock names no file stops the staging: a
# package never goes out without the license of something in it.
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS ROOT SOURCE OPERATING_SYSTEM RECORD_DIR)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "stage_records.cmake needs ${argument}")
    endif()
endforeach()
set(lock_file "${SOURCE}/dependencies/dependencies.lock.json")
if(NOT EXISTS "${lock_file}")
    message(FATAL_ERROR "stage_records.cmake: ${lock_file} does not exist")
endif()
file(READ "${lock_file}" lock)

set(records "${ROOT}/${RECORD_DIR}")
set(licenses "${records}/licenses")
file(REMOVE_RECURSE "${licenses}")
file(MAKE_DIRECTORY "${licenses}")

# The profile of this operating system.
set(profile "")
string(JSON profile_count LENGTH "${lock}" profiles)
math(EXPR profile_last "${profile_count} - 1")
foreach(index RANGE 0 ${profile_last})
    string(JSON system GET "${lock}" profiles ${index} target operatingSystem)
    if(system STREQUAL OPERATING_SYSTEM)
        string(JSON profile GET "${lock}" profiles ${index} id)
    endif()
endforeach()
if(NOT profile)
    message(FATAL_ERROR "stage_records.cmake: the dependency lock has no ${OPERATING_SYSTEM} "
                        "profile")
endif()

set(locked_directories "")
set(staged 0)
string(JSON component_count LENGTH "${lock}" components)
math(EXPR component_last "${component_count} - 1")
foreach(index RANGE 0 ${component_last})
    string(JSON name GET "${lock}" components ${index} name)
    # The record directory is the lock's for every profile, so that a component built only for
    # another operating system is not taken for a checked-in one below.
    set(files "")
    foreach(kind IN ITEMS licenseFiles copyrightFiles noticeFiles)
        string(JSON file_count LENGTH "${lock}" components ${index} license ${kind})
        if(file_count EQUAL 0)
            continue()
        endif()
        math(EXPR file_last "${file_count} - 1")
        foreach(file RANGE 0 ${file_last})
            string(JSON path GET "${lock}" components ${index} license ${kind} ${file} path)
            string(JSON digest GET "${lock}" components ${index} license ${kind} ${file} sha256)
            list(APPEND files "${path}|${digest}")
            if(path MATCHES "^dependencies/licenses/([^/]+)/")
                list(APPEND locked_directories "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endforeach()

    set(built FALSE)
    string(JSON build_count LENGTH "${lock}" components ${index} profileBuilds)
    math(EXPR build_last "${build_count} - 1")
    foreach(build RANGE 0 ${build_last})
        string(JSON build_profile GET "${lock}" components ${index} profileBuilds ${build}
               profileId)
        if(build_profile STREQUAL profile)
            set(built TRUE)
        endif()
    endforeach()
    if(NOT built)
        continue()
    endif()
    if(NOT files)
        message(FATAL_ERROR "stage_records.cmake: the dependency lock names no license file "
                            "for ${name}")
    endif()
    foreach(entry IN LISTS files)
        string(REGEX MATCH "^(.*)\\|sha256:([0-9a-f]+)$" matched "${entry}")
        set(path "${CMAKE_MATCH_1}")
        set(digest "${CMAKE_MATCH_2}")
        if(NOT matched OR NOT path MATCHES "^dependencies/licenses/[^/]+/(.+)$")
            message(FATAL_ERROR "stage_records.cmake: ${name}: '${entry}' is not a file of a "
                                "record directory with a SHA-256")
        endif()
        set(relative "${CMAKE_MATCH_1}")
        if(NOT EXISTS "${SOURCE}/${path}")
            message(FATAL_ERROR "stage_records.cmake: ${name}: ${path} does not exist")
        endif()
        file(SHA256 "${SOURCE}/${path}" actual)
        if(NOT actual STREQUAL digest)
            message(FATAL_ERROR "stage_records.cmake: ${name}: ${path} is not the file the "
                                "dependency lock records")
        endif()
        get_filename_component(directory "${licenses}/${name}/${relative}" DIRECTORY)
        file(COPY "${SOURCE}/${path}" DESTINATION "${directory}")
        math(EXPR staged "${staged} + 1")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES locked_directories)

# The checked-in components: the license files beside their sources, and whatever else their
# record directory keeps that is not an intake record.
file(GLOB directories LIST_DIRECTORIES true "${SOURCE}/src/*/third_party/*"
     "${SOURCE}/src/*/*/third_party/*" "${SOURCE}/dependencies/licenses/*")
list(SORT directories)
set(checked_in "")
foreach(directory IN LISTS directories)
    get_filename_component(name "${directory}" NAME)
    if(NOT IS_DIRECTORY "${directory}")
        continue()
    endif()
    file(GLOB_RECURSE texts LIST_DIRECTORIES false RELATIVE "${directory}" "${directory}/*")
    if(directory MATCHES "/dependencies/licenses/[^/]+$")
        if(name IN_LIST locked_directories)
            continue()
        endif()
        list(FILTER texts EXCLUDE REGEX
             "(^|/)(provenance|review|security)\\.md$|\\.(asc|sig|sigstore|provenance\\.json)$")
    else()
        # The component's own bytes are inside the programs; its license travels as a file.
        list(FILTER texts INCLUDE REGEX "(^|/)(LICENSE|LICENCE|COPYING|NOTICE|OFL)[^/]*$")
        list(APPEND checked_in "${name}")
    endif()
    foreach(text IN LISTS texts)
        get_filename_component(destination "${licenses}/${name}/${text}" DIRECTORY)
        file(COPY "${directory}/${text}" DESTINATION "${destination}")
        math(EXPR staged "${staged} + 1")
    endforeach()
endforeach()
foreach(name IN LISTS checked_in)
    file(GLOB texts "${licenses}/${name}/*")
    if(NOT texts)
        message(FATAL_ERROR "stage_records.cmake: the checked-in component ${name} has no "
                            "license file beside its sources or in dependencies/licenses/${name}")
    endif()
endforeach()

file(COPY "${SOURCE}/LICENSE" "${SOURCE}/NOTICE" "${SOURCE}/THIRD_PARTY_NOTICES.md"
          "${lock_file}"
     DESTINATION "${records}")
message(STATUS "Bloom package: ${staged} license and notice files, the dependency lock and "
               "Bloom's own license")

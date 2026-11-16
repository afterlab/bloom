# Puts the platform's own updater into a macOS or Windows Bloom package
# (dependencies/update-runtime; docs/decisions/0041-self-update.md).
#
#   cmake -DROOT=<package root> -DRUNTIME=<dependency prefix>/<sparkle | winsparkle>
#         -DRECORD=<source tree>/dependencies/update-runtime/<sparkle | winsparkle>.json
#         -DSOURCE=<source tree> -DOPERATING_SYSTEM=<macos | windows>
#         -DLIBRARY_DIR=<library directory, relative to the root>
#         -DRECORD_DIR=<record directory, relative to the root>
#         -P stage_update_runtime.cmake
#
#   macOS    <LIBRARY_DIR>/Sparkle.framework     as published, with its links and its own
#                                                signature (copied by the system's ditto)
#   Windows  <LIBRARY_DIR>/WinSparkle.dll        as published
#   <RECORD_DIR>/licenses/update-runtime/...     the license files the record names
#   <RECORD_DIR>/update-runtime.json             the record: release, address and checksum of
#                                                what was taken, and what is inside it
#
# The prefix's directory is what the superbuild's update-runtime-fetch.cmake left, which
# compared the download with the record before it unpacked anything. Every license file is
# compared with the record's SHA-256 here. Runs after stage_qt.cmake and stage_records.cmake,
# which both start their directories afresh.
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS ROOT RUNTIME RECORD SOURCE OPERATING_SYSTEM LIBRARY_DIR RECORD_DIR)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "stage_update_runtime.cmake needs ${argument}")
    endif()
endforeach()
set(libraries "${ROOT}/${LIBRARY_DIR}")
set(records "${ROOT}/${RECORD_DIR}")
if(NOT IS_DIRECTORY "${records}/licenses")
    message(FATAL_ERROR "stage_update_runtime.cmake: ${records} has no licenses directory; "
                        "stage the records first")
endif()

if(OPERATING_SYSTEM STREQUAL "macos")
    set(item "Sparkle.framework")
    if(NOT EXISTS "${RUNTIME}/${item}/Versions/Current/Sparkle")
        message(FATAL_ERROR "stage_update_runtime.cmake: ${RUNTIME} has no ${item}")
    endif()
    file(MAKE_DIRECTORY "${libraries}")
    file(REMOVE_RECURSE "${libraries}/${item}")
    execute_process(COMMAND ditto "${RUNTIME}/${item}" "${libraries}/${item}"
                    RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "stage_update_runtime.cmake: ${item} could not be copied: ${error}")
    endif()
elseif(OPERATING_SYSTEM STREQUAL "windows")
    set(item "WinSparkle.dll")
    if(NOT EXISTS "${RUNTIME}/x64/Release/${item}")
        message(FATAL_ERROR "stage_update_runtime.cmake: ${RUNTIME} has no ${item}")
    endif()
    file(COPY "${RUNTIME}/x64/Release/${item}" DESTINATION "${libraries}")
else()
    message(FATAL_ERROR "stage_update_runtime.cmake: no platform updater is carried on "
                        "'${OPERATING_SYSTEM}'")
endif()

# The license files: the record's own and each component's, each once.
file(READ "${RECORD}" record)
set(files "")
string(JSON path GET "${record}" license file path)
string(JSON digest GET "${record}" license file sha256)
list(APPEND files "${path}|${digest}")
string(JSON component_count LENGTH "${record}" components)
math(EXPR component_last "${component_count} - 1")
foreach(component RANGE 0 ${component_last})
    string(JSON component_name GET "${record}" components ${component} name)
    string(JSON file_count LENGTH "${record}" components ${component} files)
    if(file_count EQUAL 0)
        message(FATAL_ERROR "stage_update_runtime.cmake: the record names no license file for "
                            "${component_name}")
    endif()
    math(EXPR file_last "${file_count} - 1")
    foreach(file RANGE 0 ${file_last})
        string(JSON path GET "${record}" components ${component} files ${file} path)
        string(JSON digest GET "${record}" components ${component} files ${file} sha256)
        list(APPEND files "${path}|${digest}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES files)
file(REMOVE_RECURSE "${records}/licenses/update-runtime")
foreach(entry IN LISTS files)
    string(REGEX MATCH "^(.*)\\|([0-9a-f]+)$" matched "${entry}")
    set(path "${CMAKE_MATCH_1}")
    set(digest "${CMAKE_MATCH_2}")
    if(NOT path MATCHES "^dependencies/update-runtime/licenses/(.+)$")
        message(FATAL_ERROR "stage_update_runtime.cmake: ${path} is not a license file of the "
                            "update runtime")
    endif()
    set(relative "${CMAKE_MATCH_1}")
    if(NOT EXISTS "${SOURCE}/${path}")
        message(FATAL_ERROR "stage_update_runtime.cmake: ${path} does not exist")
    endif()
    file(SHA256 "${SOURCE}/${path}" actual)
    if(NOT actual STREQUAL digest)
        message(FATAL_ERROR "stage_update_runtime.cmake: ${path} is not the file the record names")
    endif()
    get_filename_component(destination "${records}/licenses/update-runtime/${relative}" DIRECTORY)
    file(COPY "${SOURCE}/${path}" DESTINATION "${destination}")
endforeach()
file(COPY_FILE "${RECORD}" "${records}/update-runtime.json")
list(LENGTH files file_count)
message(STATUS "Bloom package: ${item} and ${file_count} license files of the update runtime")

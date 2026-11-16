# Puts an installer tool (dependencies/installer-tools/innosetup.json) into the dependency
# prefix, by checksum.
#
#   cmake -DBLOOM_RECORD=<dependencies/installer-tools/innosetup.json>
#         -DBLOOM_DESTINATION=<prefix>/innosetup -DBLOOM_DOWNLOAD_DIR=<archive cache>
#         -DBLOOM_ALLOW_DOWNLOADS=<ON|OFF> [-DBLOOM_CHECK_ONLY=ON]
#         -P installer-tools-fetch.cmake
#
# The published file is taken from the cache or, when downloads are allowed, from the record's
# URL; its size and SHA-256 are compared with the record first, and a file that differs stops the
# build before anything is run. The file is itself an installer. On Windows it is run silently,
# for the current user only, in portable mode (no Start menu entries, no file association and no
# uninstall entry in the registry), into <destination>; the record's `install.switches` are its
# command line. <destination>/bloom-installer-tool.txt names the release and checksum. A
# destination whose note already says the same, and that holds the record's `install.program`,
# is left untouched. On any other host the checksum is verified and nothing is installed: the
# tool is unpacked on Windows only.
cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS BLOOM_RECORD BLOOM_DESTINATION BLOOM_DOWNLOAD_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "installer-tools-fetch.cmake needs ${required}")
    endif()
endforeach()
file(READ "${BLOOM_RECORD}" record)
string(JSON format GET "${record}" format)
string(JSON version GET "${record}" version)
if(NOT format STREQUAL "bloom.installer-tool" OR NOT version EQUAL 1)
    message(FATAL_ERROR "${BLOOM_RECORD} is not a version 1 installer tool record")
endif()
foreach(field IN ITEMS name release url sha256 sizeBytes)
    string(JSON ${field} GET "${record}" ${field})
endforeach()
string(JSON program GET "${record}" install program)
if(NOT url MATCHES "^https://[^ \t\r\n;]+$" OR NOT sha256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "${BLOOM_RECORD}: the URL or the checksum is malformed")
endif()
string(LENGTH "${sha256}" length)
if(NOT length EQUAL 64)
    message(FATAL_ERROR "${BLOOM_RECORD}: the checksum is not a SHA-256")
endif()
if(NOT program MATCHES "^[A-Za-z0-9_+-][A-Za-z0-9._+-]*$")
    message(FATAL_ERROR "${BLOOM_RECORD}: install.program is malformed")
endif()
set(switches "")
string(JSON switch_count LENGTH "${record}" install switches)
if(switch_count GREATER 0)
    math(EXPR switch_last "${switch_count} - 1")
    foreach(index RANGE 0 ${switch_last})
        string(JSON switch GET "${record}" install switches ${index})
        # Only the installer's own switches; the destination is added here.
        if(NOT switch MATCHES "^/[A-Z-]+(=[A-Za-z0-9]+)?$")
            message(FATAL_ERROR "${BLOOM_RECORD}: the switch '${switch}' is malformed")
        endif()
        list(APPEND switches "${switch}")
    endforeach()
endif()

string(REGEX REPLACE "^.*/" "" file "${url}")
set(text "${release} ${sha256} ${sizeBytes} ${file}\n")
set(note "${BLOOM_DESTINATION}/bloom-installer-tool.txt")
if(NOT BLOOM_CHECK_ONLY AND EXISTS "${note}" AND EXISTS "${BLOOM_DESTINATION}/${program}")
    file(READ "${note}" present)
    if(present STREQUAL text)
        message(STATUS "Bloom ${name}: ${BLOOM_DESTINATION} already holds release ${release}")
        return()
    endif()
endif()

function(bloom_tool_problem output file)
    set(${output} "" PARENT_SCOPE)
    file(SIZE "${file}" actual_size)
    if(NOT actual_size EQUAL sizeBytes)
        set(${output} "is ${actual_size} bytes; the record says ${sizeBytes}" PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${file}" actual_digest)
    if(NOT actual_digest STREQUAL sha256)
        set(${output} "does not match its SHA-256 in the record (${actual_digest})"
            PARENT_SCOPE)
    endif()
endfunction()

file(MAKE_DIRECTORY "${BLOOM_DOWNLOAD_DIR}")
set(cached "${BLOOM_DOWNLOAD_DIR}/${file}")
set(problem "is not in the archive cache")
if(EXISTS "${cached}")
    bloom_tool_problem(problem "${cached}")
endif()
if(problem AND BLOOM_ALLOW_DOWNLOADS)
    message(STATUS "Bloom ${name}: downloading release ${release}")
    file(DOWNLOAD "${url}" "${cached}.part" STATUS status TLS_VERIFY ON INACTIVITY_TIMEOUT 120)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${cached}.part")
        message(FATAL_ERROR "${url} could not be downloaded: ${status}")
    endif()
    bloom_tool_problem(problem "${cached}.part")
    if(problem)
        file(REMOVE "${cached}.part")
        message(FATAL_ERROR "${url} ${problem}. Nothing was installed.")
    endif()
    file(RENAME "${cached}.part" "${cached}")
endif()
if(problem)
    message(FATAL_ERROR "${cached} ${problem}. (Downloads: BLOOM_DEPENDENCY_ALLOW_DOWNLOADS.)")
endif()
if(BLOOM_CHECK_ONLY)
    message(STATUS "Bloom ${name}: the archive cache holds release ${release}")
    return()
endif()
if(NOT CMAKE_HOST_WIN32)
    message(STATUS "Bloom ${name}: ${cached} matches the record. ${name} is unpacked on "
                   "Windows only; nothing was installed on this host.")
    return()
endif()

# Run the installer into a sibling directory, then move it into place, so a stop in the middle
# leaves no half-filled destination.
set(staging "${BLOOM_DESTINATION}.unpacking")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}")
file(TO_NATIVE_PATH "${staging}" native_staging)
execute_process(
    COMMAND "${cached}" ${switches} "/DIR=${native_staging}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_output
    TIMEOUT 900)
if(NOT install_result EQUAL 0)
    file(REMOVE_RECURSE "${staging}")
    message(FATAL_ERROR "${cached} could not be installed (${install_result}):\n"
                        "${install_output}")
endif()
if(NOT EXISTS "${staging}/${program}")
    file(REMOVE_RECURSE "${staging}")
    message(FATAL_ERROR "${cached} did not install ${program}. Nothing was kept.")
endif()
file(REMOVE_RECURSE "${BLOOM_DESTINATION}")
file(RENAME "${staging}" "${BLOOM_DESTINATION}")
file(WRITE "${note}" "${text}")
message(STATUS "Bloom ${name}: release ${release} in ${BLOOM_DESTINATION}")

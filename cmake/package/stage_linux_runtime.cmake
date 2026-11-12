# Puts the distribution libraries a Linux Bloom package carries beside its Qt
# (dependencies/linux-runtime/packages.json; docs/architecture/platform-support.md, "Packages").
#
#   cmake -DROOT=<package root> -DRUNTIME=<dependency prefix>/linux-runtime
#         -DINVENTORY=<source tree>/dependencies/linux-runtime/packages.json
#         -DQT_DIR=<Qt directory, relative to the root>
#         -DRECORD_DIR=<record directory, relative to the root>
#         -P stage_linux_runtime.cmake
#
#   <QT_DIR>/lib/<library>                      each under its real name, with the link Qt asks
#                                               for; Qt's libraries and plugins search this
#                                               directory, so nothing of Qt is rewritten
#   <RECORD_DIR>/licenses/<package>/copyright   the package's own copyright file
#   <RECORD_DIR>/linux-runtime-packages.json    the inventory: which package, version and
#                                               checksum each library came from
#
# The prefix's directory must hold exactly what the inventory lists (the superbuild's
# linux-runtime-fetch.cmake writes it and its record); every library is compared with the
# inventory's SHA-256 of the file as it was packaged before the one change made to it, so the
# comparison here is against the record the fetch left. Runs after stage_qt.cmake and
# stage_records.cmake, which both start their directories afresh.
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS ROOT RUNTIME INVENTORY QT_DIR RECORD_DIR)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "stage_linux_runtime.cmake needs ${argument}")
    endif()
endforeach()
if(NOT EXISTS "${RUNTIME}/bloom-linux-runtime.txt")
    message(FATAL_ERROR "stage_linux_runtime.cmake: ${RUNTIME} is not the Linux runtime "
                        "directory of a dependency prefix (it has no bloom-linux-runtime.txt)")
endif()
file(READ "${INVENTORY}" inventory)
string(JSON distribution GET "${inventory}" distribution)
string(JSON architecture GET "${inventory}" architecture)
file(STRINGS "${RUNTIME}/bloom-linux-runtime.txt" present)

set(libraries "${ROOT}/${QT_DIR}/lib")
set(records "${ROOT}/${RECORD_DIR}")
if(NOT IS_DIRECTORY "${libraries}" OR NOT IS_DIRECTORY "${records}/licenses")
    message(FATAL_ERROR "stage_linux_runtime.cmake: the package has no Qt or no licenses yet")
endif()

string(JSON count LENGTH "${inventory}" packages)
math(EXPR last "${count} - 1")
math(EXPR expected_lines "${count} + 1")
list(LENGTH present present_lines)
list(GET present 0 heading)
if(NOT present_lines EQUAL expected_lines OR NOT heading STREQUAL "${distribution} ${architecture}")
    message(FATAL_ERROR "stage_linux_runtime.cmake: ${RUNTIME} does not hold the packages of "
                        "${INVENTORY}. Build the superbuild's bloom_dependency_linux_runtime.")
endif()
foreach(index RANGE 0 ${last})
    string(JSON package GET "${inventory}" packages ${index} package)
    string(JSON version GET "${inventory}" packages ${index} version)
    string(JSON digest GET "${inventory}" packages ${index} sha256)
    string(JSON size GET "${inventory}" packages ${index} sizeBytes)
    string(JSON file GET "${inventory}" packages ${index} library file)
    string(JSON name GET "${inventory}" packages ${index} library name)
    string(JSON copyright_digest GET "${inventory}" packages ${index} copyright sha256)
    if(NOT "${digest} ${size} ${package} ${version} ${file}" IN_LIST present)
        message(FATAL_ERROR "stage_linux_runtime.cmake: ${RUNTIME} was not made from "
                            "${package} ${version} as the inventory records it")
    endif()
    set(copyright "${RUNTIME}/licenses/${package}/copyright")
    if(NOT EXISTS "${RUNTIME}/lib/${file}" OR NOT EXISTS "${copyright}")
        message(FATAL_ERROR "stage_linux_runtime.cmake: ${RUNTIME} has no ${file} or no "
                            "copyright file of ${package}")
    endif()
    file(SHA256 "${copyright}" actual)
    if(NOT actual STREQUAL copyright_digest)
        message(FATAL_ERROR "stage_linux_runtime.cmake: ${package}: the copyright file is not "
                            "the one the inventory records")
    endif()
    if(EXISTS "${libraries}/${name}" OR EXISTS "${libraries}/${file}")
        message(FATAL_ERROR "stage_linux_runtime.cmake: the package already has a ${name}")
    endif()
    file(COPY "${RUNTIME}/lib/${file}" DESTINATION "${libraries}")
    if(NOT name STREQUAL file)
        file(CREATE_LINK "${file}" "${libraries}/${name}" SYMBOLIC)
    endif()
    file(COPY "${copyright}" DESTINATION "${records}/licenses/${package}")
endforeach()
file(COPY "${INVENTORY}" DESTINATION "${records}")
get_filename_component(inventory_name "${INVENTORY}" NAME)
file(RENAME "${records}/${inventory_name}" "${records}/linux-runtime-packages.json")
message(STATUS "Bloom package: carries ${count} libraries from ${distribution} packages")

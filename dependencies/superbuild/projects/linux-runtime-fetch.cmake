# Puts the distribution libraries a Linux Bloom package carries into the dependency prefix.
#
#   cmake -DBLOOM_INVENTORY=<dependencies/linux-runtime/packages.json>
#         -DBLOOM_SOURCE=<source tree> -DBLOOM_DESTINATION=<prefix>/linux-runtime
#         -DBLOOM_DOWNLOAD_DIR=<archive cache> -DBLOOM_ALLOW_DOWNLOADS=<ON|OFF>
#         -DBLOOM_PATCHELF=<patchelf> [-DBLOOM_CHECK_ONLY=ON] -P linux-runtime-fetch.cmake
#
# The inventory is the only place the packages are written: for each, the Ubuntu binary package
# with its version, the places it can be downloaded from, its SHA-256 and size, the one library
# taken from it with that file's SHA-256, and the package's copyright file as it is checked in.
#
# Every package is taken from the cache or, when downloads are allowed, from the first of its
# places that answers; its size and SHA-256 are compared with the inventory before it is
# unpacked, and a package that differs stops the build. The library inside must be the file the
# inventory records, and the package's own copyright file must be the one checked in, so the
# license text a Bloom package shows is the text that came with the library.
#
#   <destination>/lib/<library>                 under its real name, with the name programs ask
#                                               for as a link
#   <destination>/licenses/<package>/copyright
#   <destination>/bloom-linux-runtime.txt       what the directory holds
#
# One thing is changed in each library: its search path becomes its own directory ($ORIGIN). A
# distribution's library has none, because it lives where the loader looks anyway; inside a
# package it must find the libraries that came with it beside itself, not the system's.
#
# With BLOOM_CHECK_ONLY the packages are fetched and compared but nothing is unpacked. A
# destination whose record already says exactly what the inventory says is left untouched.
cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS BLOOM_INVENTORY BLOOM_SOURCE BLOOM_DESTINATION BLOOM_DOWNLOAD_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "linux-runtime-fetch.cmake needs ${required}")
    endif()
endforeach()
file(READ "${BLOOM_INVENTORY}" inventory)
string(JSON format GET "${inventory}" format)
string(JSON version GET "${inventory}" version)
if(NOT format STREQUAL "bloom.linux-runtime-packages" OR NOT version EQUAL 1)
    message(FATAL_ERROR "${BLOOM_INVENTORY} is not a version 1 Linux runtime package inventory")
endif()
string(JSON distribution GET "${inventory}" distribution)
string(JSON architecture GET "${inventory}" architecture)

set(record "${distribution} ${architecture}\n")
string(JSON count LENGTH "${inventory}" packages)
if(count EQUAL 0)
    message(FATAL_ERROR "${BLOOM_INVENTORY} lists no package")
endif()
math(EXPR last "${count} - 1")
foreach(index RANGE 0 ${last})
    foreach(field IN ITEMS package version sha256 sizeBytes)
        string(JSON ${field}_${index} GET "${inventory}" packages ${index} ${field})
    endforeach()
    string(JSON file_${index} GET "${inventory}" packages ${index} library file)
    string(JSON name_${index} GET "${inventory}" packages ${index} library name)
    string(JSON library_sha256_${index} GET "${inventory}" packages ${index} library sha256)
    string(JSON copyright_${index} GET "${inventory}" packages ${index} copyright path)
    string(JSON copyright_sha256_${index} GET "${inventory}" packages ${index} copyright sha256)
    string(JSON url_count LENGTH "${inventory}" packages ${index} urls)
    set(urls_${index} "")
    math(EXPR url_last "${url_count} - 1")
    foreach(url_index RANGE 0 ${url_last})
        string(JSON url GET "${inventory}" packages ${index} urls ${url_index})
        if(NOT url MATCHES "^https://[^ \t\r\n;]+/([^/ \t\r\n;?#]+\\.deb)$")
            message(FATAL_ERROR "${url} is not an HTTPS URL of a package file")
        endif()
        list(APPEND urls_${index} "${url}")
        set(archive_${index} "${CMAKE_MATCH_1}")
    endforeach()
    string(LENGTH "${sha256_${index}}" length)
    if(NOT length EQUAL 64 OR NOT sha256_${index} MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${package_${index}}: the inventory's checksum is not a SHA-256")
    endif()
    string(APPEND record "${sha256_${index}} ${sizeBytes_${index}} ${package_${index}} "
                         "${version_${index}} ${file_${index}}\n")
endforeach()

set(record_file "${BLOOM_DESTINATION}/bloom-linux-runtime.txt")
if(NOT BLOOM_CHECK_ONLY AND EXISTS "${record_file}")
    file(READ "${record_file}" present)
    if(present STREQUAL record)
        message(STATUS "Bloom Linux runtime: ${BLOOM_DESTINATION} already holds the "
                       "${count} packages of the inventory")
        return()
    endif()
endif()

function(bloom_package_problem output file digest size)
    set(${output} "" PARENT_SCOPE)
    file(SIZE "${file}" actual_size)
    if(NOT actual_size EQUAL size)
        set(${output} "is ${actual_size} bytes; the inventory records ${size}" PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${file}" actual_digest)
    if(NOT actual_digest STREQUAL digest)
        set(${output} "does not match its SHA-256 in the inventory (${actual_digest})"
            PARENT_SCOPE)
    endif()
endfunction()

# Every package is checked before any is unpacked.
file(MAKE_DIRECTORY "${BLOOM_DOWNLOAD_DIR}")
foreach(index RANGE 0 ${last})
    set(file "${BLOOM_DOWNLOAD_DIR}/${archive_${index}}")
    set(problem "is not in the archive cache")
    if(EXISTS "${file}")
        bloom_package_problem(problem "${file}" "${sha256_${index}}" "${sizeBytes_${index}}")
    endif()
    if(problem AND BLOOM_ALLOW_DOWNLOADS)
        foreach(url IN LISTS urls_${index})
            message(STATUS "Bloom Linux runtime: downloading ${archive_${index}}")
            file(DOWNLOAD "${url}" "${file}.part" STATUS status TLS_VERIFY ON
                 INACTIVITY_TIMEOUT 120)
            list(GET status 0 code)
            if(NOT code EQUAL 0)
                file(REMOVE "${file}.part")
                set(problem "could not be downloaded from ${url}: ${status}")
                continue()
            endif()
            bloom_package_problem(problem "${file}.part" "${sha256_${index}}"
                                  "${sizeBytes_${index}}")
            if(problem)
                # A place that serves other bytes is an error, not a reason to try the next.
                file(REMOVE "${file}.part")
                message(FATAL_ERROR "${url} ${problem}. Nothing was unpacked.")
            endif()
            file(RENAME "${file}.part" "${file}")
            break()
        endforeach()
    endif()
    if(problem)
        message(FATAL_ERROR "${file} ${problem}. Nothing was unpacked."
                            " (Downloads: BLOOM_DEPENDENCY_ALLOW_DOWNLOADS.)")
    endif()
endforeach()

if(BLOOM_CHECK_ONLY)
    message(STATUS "Bloom Linux runtime: the archive cache holds the ${count} packages of the "
                   "inventory")
    return()
endif()
if(NOT BLOOM_PATCHELF OR NOT EXISTS "${BLOOM_PATCHELF}")
    message(FATAL_ERROR "linux-runtime-fetch.cmake needs BLOOM_PATCHELF")
endif()

# Unpacked beside the destination and moved into place whole.
set(staging "${BLOOM_DESTINATION}.unpacking")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}/lib")
foreach(index RANGE 0 ${last})
    set(package "${package_${index}}")
    set(work "${staging}/work")
    file(REMOVE_RECURSE "${work}")
    file(MAKE_DIRECTORY "${work}/data")
    # A package file is an archive of archives: its data member holds the files.
    file(ARCHIVE_EXTRACT INPUT "${BLOOM_DOWNLOAD_DIR}/${archive_${index}}" DESTINATION "${work}"
         PATTERNS "data.tar*")
    file(GLOB data "${work}/data.tar*")
    list(LENGTH data data_count)
    if(NOT data_count EQUAL 1)
        message(FATAL_ERROR "${archive_${index}} has no single data member: ${data}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${data}" DESTINATION "${work}/data")

    file(GLOB_RECURSE library "${work}/data/*/${file_${index}}")
    list(LENGTH library library_count)
    if(NOT library_count EQUAL 1)
        message(FATAL_ERROR "${package}: no single ${file_${index}} in the package: ${library}")
    endif()
    file(SHA256 "${library}" actual)
    if(NOT actual STREQUAL library_sha256_${index})
        message(FATAL_ERROR "${package}: ${file_${index}} is not the file the inventory records")
    endif()

    set(shipped "${work}/data/usr/share/doc/${package}/copyright")
    set(checked_in "${BLOOM_SOURCE}/${copyright_${index}}")
    if(NOT EXISTS "${shipped}" OR NOT EXISTS "${checked_in}")
        message(FATAL_ERROR "${package}: no copyright file in the package or in the source tree "
                            "(${copyright_${index}})")
    endif()
    file(SHA256 "${shipped}" shipped_digest)
    file(SHA256 "${checked_in}" checked_in_digest)
    if(NOT shipped_digest STREQUAL copyright_sha256_${index}
       OR NOT checked_in_digest STREQUAL copyright_sha256_${index})
        message(FATAL_ERROR "${package}: the copyright file in the package, the one checked in "
                            "and the inventory's SHA-256 do not agree")
    endif()

    file(COPY "${library}" DESTINATION "${staging}/lib")
    execute_process(COMMAND "${BLOOM_PATCHELF}" --set-rpath "$ORIGIN"
                            "${staging}/lib/${file_${index}}"
                    RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "patchelf failed for ${file_${index}}: ${error}")
    endif()
    if(NOT name_${index} STREQUAL file_${index})
        file(CREATE_LINK "${file_${index}}" "${staging}/lib/${name_${index}}" SYMBOLIC)
    endif()
    file(COPY "${checked_in}" DESTINATION "${staging}/licenses/${package}")
    file(REMOVE_RECURSE "${work}")
endforeach()
file(WRITE "${staging}/bloom-linux-runtime.txt" "${record}")
file(REMOVE_RECURSE "${BLOOM_DESTINATION}")
file(RENAME "${staging}" "${BLOOM_DESTINATION}")
message(STATUS "Bloom Linux runtime: ${count} libraries from ${distribution} packages in "
               "${BLOOM_DESTINATION}")

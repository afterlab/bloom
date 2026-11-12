# Puts the official binaries the dependency lock names for one component into the prefix.
#
#   cmake -DBLOOM_LOCK=<dependencies.lock.json> -DBLOOM_COMPONENT=<name>
#         -DBLOOM_OPERATING_SYSTEM=<linux|macos|windows> -DBLOOM_DESTINATION=<prefix>/<name>
#         -DBLOOM_DOWNLOAD_DIR=<archive cache> -DBLOOM_ALLOW_DOWNLOADS=<ON|OFF>
#         [-DBLOOM_LIBRARY_ARCHIVES=<regex>] [-DBLOOM_CHECK_ONLY=ON] -P prebuilt-fetch.cmake
#
# The lock (schema 1.4) is the only place the archives are written: the component's build for the
# profile of this operating system carries a `prebuilt` list, each entry one archive's HTTPS URL,
# SHA-256 and size. Every archive is taken from the cache or, when downloads are allowed, from its
# URL; its size and SHA-256 are compared with the lock before anything is unpacked, and an archive
# that differs stops the build and is never unpacked. The archives are unpacked together into the
# destination exactly as published: no file is patched, renamed or relinked.
#
# An archive whose file name matches BLOOM_LIBRARY_ARCHIVES holds bare libraries and is unpacked
# into the destination's `lib` directory (the Unicode libraries the Linux binaries link).
#
# With BLOOM_CHECK_ONLY the archives are fetched and compared but nothing is unpacked.
#
# The destination records what it holds in `bloom-prebuilt.txt` (the component, its version and
# each archive's SHA-256, size and name). A destination whose record already says exactly what
# the lock says is left untouched, so nothing built against it is rebuilt.
cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS BLOOM_LOCK BLOOM_COMPONENT BLOOM_OPERATING_SYSTEM BLOOM_DESTINATION
                          BLOOM_DOWNLOAD_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "prebuilt-fetch.cmake needs ${required}")
    endif()
endforeach()
if(NOT EXISTS "${BLOOM_LOCK}")
    message(FATAL_ERROR "The dependency lock ${BLOOM_LOCK} does not exist")
endif()
file(READ "${BLOOM_LOCK}" lock)

# The profile of this operating system.
set(profile "")
string(JSON profile_count LENGTH "${lock}" profiles)
math(EXPR profile_last "${profile_count} - 1")
foreach(index RANGE 0 ${profile_last})
    string(JSON system GET "${lock}" profiles ${index} target operatingSystem)
    if(system STREQUAL BLOOM_OPERATING_SYSTEM)
        if(profile)
            message(FATAL_ERROR "The dependency lock has more than one ${BLOOM_OPERATING_SYSTEM} "
                                "profile; prebuilt-fetch.cmake cannot choose between them")
        endif()
        string(JSON profile GET "${lock}" profiles ${index} id)
    endif()
endforeach()
if(NOT profile)
    message(FATAL_ERROR "The dependency lock has no ${BLOOM_OPERATING_SYSTEM} profile")
endif()

# The component's build for that profile, and its archives.
set(version "")
set(archive_count -1)
string(JSON component_count LENGTH "${lock}" components)
math(EXPR component_last "${component_count} - 1")
foreach(index RANGE 0 ${component_last})
    string(JSON name GET "${lock}" components ${index} name)
    if(NOT name STREQUAL BLOOM_COMPONENT)
        continue()
    endif()
    string(JSON version GET "${lock}" components ${index} version)
    string(JSON build_count LENGTH "${lock}" components ${index} profileBuilds)
    math(EXPR build_last "${build_count} - 1")
    foreach(build RANGE 0 ${build_last})
        string(JSON build_profile GET "${lock}" components ${index} profileBuilds ${build}
               profileId)
        if(NOT build_profile STREQUAL profile)
            continue()
        endif()
        string(JSON archives ERROR_VARIABLE no_archives GET "${lock}" components ${index}
               profileBuilds ${build} prebuilt)
        if(no_archives)
            message(FATAL_ERROR "The dependency lock records no binary archives (prebuilt) of "
                                "${BLOOM_COMPONENT} for ${profile}")
        endif()
        string(JSON archive_count LENGTH "${archives}")
    endforeach()
endforeach()
if(version STREQUAL "")
    message(FATAL_ERROR "The dependency lock has no component ${BLOOM_COMPONENT}")
endif()
if(archive_count LESS 1)
    message(FATAL_ERROR "The dependency lock has no build of ${BLOOM_COMPONENT} for ${profile}")
endif()

set(record "${BLOOM_COMPONENT} ${version}\n")
if(BLOOM_LIBRARY_ARCHIVES)
    # The layout rule is part of what the destination holds.
    string(APPEND record "library archives ${BLOOM_LIBRARY_ARCHIVES}\n")
endif()
set(urls)
set(digests)
set(sizes)
set(names)
math(EXPR archive_last "${archive_count} - 1")
foreach(index RANGE 0 ${archive_last})
    string(JSON url GET "${archives}" ${index} url)
    string(JSON digest GET "${archives}" ${index} archiveSha256)
    string(JSON size GET "${archives}" ${index} archiveSizeBytes)
    if(NOT url MATCHES "^https://[^ \t\r\n;]+/([^/ \t\r\n;?#]+)$")
        message(FATAL_ERROR "${url} is not an HTTPS URL that ends in a file name")
    endif()
    set(name "${CMAKE_MATCH_1}")
    if(NOT digest MATCHES "^sha256:([0-9a-f]+)$")
        message(FATAL_ERROR "${name}: the lock's checksum is not a SHA-256")
    endif()
    set(digest "${CMAKE_MATCH_1}")
    string(LENGTH "${digest}" digest_length)
    if(NOT digest_length EQUAL 64)
        message(FATAL_ERROR "${name}: the lock's checksum is not a SHA-256")
    endif()
    if(name IN_LIST names)
        message(FATAL_ERROR "${name} is named twice by the lock")
    endif()
    list(APPEND urls "${url}")
    list(APPEND digests "${digest}")
    list(APPEND sizes "${size}")
    list(APPEND names "${name}")
    string(APPEND record "${digest} ${size} ${name}\n")
endforeach()

set(record_file "${BLOOM_DESTINATION}/bloom-prebuilt.txt")
if(EXISTS "${record_file}")
    file(READ "${record_file}" present)
    if(present STREQUAL record)
        message(STATUS "Bloom ${BLOOM_COMPONENT} ${version}: ${BLOOM_DESTINATION} already holds "
                       "the locked binaries")
        return()
    endif()
endif()

# Every archive is checked before any is unpacked.
function(bloom_archive_problem output file digest size)
    set(${output} "" PARENT_SCOPE)
    file(SIZE "${file}" actual_size)
    if(NOT actual_size EQUAL size)
        set(${output} "is ${actual_size} bytes; the lock records ${size}" PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${file}" actual_digest)
    if(NOT actual_digest STREQUAL digest)
        set(${output} "does not match its locked SHA-256 (${actual_digest}, locked ${digest})"
            PARENT_SCOPE)
    endif()
endfunction()

file(MAKE_DIRECTORY "${BLOOM_DOWNLOAD_DIR}")
foreach(index RANGE 0 ${archive_last})
    list(GET urls ${index} url)
    list(GET digests ${index} digest)
    list(GET sizes ${index} size)
    list(GET names ${index} name)
    set(file "${BLOOM_DOWNLOAD_DIR}/${name}")
    set(problem "is not in the archive cache")
    if(EXISTS "${file}")
        bloom_archive_problem(problem "${file}" "${digest}" "${size}")
    endif()
    if(problem AND BLOOM_ALLOW_DOWNLOADS)
        message(STATUS "Bloom ${BLOOM_COMPONENT} ${version}: downloading ${name}")
        file(DOWNLOAD "${url}" "${file}.part" STATUS status TLS_VERIFY ON
             INACTIVITY_TIMEOUT 120)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${file}.part")
            message(FATAL_ERROR "${url} could not be downloaded: ${status}")
        endif()
        bloom_archive_problem(problem "${file}.part" "${digest}" "${size}")
        if(problem)
            file(REMOVE "${file}.part")
            message(FATAL_ERROR "${url} ${problem}. Nothing was unpacked.")
        endif()
        file(RENAME "${file}.part" "${file}")
    endif()
    if(problem)
        message(FATAL_ERROR "${file} ${problem}. Nothing was unpacked."
                            " (Downloads are off: BLOOM_DEPENDENCY_ALLOW_DOWNLOADS.)")
    endif()
endforeach()

if(BLOOM_CHECK_ONLY)
    message(STATUS "Bloom ${BLOOM_COMPONENT} ${version}: the archive cache holds the "
                   "${archive_count} locked archives")
    return()
endif()

# Unpacked beside the destination and moved into place whole, so an interrupted run leaves no
# half-filled directory that looks complete.
set(staging "${BLOOM_DESTINATION}.unpacking")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}")
foreach(name IN LISTS names)
    set(directory "${staging}")
    if(BLOOM_LIBRARY_ARCHIVES AND name MATCHES "${BLOOM_LIBRARY_ARCHIVES}")
        set(directory "${staging}/lib")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${BLOOM_DOWNLOAD_DIR}/${name}" DESTINATION "${directory}")
endforeach()
file(WRITE "${staging}/bloom-prebuilt.txt" "${record}")
file(REMOVE_RECURSE "${BLOOM_DESTINATION}")
file(RENAME "${staging}" "${BLOOM_DESTINATION}")
message(STATUS "Bloom ${BLOOM_COMPONENT} ${version}: ${archive_count} locked archives unpacked "
               "into ${BLOOM_DESTINATION}")

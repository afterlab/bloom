# Tests what cmake/package/make_archives.cmake adds to an AppImage for its updates
# (docs/decisions/0041-self-update.md): the update information in the runtime and the control
# file beside the AppImage. The tree is a stand-in of a few small files, so the test costs
# nothing; the package's own contents are the package check's business. Where the system has no
# mksquashfs, a stand-in writes a placeholder for the image: what is tested here is in the
# runtime before the image and in the control file, and neither reads the image.
#
#   cmake -DSOURCE=<source tree> -DRUNTIME=<dependency prefix>/appimage-runtime
#         -DZSYNC_MAKE=<bloom-zsync-make> -DWORK=<work directory>
#         -P make_archives_test.cmake
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS SOURCE RUNTIME ZSYNC_MAKE WORK)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "make_archives_test.cmake needs ${argument}")
    endif()
endforeach()
find_program(BLOOM_MKSQUASHFS mksquashfs)
find_program(BLOOM_READELF readelf)
if(NOT BLOOM_READELF OR NOT IS_DIRECTORY "${RUNTIME}")
    message(STATUS "SKIPPED: readelf or the AppImage runtime is not available")
    return()
endif()

set(name "Bloom-0.0.0-test-linux-x86_64")
set(tree "${WORK}/tree/${name}")
set(linux "${SOURCE}/apps/bloom/linux")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${tree}/bin" "${tree}/share/bloom" "${tree}/share/applications"
                    "${tree}/share/icons/hicolor/256x256/apps")
file(WRITE "${tree}/bin/Bloom" "#!/bin/sh\necho started\n")
file(CHMOD "${tree}/bin/Bloom" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE "${tree}/share/bloom/dependencies.lock.json" "{}\n")
file(COPY "${linux}/id.afterlab.bloom.desktop" DESTINATION "${tree}/share/applications")
file(COPY "${linux}/icons/256x256/id.afterlab.bloom.png"
     DESTINATION "${tree}/share/icons/hicolor/256x256/apps")

if(NOT BLOOM_MKSQUASHFS)
    set(BLOOM_MKSQUASHFS "${WORK}/tools/mksquashfs")
    file(WRITE "${BLOOM_MKSQUASHFS}" "#!/bin/sh\nprintf 'a placeholder for the image' > \"$2\"\n")
    file(CHMOD "${BLOOM_MKSQUASHFS}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()

set(information "gh-releases-zsync|afterlab|bloom|alpha-channel|Bloom-*x86_64.AppImage.zsync")
set(epoch 1700000000)

# Runs the script; `result` and `error` hold what it said.
function(make output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DTREE=${tree}" "-DOUTPUT=${output}" "-DWORK=${output}-work"
                "-DSOURCE=${SOURCE}" "-DRUNTIME=${RUNTIME}" "-DEPOCH=${epoch}"
                "-DBLOOM_MKSQUASHFS=${BLOOM_MKSQUASHFS}" ${ARGN}
                -P "${CMAKE_CURRENT_LIST_DIR}/make_archives.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE made ERROR_VARIABLE error)
    set(result "${result}" PARENT_SCOPE)
    set(error "${made}${error}" PARENT_SCOPE)
endfunction()

# What the AppImage's own runtime says about itself.
function(runtime_says appimage option variable)
    execute_process(COMMAND "${appimage}" "${option}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE said ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${appimage} ${option} failed: ${error}")
    endif()
    string(STRIP "${said}" said)
    set(${variable} "${said}" PARENT_SCOPE)
endfunction()

# 1. With both: the runtime carries the text and the image still starts where the runtime ends.
make("${WORK}/with" "-DUPDATE_INFORMATION=${information}" "-DZSYNC_MAKE=${ZSYNC_MAKE}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the archives with update information could not be made: ${error}")
endif()
set(appimage "${WORK}/with/${name}.AppImage")
set(control "${appimage}.zsync")
runtime_says("${appimage}" --appimage-updateinformation said)
if(NOT said STREQUAL information)
    message(FATAL_ERROR "the AppImage's runtime gives the update information '${said}', not "
                        "'${information}'")
endif()
file(GLOB runtimes "${RUNTIME}/runtime-*")
list(GET runtimes 0 runtime)
file(SIZE "${runtime}" runtime_size)
runtime_says("${appimage}" --appimage-offset offset)
if(NOT offset EQUAL runtime_size)
    message(FATAL_ERROR "the image starts at ${offset}, not at the runtime's end "
                        "(${runtime_size})")
endif()

# 2. The control file describes that AppImage under its published name.
if(NOT EXISTS "${control}")
    message(FATAL_ERROR "no control file was written beside the AppImage")
endif()
file(SIZE "${appimage}" size)
file(SHA1 "${appimage}" digest)
# The header is text up to the first empty line; block checksums follow as bytes.
file(READ "${control}" header_hex HEX)
string(FIND "${header_hex}" "0a0a" header_end)
if(header_end EQUAL -1)
    message(FATAL_ERROR "the control file has no header")
endif()
string(SUBSTRING "${header_hex}" 0 ${header_end} header_hex)
string(REGEX MATCHALL "[0-9a-f][0-9a-f]" header_bytes "${header_hex}")
set(header "")
foreach(byte IN LISTS header_bytes)
    if(byte STREQUAL "0a")
        string(APPEND header "\n")
    else()
        math(EXPR code "0x${byte}")
        string(ASCII ${code} character)
        string(APPEND header "${character}")
    endif()
endforeach()
foreach(line IN ITEMS "zsync: 0.6.6" "Filename: ${name}.AppImage" "URL: ${name}.AppImage"
                      "Length: ${size}" "SHA-1: ${digest}"
                      "MTime: Tue, 14 Nov 2023 22:13:20 +0000")
    string(FIND "\n${header}\n" "\n${line}\n" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "the control file's header has no line '${line}':\n${header}")
    endif()
endforeach()

# 3. Without either, the runtime is the published one and no control file is written.
make("${WORK}/without")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the archives without update information could not be made: ${error}")
endif()
runtime_says("${WORK}/without/${name}.AppImage" --appimage-updateinformation said)
if(NOT said STREQUAL "" OR EXISTS "${WORK}/without/${name}.AppImage.zsync")
    message(FATAL_ERROR "an AppImage made without update information carries '${said}' or has "
                        "a control file")
endif()

# 4. A text the section cannot hold, or one with a character it cannot carry, is refused.
string(REPEAT "a" 2000 long)
foreach(refused IN ITEMS "${long}" "zsync|two words" "zsync|new\nline")
    make("${WORK}/refused" "-DUPDATE_INFORMATION=${refused}")
    if(result EQUAL 0 OR EXISTS "${WORK}/refused/${name}.AppImage")
        message(FATAL_ERROR "this update information was accepted: '${refused}'")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")
message(STATUS "make_archives_test: the update information and the control file are right")

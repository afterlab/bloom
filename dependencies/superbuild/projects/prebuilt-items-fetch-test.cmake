# Tests update-runtime-fetch.cmake and installer-tools-fetch.cmake without the network (the
# bloom.quality.prebuilt_items_fetch test).
#
#   cmake -DBLOOM_WORK=<empty scratch directory> -P prebuilt-items-fetch-test.cmake
#
# Synthetic records name small archives and a stand-in installer made here. With downloads off
# each script must install only the record's kept paths from an archive that is the record's,
# keep a framework's symbolic links (where the host can make them), leave an up-to-date
# destination untouched, accept the check-only mode, and refuse, installing and unpacking
# nothing, a file with a wrong checksum, a wrong size, or one that is not in the cache. The
# installer tool is only run on Windows, so there the stand-in is never started: on a Windows host
# only its refusals and its check-only mode are tried, and on other hosts its good case ends with
# the message that it is unpacked on Windows only.
cmake_minimum_required(VERSION 3.25)
if(NOT BLOOM_WORK)
    message(FATAL_ERROR "prebuilt-items-fetch-test.cmake needs BLOOM_WORK")
endif()
set(update_script "${CMAKE_CURRENT_LIST_DIR}/update-runtime-fetch.cmake")
set(tool_script "${CMAKE_CURRENT_LIST_DIR}/installer-tools-fetch.cmake")
file(REMOVE_RECURSE "${BLOOM_WORK}")
file(MAKE_DIRECTORY "${BLOOM_WORK}/cache" "${BLOOM_WORK}/records")

# A framework-like tree (with a link, where the host has them), tools that are not kept, and
# the same under a root directory in a zip.
set(content "${BLOOM_WORK}/content")
file(MAKE_DIRECTORY "${content}/Thing.framework/Versions/B" "${content}/bin" "${content}/root/x64"
     "${content}/root/include" "${content}/root/pdb")
file(WRITE "${content}/Thing.framework/Versions/B/Thing" "the framework\n")
file(WRITE "${content}/bin/sign_tool" "a tool that is not kept\n")
file(WRITE "${content}/LICENSE" "the license\n")
file(WRITE "${content}/root/x64/Thing.dll" "the library\n")
file(WRITE "${content}/root/include/thing.h" "the header\n")
file(WRITE "${content}/root/pdb/Thing.pdb" "symbols that are not kept\n")
file(WRITE "${content}/root/COPYING" "the license\n")
set(have_links FALSE)
if(NOT CMAKE_HOST_WIN32)
    file(CREATE_LINK "Versions/B/Thing" "${content}/Thing.framework/Thing" SYMBOLIC
         RESULT link_result)
    if(link_result STREQUAL "0")
        set(have_links TRUE)
    endif()
endif()
file(ARCHIVE_CREATE OUTPUT "${BLOOM_WORK}/cache/Thing-1.0.tar.xz"
     PATHS Thing.framework bin LICENSE FORMAT gnutar COMPRESSION XZ
     WORKING_DIRECTORY "${content}")
file(ARCHIVE_CREATE OUTPUT "${BLOOM_WORK}/cache/Other-2.0.zip" PATHS root FORMAT zip
     WORKING_DIRECTORY "${content}")
file(WRITE "${BLOOM_WORK}/cache/tool-3.0.exe" "a stand-in for an installer\n")

function(digest_of file output_digest output_size)
    file(SHA256 "${file}" digest)
    file(SIZE "${file}" size)
    set(${output_digest} "${digest}" PARENT_SCOPE)
    set(${output_size} "${size}" PARENT_SCOPE)
endfunction()

function(wrong_digest digest output)
    string(SUBSTRING "${digest}" 0 1 first)
    string(SUBSTRING "${digest}" 1 -1 rest)
    if(first STREQUAL "0")
        set(${output} "1${rest}" PARENT_SCOPE)
    else()
        set(${output} "0${rest}" PARENT_SCOPE)
    endif()
endfunction()

# The records. `digest` and `size` are the values the record states.
function(write_update_record path file digest size keeps root)
    set(root_field "")
    if(root)
        set(root_field "\"archiveRoot\":\"${root}\",")
    endif()
    file(WRITE "${path}" "{\"format\":\"bloom.update-runtime\",\"version\":1,\"name\":\"Thing\",
 \"release\":\"1.0\",\"url\":\"https://example.invalid/a/${file}\",\"sha256\":\"${digest}\",
 \"sizeBytes\":${size},${root_field}\"keeps\":[${keeps}]}")
endfunction()

function(write_tool_record path file digest size)
    file(WRITE "${path}" "{\"format\":\"bloom.installer-tool\",\"version\":1,\"name\":\"Tool\",
 \"release\":\"3.0\",\"url\":\"https://example.invalid/a/${file}\",\"sha256\":\"${digest}\",
 \"sizeBytes\":${size},\"install\":{\"program\":\"TOOLC.exe\",
 \"switches\":[\"/VERYSILENT\",\"/CURRENTUSER\",\"/PORTABLE=1\"]}}")
endfunction()

function(run script record destination result_variable output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBLOOM_RECORD=${record}" "-DBLOOM_DESTINATION=${destination}"
                "-DBLOOM_DOWNLOAD_DIR=${BLOOM_WORK}/cache" -DBLOOM_ALLOW_DOWNLOADS=OFF ${ARGN}
                -P "${script}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

function(expect_refusal what script record destination expected)
    run("${script}" "${record}" "${destination}" result output ${ARGN})
    if(result EQUAL 0)
        message(FATAL_ERROR "${what}: the fetch succeeded")
    endif()
    if(NOT output MATCHES "${expected}")
        message(FATAL_ERROR "${what}: refused without the reason (${expected}):\n${output}")
    endif()
    if(EXISTS "${destination}" OR EXISTS "${destination}.unpacking")
        message(FATAL_ERROR "${what}: something was installed")
    endif()
    message(STATUS "refused as expected: ${what}")
endfunction()

# ---- update-runtime-fetch.cmake: a tar.xz, kept paths at the top of the archive ----
digest_of("${BLOOM_WORK}/cache/Thing-1.0.tar.xz" tar_digest tar_size)
set(tar_keeps "\"Thing.framework\",\"LICENSE\"")
set(tar_record "${BLOOM_WORK}/records/tar.json")
set(tar_destination "${BLOOM_WORK}/prefix/tar")

wrong_digest("${tar_digest}" tar_wrong_digest)
write_update_record("${tar_record}" Thing-1.0.tar.xz "${tar_wrong_digest}" "${tar_size}"
                    "${tar_keeps}" "")
expect_refusal("an archive whose SHA-256 is not the record's" "${update_script}" "${tar_record}"
               "${tar_destination}" "does not match its SHA-256 in the record")
math(EXPR tar_wrong_size "${tar_size} + 1")
write_update_record("${tar_record}" Thing-1.0.tar.xz "${tar_digest}" "${tar_wrong_size}"
                    "${tar_keeps}" "")
expect_refusal("an archive whose size is not the record's" "${update_script}" "${tar_record}"
               "${tar_destination}" "the record says ${tar_wrong_size}")
write_update_record("${tar_record}" Absent-1.0.tar.xz "${tar_digest}" "${tar_size}"
                    "${tar_keeps}" "")
expect_refusal("an archive that is not in the cache" "${update_script}" "${tar_record}"
               "${tar_destination}" "is not in the archive cache")
write_update_record("${tar_record}" Absent-1.0.tar.xz "${tar_digest}" "${tar_size}"
                    "${tar_keeps}" "")
expect_refusal("check-only with an archive that is not in the cache" "${update_script}"
               "${tar_record}" "${tar_destination}" "is not in the archive cache"
               -DBLOOM_CHECK_ONLY=ON)
write_update_record("${tar_record}" Thing-1.0.tar.xz "${tar_digest}" "${tar_size}"
                    "\"Thing.framework\",\"../outside\"" "")
expect_refusal("a kept path that leaves the destination" "${update_script}" "${tar_record}"
               "${tar_destination}" "the kept path '../outside' is malformed")
write_update_record("${tar_record}" Thing-1.0.tar.xz "${tar_digest}" "${tar_size}"
                    "\"Thing.framework\",\"Missing.framework\"" "")
expect_refusal("a kept path the archive does not hold" "${update_script}" "${tar_record}"
               "${tar_destination}" "(could not be unpacked|does not hold Missing.framework)")

write_update_record("${tar_record}" Thing-1.0.tar.xz "${tar_digest}" "${tar_size}"
                    "${tar_keeps}" "")
run("${update_script}" "${tar_record}" "${tar_destination}" result output -DBLOOM_CHECK_ONLY=ON)
if(NOT result EQUAL 0 OR EXISTS "${tar_destination}")
    message(FATAL_ERROR "check-only did not accept the cached archive, or installed it:\n${output}")
endif()
run("${update_script}" "${tar_record}" "${tar_destination}" result output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the record's archive was refused:\n${output}")
endif()
foreach(path IN ITEMS Thing.framework/Versions/B/Thing LICENSE bloom-update-runtime.txt)
    if(NOT EXISTS "${tar_destination}/${path}")
        message(FATAL_ERROR "${path} was not installed:\n${output}")
    endif()
endforeach()
if(EXISTS "${tar_destination}/bin" OR EXISTS "${tar_destination}.unpacking")
    message(FATAL_ERROR "something that is not kept was installed:\n${output}")
endif()
if(have_links)
    # EXISTS would follow the link; its own value shows it is still a link.
    file(READ_SYMLINK "${tar_destination}/Thing.framework/Thing" link_target)
    if(NOT link_target STREQUAL "Versions/B/Thing")
        message(FATAL_ERROR "the framework's symbolic link was not kept: '${link_target}'")
    endif()
endif()
file(READ "${tar_destination}/bloom-update-runtime.txt" note)
if(NOT note STREQUAL "1.0 ${tar_digest} ${tar_size} Thing-1.0.tar.xz\n")
    message(FATAL_ERROR "the destination's note is not the record's:\n${note}")
endif()

# An up-to-date destination is left untouched (a marker file survives); one that lacks a kept
# path is replaced.
file(WRITE "${tar_destination}/marker" "")
run("${update_script}" "${tar_record}" "${tar_destination}" result output)
if(NOT result EQUAL 0 OR NOT EXISTS "${tar_destination}/marker")
    message(FATAL_ERROR "an up-to-date destination was not left untouched:\n${output}")
endif()
file(REMOVE "${tar_destination}/LICENSE")
run("${update_script}" "${tar_record}" "${tar_destination}" result output)
if(NOT result EQUAL 0 OR EXISTS "${tar_destination}/marker"
   OR NOT EXISTS "${tar_destination}/LICENSE")
    message(FATAL_ERROR "a destination that lacked a kept path was not replaced:\n${output}")
endif()

# ---- update-runtime-fetch.cmake: a zip, kept paths below the archive's root directory ----
digest_of("${BLOOM_WORK}/cache/Other-2.0.zip" zip_digest zip_size)
set(zip_keeps "\"include/thing.h\",\"x64/Thing.dll\",\"COPYING\"")
set(zip_record "${BLOOM_WORK}/records/zip.json")
set(zip_destination "${BLOOM_WORK}/prefix/zip")
write_update_record("${zip_record}" Other-2.0.zip "${zip_digest}" "${zip_size}" "${zip_keeps}"
                    "root")
run("${update_script}" "${zip_record}" "${zip_destination}" result output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the record's zip was refused:\n${output}")
endif()
foreach(path IN ITEMS include/thing.h x64/Thing.dll COPYING bloom-update-runtime.txt)
    if(NOT EXISTS "${zip_destination}/${path}")
        message(FATAL_ERROR "${path} was not installed from the zip:\n${output}")
    endif()
endforeach()
if(EXISTS "${zip_destination}/pdb" OR EXISTS "${zip_destination}/root")
    message(FATAL_ERROR "something that is not kept was installed from the zip:\n${output}")
endif()

# ---- installer-tools-fetch.cmake ----
digest_of("${BLOOM_WORK}/cache/tool-3.0.exe" tool_digest tool_size)
set(tool_record "${BLOOM_WORK}/records/tool.json")
set(tool_destination "${BLOOM_WORK}/prefix/tool")
wrong_digest("${tool_digest}" tool_wrong_digest)
write_tool_record("${tool_record}" tool-3.0.exe "${tool_wrong_digest}" "${tool_size}")
expect_refusal("an installer whose SHA-256 is not the record's" "${tool_script}" "${tool_record}"
               "${tool_destination}" "does not match its SHA-256 in the record")
math(EXPR tool_wrong_size "${tool_size} + 1")
write_tool_record("${tool_record}" tool-3.0.exe "${tool_digest}" "${tool_wrong_size}")
expect_refusal("an installer whose size is not the record's" "${tool_script}" "${tool_record}"
               "${tool_destination}" "the record says ${tool_wrong_size}")
write_tool_record("${tool_record}" absent-3.0.exe "${tool_digest}" "${tool_size}")
expect_refusal("an installer that is not in the cache" "${tool_script}" "${tool_record}"
               "${tool_destination}" "is not in the archive cache")

write_tool_record("${tool_record}" absent-3.0.exe "${tool_digest}" "${tool_size}")
expect_refusal("check-only with an installer that is not in the cache" "${tool_script}"
               "${tool_record}" "${tool_destination}" "is not in the archive cache"
               -DBLOOM_CHECK_ONLY=ON)
write_tool_record("${tool_record}" tool-3.0.exe "${tool_digest}" "${tool_size}")
run("${tool_script}" "${tool_record}" "${tool_destination}" result output -DBLOOM_CHECK_ONLY=ON)
if(NOT result EQUAL 0 OR EXISTS "${tool_destination}")
    message(FATAL_ERROR "check-only did not accept the cached installer, or installed it:\n${output}")
endif()
if(NOT CMAKE_HOST_WIN32)
    run("${tool_script}" "${tool_record}" "${tool_destination}" result output)
    if(NOT result EQUAL 0 OR NOT output MATCHES "is unpacked on Windows only"
       OR EXISTS "${tool_destination}" OR EXISTS "${tool_destination}.unpacking")
        message(FATAL_ERROR "a host that is not Windows installed the tool, or did not say "
                            "so:\n${output}")
    endif()
endif()

file(REMOVE_RECURSE "${BLOOM_WORK}")
message(STATUS "update-runtime-fetch.cmake and installer-tools-fetch.cmake: every case passed")

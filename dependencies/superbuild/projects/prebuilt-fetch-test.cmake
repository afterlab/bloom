# Tests prebuilt-fetch.cmake without the network (the bloom.quality.prebuilt_fetch test).
#
#   cmake -DBLOOM_WORK=<empty scratch directory> -P prebuilt-fetch-test.cmake
#
# A synthetic lock names two small archives made here. With downloads off the script must unpack
# archives that are the lock's, leave an up-to-date destination untouched, and refuse, unpacking
# nothing, an archive whose bytes are not the ones the lock records: a wrong checksum, a wrong
# size, and an archive that is not in the cache.
cmake_minimum_required(VERSION 3.25)
if(NOT BLOOM_WORK)
    message(FATAL_ERROR "prebuilt-fetch-test.cmake needs BLOOM_WORK")
endif()
set(script "${CMAKE_CURRENT_LIST_DIR}/prebuilt-fetch.cmake")
file(REMOVE_RECURSE "${BLOOM_WORK}")
file(MAKE_DIRECTORY "${BLOOM_WORK}/content/bin" "${BLOOM_WORK}/libraries" "${BLOOM_WORK}/cache")
file(WRITE "${BLOOM_WORK}/content/bin/program.txt" "the program\n")
file(WRITE "${BLOOM_WORK}/libraries/library.txt" "the library\n")
file(ARCHIVE_CREATE OUTPUT "${BLOOM_WORK}/cache/base.7z" PATHS bin FORMAT 7zip
     WORKING_DIRECTORY "${BLOOM_WORK}/content")
file(ARCHIVE_CREATE OUTPUT "${BLOOM_WORK}/cache/icu-libraries.7z" PATHS library.txt FORMAT 7zip
     WORKING_DIRECTORY "${BLOOM_WORK}/libraries")

function(write_lock base_digest base_size)
    file(SHA256 "${BLOOM_WORK}/cache/icu-libraries.7z" libraries_digest)
    file(SIZE "${BLOOM_WORK}/cache/icu-libraries.7z" libraries_size)
    file(WRITE "${BLOOM_WORK}/lock.json" "{\"profiles\":[
 {\"id\":\"bloom.other\",\"target\":{\"operatingSystem\":\"macos\"}},
 {\"id\":\"bloom.synthetic\",\"target\":{\"operatingSystem\":\"linux\"}}],
 \"components\":[{\"name\":\"other\",\"version\":\"1\",\"profileBuilds\":[]},
 {\"name\":\"synthetic\",\"version\":\"1.2.3\",\"profileBuilds\":[
  {\"profileId\":\"bloom.other\"},
  {\"profileId\":\"bloom.synthetic\",\"prebuilt\":[
   {\"url\":\"https://example.invalid/a/base.7z\",\"archiveSha256\":\"sha256:${base_digest}\",
    \"archiveSizeBytes\":${base_size}},
   {\"url\":\"https://example.invalid/a/icu-libraries.7z\",
    \"archiveSha256\":\"sha256:${libraries_digest}\",\"archiveSizeBytes\":${libraries_size}}]}]}]}")
endfunction()

function(fetch result_variable output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBLOOM_LOCK=${BLOOM_WORK}/lock.json"
                -DBLOOM_COMPONENT=synthetic -DBLOOM_OPERATING_SYSTEM=linux
                "-DBLOOM_DESTINATION=${BLOOM_WORK}/prefix/synthetic"
                "-DBLOOM_DOWNLOAD_DIR=${BLOOM_WORK}/cache" -DBLOOM_ALLOW_DOWNLOADS=OFF
                "-DBLOOM_LIBRARY_ARCHIVES=^icu-" ${ARGN} -P "${script}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

function(expect_refusal what expected)
    fetch(result output ${ARGN})
    if(result EQUAL 0)
        message(FATAL_ERROR "${what}: the fetch succeeded")
    endif()
    if(NOT output MATCHES "${expected}")
        message(FATAL_ERROR "${what}: refused without the reason (${expected}):\n${output}")
    endif()
    if(EXISTS "${BLOOM_WORK}/prefix/synthetic" OR EXISTS "${BLOOM_WORK}/prefix/synthetic.unpacking")
        message(FATAL_ERROR "${what}: something was unpacked")
    endif()
    message(STATUS "refused as expected: ${what}")
endfunction()

file(SHA256 "${BLOOM_WORK}/cache/base.7z" base_digest)
file(SIZE "${BLOOM_WORK}/cache/base.7z" base_size)

# A wrong checksum: one digit of the archive's own SHA-256 changed.
string(SUBSTRING "${base_digest}" 0 1 first)
string(SUBSTRING "${base_digest}" 1 -1 rest)
if(first STREQUAL "0")
    set(wrong_digest "1${rest}")
else()
    set(wrong_digest "0${rest}")
endif()
write_lock("${wrong_digest}" "${base_size}")
expect_refusal("an archive whose SHA-256 is not the lock's" "does not match its locked SHA-256")

math(EXPR wrong_size "${base_size} + 1")
write_lock("${base_digest}" "${wrong_size}")
expect_refusal("an archive whose size is not the lock's" "the lock records ${wrong_size}")

write_lock("${base_digest}" "${base_size}")
file(RENAME "${BLOOM_WORK}/cache/base.7z" "${BLOOM_WORK}/base.7z")
expect_refusal("an archive that is not in the cache" "is not in the archive cache")
file(RENAME "${BLOOM_WORK}/base.7z" "${BLOOM_WORK}/cache/base.7z")

expect_refusal("a component the lock does not have" "has no component absent"
               -DBLOOM_COMPONENT=absent)
expect_refusal("a build without binary archives" "records no binary archives"
               -DBLOOM_OPERATING_SYSTEM=macos)

# The lock's own archives are unpacked, the library archive into lib.
fetch(result output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the locked archives were refused:\n${output}")
endif()
foreach(file IN ITEMS bin/program.txt lib/library.txt bloom-prebuilt.txt)
    if(NOT EXISTS "${BLOOM_WORK}/prefix/synthetic/${file}")
        message(FATAL_ERROR "${file} was not unpacked:\n${output}")
    endif()
endforeach()
file(READ "${BLOOM_WORK}/prefix/synthetic/bloom-prebuilt.txt" record)
if(NOT record MATCHES "^synthetic 1\\.2\\.3\nlibrary archives [^\n]*\n${base_digest} ${base_size} base\\.7z\n")
    message(FATAL_ERROR "the destination's record is not the lock's:\n${record}")
endif()

# An up-to-date destination is left untouched (a marker file survives); a lock that names other
# bytes replaces it.
file(WRITE "${BLOOM_WORK}/prefix/synthetic/marker" "")
fetch(result output)
if(NOT result EQUAL 0 OR NOT EXISTS "${BLOOM_WORK}/prefix/synthetic/marker")
    message(FATAL_ERROR "an up-to-date destination was not left untouched:\n${output}")
endif()
file(WRITE "${BLOOM_WORK}/content/bin/program.txt" "another program\n")
file(ARCHIVE_CREATE OUTPUT "${BLOOM_WORK}/cache/base.7z" PATHS bin FORMAT 7zip
     WORKING_DIRECTORY "${BLOOM_WORK}/content")
file(SHA256 "${BLOOM_WORK}/cache/base.7z" base_digest)
file(SIZE "${BLOOM_WORK}/cache/base.7z" base_size)
write_lock("${base_digest}" "${base_size}")
fetch(result output)
file(READ "${BLOOM_WORK}/prefix/synthetic/bin/program.txt" program)
if(NOT result EQUAL 0 OR EXISTS "${BLOOM_WORK}/prefix/synthetic/marker"
   OR NOT program STREQUAL "another program\n")
    message(FATAL_ERROR "a destination that held other archives was not replaced:\n${output}")
endif()
file(REMOVE_RECURSE "${BLOOM_WORK}")
message(STATUS "prebuilt-fetch.cmake: every case passed")

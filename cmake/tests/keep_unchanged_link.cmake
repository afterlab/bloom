# The link launcher that keeps an unchanged program's file (cmake/BloomKeepUnchangedLink.cmake,
# #727), run with a stand-in linker that copies a given file to its `-o` output. Whether the old
# file was kept is seen through a hard link made to it before the link: a kept file is still that
# file, a replaced one is not.
#
#   cmake -DMODULE_DIR=<cmake/> -DWORK=<empty folder in the build tree> -P <this file>
cmake_minimum_required(VERSION 3.25)
if(NOT MODULE_DIR OR NOT WORK)
    message(FATAL_ERROR "keep_unchanged_link.cmake needs MODULE_DIR and WORK")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(output "${WORK}/program")
set(previous "${output}.bloom-previous-link")
set(seen "${WORK}/linker-saw-output")

# The stand-in linker: records whether the output path existed when it ran, then copies its input
# to the output as a new file, or fails without writing one.
file(WRITE "${WORK}/linker.cmake" [=[
cmake_minimum_required(VERSION 3.25)
set(input "${CMAKE_ARGV5}")
set(output "${CMAKE_ARGV7}")
if(EXISTS "${output}")
    file(WRITE "${SEEN}" "existing")
else()
    file(WRITE "${SEEN}" "absent")
endif()
if(input STREQUAL "fail")
    message(FATAL_ERROR "the stand-in linker fails")
endif()
file(COPY_FILE "${input}" "${output}")
]=])
file(WRITE "${WORK}/one" "the first program\n")
file(WRITE "${WORK}/two" "the second program\n")

set(failures "")
macro(fail what)
    string(APPEND failures "\n${what}")
endmacro()

function(link input result_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${MODULE_DIR}/BloomKeepUnchangedLink.cmake" --
            "${CMAKE_COMMAND}" "-DSEEN=${seen}" -P "${WORK}/linker.cmake" -- "${input}" -o
            "${output}"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    set(${result_variable} "${result}" PARENT_SCOPE)
endfunction()

function(read_file path variable)
    if(EXISTS "${path}")
        file(READ "${path}" text)
    else()
        set(text "<missing>")
    endif()
    set(${variable} "${text}" PARENT_SCOPE)
endfunction()

# Whether <witness> (a hard link made earlier) is still the same file as the output: a line added
# through one is read through the other.
function(same_file witness variable)
    file(APPEND "${witness}" "marked\n")
    read_file("${output}" text)
    if(text MATCHES "marked\n$")
        set(${variable} TRUE PARENT_SCOPE)
    else()
        set(${variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

# 1. First link: there is nothing to keep; the linker writes the program.
link("${WORK}/one" result)
read_file("${output}" text)
if(NOT result STREQUAL "0" OR NOT text STREQUAL "the first program\n")
    fail("a first link did not write the program (exit ${result}, '${text}')")
endif()
if(EXISTS "${previous}")
    fail("a first link left ${previous}")
endif()

# 2. The same bytes again: the linker writes into the output path (the path goes into a real
# program), and the old file is put back and marked as just built.
file(CREATE_LINK "${output}" "${WORK}/witness-same")
file(TIMESTAMP "${output}" before "%Y%m%d%H%M%S%f" UTC)
link("${WORK}/one" result)
read_file("${seen}" linker_saw)
file(TIMESTAMP "${output}" after "%Y%m%d%H%M%S%f" UTC)
read_file("${output}" text)
if(NOT result STREQUAL "0" OR NOT text STREQUAL "the first program\n")
    fail("an unchanged relink failed or changed the program (exit ${result}, '${text}')")
endif()
if(NOT linker_saw STREQUAL "absent")
    fail("the linker did not write a new file into the output path (it saw the path ${linker_saw})")
endif()
if(NOT after STRGREATER before)
    fail("the kept program was not marked as just built (${before} then ${after})")
endif()
if(EXISTS "${previous}")
    fail("an unchanged relink left ${previous}")
endif()
same_file("${WORK}/witness-same" kept)
if(NOT kept)
    fail("an unchanged relink replaced the program's file instead of keeping it")
endif()

# 3. Other bytes: the linker's new file replaces the old one.
file(CREATE_LINK "${output}" "${WORK}/witness-changed")
link("${WORK}/two" result)
read_file("${output}" text)
if(NOT result STREQUAL "0" OR NOT text STREQUAL "the second program\n")
    fail("a changed relink did not install the new program (exit ${result}, '${text}')")
endif()
if(EXISTS "${previous}")
    fail("a changed relink left ${previous}")
endif()
same_file("${WORK}/witness-changed" kept)
if(kept)
    fail("a changed relink kept the old file")
endif()

# 4. A failed link fails, and the program from before stays where it was.
file(WRITE "${output}" "the second program\n")
link("fail" result)
read_file("${output}" text)
if(result STREQUAL "0")
    fail("a failed link was reported as a success")
endif()
if(NOT text STREQUAL "the second program\n")
    fail("a failed link did not leave the program from before ('${text}')")
endif()
if(EXISTS "${previous}")
    fail("a failed link left ${previous}")
endif()

# 5. A link stopped after the old file was moved aside: the next link puts it back first, so an
# unchanged program is still kept.
file(RENAME "${output}" "${previous}")
file(CREATE_LINK "${previous}" "${WORK}/witness-stopped")
link("${WORK}/two" result)
read_file("${output}" text)
if(NOT result STREQUAL "0" OR NOT text STREQUAL "the second program\n" OR EXISTS "${previous}")
    fail("the link after a stopped one did not restore the program (exit ${result}, '${text}')")
endif()
same_file("${WORK}/witness-stopped" kept)
if(NOT kept)
    fail("the link after a stopped one replaced the unchanged program's file")
endif()

# 6. A command without an output runs as it is.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${MODULE_DIR}/BloomKeepUnchangedLink.cmake" --
        "${CMAKE_COMMAND}" -E true
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(NOT result STREQUAL "0")
    fail("a command without -o did not run (exit ${result})")
endif()

file(REMOVE_RECURSE "${WORK}")
if(failures)
    message(FATAL_ERROR "BloomKeepUnchangedLink.cmake:${failures}")
endif()
message(STATUS "BloomKeepUnchangedLink.cmake: unchanged program kept, changed one replaced, "
    "failure reported, stopped link recovered")

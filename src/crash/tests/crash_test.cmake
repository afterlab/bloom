# A deliberate crash in a child process, then a check of the crash folder it left (issue #736).
#
#   cmake -DCRASH=<command, ;-separated> -DCHECK=<bloom_crash_check> -DMODE=<mode>
#         -DFOLDER=<crash folder> -DPROGRAM=<bloom|bloom-cli|bloom-mcp> -P crash_test.cmake
#
# The folder starts empty. The command runs with BLOOM_TEST_CRASH_DIRECTORY naming it, so a Bloom
# program records its crash there; it must not exit successfully (mode `clean` must). The check
# then collects the folder as the next start does and checks the minidump and the text report,
# and the stacks of the threads the program named in BLOOM_TEST_CRASH_THREADS as alive (#756).
cmake_minimum_required(VERSION 3.25)
foreach(argument IN ITEMS CRASH CHECK MODE FOLDER PROGRAM)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "crash_test.cmake needs ${argument}")
    endif()
endforeach()
file(REMOVE_RECURSE "${FOLDER}")
file(MAKE_DIRECTORY "${FOLDER}")
set(ENV{BLOOM_TEST_CRASH_DIRECTORY} "${FOLDER}")
# #756: the crashing program names the threads it knows are alive at the crash here, beside the
# folder (the folder holds only what a crash leaves).
set(threads "${FOLDER}-threads.txt")
file(REMOVE "${threads}")
set(ENV{BLOOM_TEST_CRASH_THREADS} "${threads}")
execute_process(COMMAND ${CRASH}
    RESULT_VARIABLE crash_result
    OUTPUT_VARIABLE crash_output
    ERROR_VARIABLE crash_output
    TIMEOUT 120)
message(STATUS "crashing program: ${crash_result}\n${crash_output}")
if(MODE STREQUAL "clean")
    if(NOT crash_result EQUAL 0)
        message(FATAL_ERROR "the program did not exit cleanly: ${crash_result}")
    endif()
elseif(crash_result EQUAL 0 OR crash_result MATCHES "timeout")
    message(FATAL_ERROR "the program did not crash: ${crash_result}")
endif()
execute_process(COMMAND "${CHECK}" "${MODE}" "${FOLDER}" "${PROGRAM}" "${threads}"
    RESULT_VARIABLE check_result
    OUTPUT_VARIABLE check_output
    ERROR_VARIABLE check_output
    TIMEOUT 60)
message(STATUS "${check_output}")
if(NOT check_result EQUAL 0)
    message(FATAL_ERROR "the crash folder check failed: ${check_result}")
endif()

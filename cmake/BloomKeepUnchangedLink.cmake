# Link launcher that keeps an unchanged program's file (#727). On macOS a test program pays a
# Gatekeeper scan the first time each new file is started: the system's policy daemon checks it
# online and with XProtect, one file at a time, and the test waits for its turn. The scan is
# remembered per file, not per content, so a byte-identical relink (for example after a library
# archive was rewritten with nothing the program uses changed) is scanned again. This launcher
# links into the usual output path and, when the new file has the same bytes as the one it
# replaces, puts the old file back and marks it as just built, so its scan still holds. A program
# whose bytes changed is replaced as the linker wrote it and is scanned as any new program is.
#
# The linker writes into the output path itself, because the output's name and path go into the
# program (the ad-hoc signature's identifier and the image UUID).
#
# CMake runs it as a <LANG>_LINKER_LAUNCHER:
#   cmake -P BloomKeepUnchangedLink.cmake -- <linker> <arguments...> -o <output> <arguments...>
# A command without `-o <output>` (or a first link) runs unchanged. The exit status is the
# linker's success or failure.
cmake_minimum_required(VERSION 3.25)

set(command "")
set(output "")
set(started FALSE)
set(next_is_output FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(index RANGE 1 ${last})
    set(argument "${CMAKE_ARGV${index}}")
    if(NOT started)
        if(argument STREQUAL "--")
            set(started TRUE)
        endif()
        continue()
    endif()
    list(APPEND command "${argument}")
    if(next_is_output)
        set(output "${argument}")
        set(next_is_output FALSE)
    elseif(argument STREQUAL "-o")
        set(next_is_output TRUE)
    endif()
endforeach()
if(NOT command)
    message(FATAL_ERROR "BloomKeepUnchangedLink.cmake: no link command after `--`")
endif()

set(previous "")
if(output)
    set(previous "${output}.bloom-previous-link")
    # A link stopped between moving the old file aside and finishing leaves it here: it is the
    # newest complete output, so it goes back first.
    if(EXISTS "${previous}" AND NOT EXISTS "${output}")
        file(RENAME "${previous}" "${output}")
    endif()
    if(EXISTS "${output}" AND NOT IS_DIRECTORY "${output}")
        file(RENAME "${output}" "${previous}")
    else()
        set(previous "")
    endif()
endif()

execute_process(COMMAND ${command} RESULT_VARIABLE link_result)

if(previous)
    set(same FALSE)
    if(link_result STREQUAL "0" AND EXISTS "${output}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${output}" "${previous}"
            RESULT_VARIABLE compare_result OUTPUT_QUIET ERROR_QUIET)
        if(compare_result STREQUAL "0")
            set(same TRUE)
        endif()
    endif()
    if(same)
        file(RENAME "${previous}" "${output}")
        file(TOUCH_NOCREATE "${output}")
    elseif(NOT link_result STREQUAL "0" AND NOT EXISTS "${output}")
        # A failed link leaves the old program where it was, as the linker itself does; its time
        # stays older than the inputs, so the next build links again.
        file(RENAME "${previous}" "${output}")
    else()
        file(REMOVE "${previous}")
    endif()
endif()

if(NOT link_result STREQUAL "0")
    message(FATAL_ERROR "link failed (${link_result})")
endif()

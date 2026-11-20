# Debug symbols for release builds (issue #736; docs/development/releasing.md, "Debug symbols").
#
# With BLOOM_DEBUG_SYMBOLS on, Bloom's own code is compiled with debug information, and the
# `bloom-debug-symbols` target (cmake/BloomPackage.cmake) writes each shipped program's symbols
# into BLOOM_DEBUG_SYMBOLS_DIR: a split debug file on Linux (`<file>.debug`, matched by the
# program's GNU build id), a dSYM bundle on macOS (matched by the Mach-O UUID) and the PDB on
# Windows (matched by the CodeView signature). The release workflow keeps that folder as a
# workflow artifact, so a tester's minidump can be symbolized. The symbols never go into a
# package: a Linux package is installed with its programs stripped, a macOS program keeps its
# debug information in object files the dSYM is made from, and a Windows PDB is not installed.
#
# Optimization stays the release build's: only debug information is added.
include_guard(GLOBAL)

option(BLOOM_DEBUG_SYMBOLS
    "Compile with debug information and keep each shipped program's symbols in BLOOM_DEBUG_SYMBOLS_DIR (release builds)"
    OFF)
set(BLOOM_DEBUG_SYMBOLS_DIR "${CMAKE_BINARY_DIR}/symbols" CACHE PATH
    "Where bloom-debug-symbols writes the shipped programs' debug symbols")

if(NOT BLOOM_DEBUG_SYMBOLS)
    return()
endif()

if(MSVC)
    # /Z7 keeps the debug information in the object files (no shared compiler PDB between parallel
    # compilations); the linker writes one PDB per program. /DEBUG would otherwise switch off the
    # release linker's removal and folding of unreferenced code.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-g>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The id Crashpad records for each module and a symbol server matches the debug file by.
        add_link_options(LINKER:--build-id=sha1)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT CMAKE_OBJCOPY)
        message(FATAL_ERROR "BLOOM_DEBUG_SYMBOLS needs objcopy to split the debug information")
    endif()
elseif(APPLE)
    find_program(BLOOM_DSYMUTIL dsymutil REQUIRED)
endif()
message(STATUS "Bloom debug symbols: kept in ${BLOOM_DEBUG_SYMBOLS_DIR}, not in packages")

# The commands that write the symbols of these targets into BLOOM_DEBUG_SYMBOLS_DIR, for a custom
# target. Executables and loadable modules only. The folder is flat, so two targets whose symbol
# files would have the same name stop the configuration instead of one replacing the other.
function(bloom_debug_symbol_commands output)
    set(commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${BLOOM_DEBUG_SYMBOLS_DIR}")
    set(names "")
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            continue()
        endif()
        # The base name of the symbol file: a PDB is named by PDB_NAME (Windows), any other by
        # the program's file name; both fall back to OUTPUT_NAME, then to the target's name.
        set(name "")
        if(WIN32)
            get_target_property(name ${target} PDB_NAME)
        endif()
        if(NOT name)
            get_target_property(name ${target} OUTPUT_NAME)
        endif()
        if(NOT name)
            set(name ${target})
        endif()
        string(TOLOWER "${name}" key)
        if(key IN_LIST names)
            message(FATAL_ERROR "Two shipped programs would write the debug symbols '${name}' "
                "into ${BLOOM_DEBUG_SYMBOLS_DIR}; give ${target} a PDB_NAME or OUTPUT_NAME of its own")
        endif()
        list(APPEND names "${key}")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND commands COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug
                "$<TARGET_FILE:${target}>"
                "${BLOOM_DEBUG_SYMBOLS_DIR}/$<TARGET_FILE_NAME:${target}>.debug")
        elseif(APPLE)
            list(APPEND commands COMMAND "${CMAKE_COMMAND}" -E rm -rf
                "${BLOOM_DEBUG_SYMBOLS_DIR}/$<TARGET_FILE_NAME:${target}>.dSYM"
                COMMAND "${BLOOM_DSYMUTIL}" "$<TARGET_FILE:${target}>"
                -o "${BLOOM_DEBUG_SYMBOLS_DIR}/$<TARGET_FILE_NAME:${target}>.dSYM")
        elseif(WIN32)
            list(APPEND commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_PDB_FILE:${target}>" "${BLOOM_DEBUG_SYMBOLS_DIR}/")
        endif()
    endforeach()
    set(${output} ${commands} PARENT_SCOPE)
endfunction()

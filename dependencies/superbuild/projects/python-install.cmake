# Runs after CPython's own install (python.cmake).
#
#   cmake -DPYTHON_PREFIX=<prefix>/python -DPYTHON_VERSION=<x.y.z> -P python-install.cmake
#
# Gives the interpreter library an @rpath-relative install name, so that what links it can be
# moved with it, and stops the build if a required module was not built or anything the
# interpreter loads would come from outside the operating system.
if(NOT PYTHON_PREFIX OR NOT PYTHON_VERSION)
    message(FATAL_ERROR "python-install.cmake needs PYTHON_PREFIX and PYTHON_VERSION")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" python_minor "${PYTHON_VERSION}")
set(library "${PYTHON_PREFIX}/lib/libpython${python_minor}.dylib")
set(program "${PYTHON_PREFIX}/bin/python${python_minor}")
set(modules "${PYTHON_PREFIX}/lib/python${python_minor}/lib-dynload")

function(bloom_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output
                    ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed: ${output}${error}")
    endif()
    set(bloom_output "${output}" PARENT_SCOPE)
endfunction()

# The library, and the record of it in the program that links it.
bloom_run(otool -D "${library}")
# otool prints the file's path, then the install name on the next line.
string(REGEX MATCH "\n([^\n]+)" old_name "${bloom_output}")
set(old_name "${CMAKE_MATCH_1}")
string(STRIP "${old_name}" old_name)
if(old_name STREQUAL "")
    message(FATAL_ERROR "${library} has no install name")
endif()
set(new_name "@rpath/libpython${python_minor}.dylib")
if(NOT old_name STREQUAL new_name)
    bloom_run(install_name_tool -id "${new_name}" "${library}")
    bloom_run(install_name_tool -change "${old_name}" "${new_name}" "${program}")
    bloom_run(codesign --force --sign - "${library}")
    bloom_run(codesign --force --sign - "${program}")
endif()

# Every module Bloom's scripting parity with the Linux system interpreter rests on.
bloom_run("${program}" -I -c
    "import ssl, lzma, sqlite3, ctypes, hashlib, decimal, zlib, bz2, pyexpat, compression.zstd; \
hashlib.sha256(); hashlib.sha3_256(); hashlib.blake2b(); print(ssl.OPENSSL_VERSION)")
message(STATUS "Bloom Python ${PYTHON_VERSION}: ${bloom_output}")

# Nothing loaded from outside the operating system or the interpreter itself.
file(GLOB loaded "${modules}/*.so")
list(APPEND loaded "${library}" "${program}")
foreach(item IN LISTS loaded)
    bloom_run(otool -L "${item}")
    string(REPLACE "\n" ";" lines "${bloom_output}")
    list(REMOVE_AT lines 0)
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()
        string(REGEX REPLACE " \\(.*$" "" name "${line}")
        if(NOT name MATCHES "^(/usr/lib/|/System/Library/|@rpath/libpython)")
            message(FATAL_ERROR "${item} loads ${name}, which is neither part of the operating "
                                "system nor the interpreter library")
        endif()
    endforeach()
endforeach()

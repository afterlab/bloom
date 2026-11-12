# Runs after CPython's own install (python.cmake).
#
#   cmake -DPYTHON_PREFIX=<prefix>/python -DPYTHON_VERSION=<x.y.z> -P python-install.cmake
#
# Makes the interpreter library something a package can carry and stops the build if a required
# module was not built, a module that no platform carries was built, or anything the interpreter
# loads would come from outside the operating system.
#
#   macOS  The library gets an @rpath-relative install name, so that what links it can be moved
#          with it. Allowed: /usr/lib, /System/Library and the interpreter library.
#   Linux  The program's search path must be $ORIGIN/../lib. Allowed: the C library's own
#          libraries and the interpreter library.
#   Windows  Allowed: the libraries the interpreter directory itself carries (the interpreter
#          library, OpenSSL's two, libffi's) and the libraries of Windows' own system directory.
#          The Visual C++ runtime is not part of Windows: it is accepted only when the
#          interpreter directory carries it.
if(NOT PYTHON_PREFIX OR NOT PYTHON_VERSION)
    message(FATAL_ERROR "python-install.cmake needs PYTHON_PREFIX and PYTHON_VERSION")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" python_minor "${PYTHON_VERSION}")
if(CMAKE_HOST_WIN32)
    set(program "${PYTHON_PREFIX}/python.exe")
    set(library_directory "${PYTHON_PREFIX}/Lib")
    set(modules "${PYTHON_PREFIX}/DLLs")
    set(module_suffix pyd)
else()
    set(program "${PYTHON_PREFIX}/bin/python${python_minor}")
    set(library_directory "${PYTHON_PREFIX}/lib/python${python_minor}")
    set(modules "${library_directory}/lib-dynload")
    set(module_suffix so)
endif()

function(bloom_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output
                    ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed: ${output}${error}")
    endif()
    set(bloom_output "${output}" PARENT_SCOPE)
endfunction()

if(CMAKE_HOST_WIN32)
    string(REPLACE "." "" python_digits "${python_minor}")
    set(library "${PYTHON_PREFIX}/python${python_digits}.dll")
    if(NOT EXISTS "${library}" OR NOT EXISTS "${program}")
        message(FATAL_ERROR "${library} or ${program} was not installed")
    endif()
    find_program(BLOOM_DUMPBIN dumpbin REQUIRED)
elseif(CMAKE_HOST_APPLE)
    set(library "${PYTHON_PREFIX}/lib/libpython${python_minor}.dylib")
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
else()
    set(library "${PYTHON_PREFIX}/lib/libpython${python_minor}.so.1.0")
    if(NOT EXISTS "${library}")
        message(FATAL_ERROR "${library} was not installed")
    endif()
    bloom_run(readelf -d "${program}")
    if(NOT bloom_output MATCHES "\\((RUNPATH|RPATH)\\)[^\n]*\\[\\$ORIGIN/\\.\\./lib\\]")
        message(FATAL_ERROR "${program} does not search $ORIGIN/../lib for the interpreter "
                            "library:\n${bloom_output}")
    endif()
endif()

# Parts of the standard library that cannot work in the carried interpreter are removed, so a
# script finds the same modules in the prefix, in every package and on every platform: the
# packages that need Tk or curses, which are not built, and pip's installer (the carried
# interpreter installs nothing). The programs that start them go too.
file(REMOVE_RECURSE
    "${library_directory}/tkinter" "${library_directory}/idlelib"
    "${library_directory}/turtledemo" "${library_directory}/curses"
    "${library_directory}/ensurepip")
file(REMOVE "${library_directory}/turtle.py")
# (Windows has no such programs: CPython's layout tool leaves them out.)
file(GLOB unused_programs "${PYTHON_PREFIX}/bin/idle3*")
if(unused_programs)
    file(REMOVE ${unused_programs})
endif()

# Every module the carried interpreter must have, on every platform (the full set is compared by
# the bloom.scripting.python-module-set test).
# (Newlines, not semicolons, separate the statements: a semicolon would split the argument.)
bloom_run("${program}" -I -c
    "import ssl, lzma, sqlite3, ctypes, hashlib, decimal, zlib, bz2, pyexpat, compression.zstd
hashlib.sha256()
hashlib.sha3_256()
hashlib.blake2b()
print(ssl.OPENSSL_VERSION, '| SQLite', sqlite3.sqlite_version, '| zlib', zlib.ZLIB_RUNTIME_VERSION)")
string(STRIP "${bloom_output}" bloom_output)
if(bloom_output STREQUAL "")
    message(FATAL_ERROR "The module check of ${program} printed nothing")
endif()
message(STATUS "Bloom Python ${PYTHON_VERSION}: ${bloom_output}")

# The modules no platform carries (python.cmake).
foreach(absent IN ITEMS _tkinter _gdbm _dbm _curses _curses_panel readline)
    file(GLOB built "${modules}/${absent}.*")
    if(built)
        message(FATAL_ERROR "${absent} was built (${built}); no platform carries it")
    endif()
endforeach()

# Nothing loaded from outside the operating system or the interpreter itself.
file(GLOB loaded "${modules}/*.${module_suffix}")
list(APPEND loaded "${library}" "${program}")
if(CMAKE_HOST_WIN32)
    # The libraries the modules load are themselves checked.
    file(GLOB carried "${modules}/*.dll" "${PYTHON_PREFIX}/*.dll")
    list(APPEND loaded ${carried})
    list(REMOVE_DUPLICATES loaded)
    file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32" system_directory)
endif()
foreach(item IN LISTS loaded)
    if(CMAKE_HOST_WIN32)
        bloom_run("${BLOOM_DUMPBIN}" /nologo /dependents "${item}")
        string(REGEX MATCHALL "\n +[^ \r\n]+\\.[Dd][Ll][Ll]" needed "${bloom_output}")
        foreach(entry IN LISTS needed)
            string(STRIP "${entry}" name)
            string(TOLOWER "${name}" lower)
            if(EXISTS "${PYTHON_PREFIX}/${name}" OR EXISTS "${modules}/${name}")
                continue()
            endif()
            # An API set is a name of Windows itself, not a file.
            if(lower MATCHES "^(api-ms-win-|ext-ms-)")
                continue()
            endif()
            if(lower MATCHES "^(vcruntime|msvcp|concrt|vcomp)" OR
               NOT EXISTS "${system_directory}/${name}")
                message(FATAL_ERROR "${item} loads ${name}, which is neither carried by the "
                                    "interpreter directory nor part of Windows")
            endif()
        endforeach()
    elseif(CMAKE_HOST_APPLE)
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
                message(FATAL_ERROR "${item} loads ${name}, which is neither part of the "
                                    "operating system nor the interpreter library")
            endif()
        endforeach()
    else()
        bloom_run(readelf -d "${item}")
        string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]\n]+\\]" needed "${bloom_output}")
        foreach(entry IN LISTS needed)
            string(REGEX REPLACE "^.*\\[([^]]+)\\]$" "\\1" name "${entry}")
            if(NOT name MATCHES
               "^(libc\\.so\\.6|libm\\.so\\.6|libdl\\.so\\.2|libpthread\\.so\\.0|librt\\.so\\.1|libutil\\.so\\.1|ld-linux-x86-64\\.so\\.2|libpython${python_minor}\\.so\\.1\\.0)$")
                message(FATAL_ERROR "${item} loads ${name}, which is neither part of the C "
                                    "library nor the interpreter library")
            endif()
        endforeach()
    endif()
endforeach()

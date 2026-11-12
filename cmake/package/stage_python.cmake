# Puts the locked CPython into a Bloom package, so scripting starts on a computer without Python.
#
#   cmake -DROOT=<package root> -DPYTHON_HOME=<dependency prefix>/python
#         -DBLOOM_PYTHON=<build tree>/python
#         -DPYTHON_DIR=<interpreter home, relative to the root>
#         -DBLOOM_PYTHON_DIR=<Bloom's Python package, relative to the root>
#         -DLIBRARY_DIR=<the interpreter library, relative to the root>
#         -P stage_python.cmake
#
# The three relative directories are cmake/BloomPackageLayout.cmake's; they are the only thing
# that differs between the operating systems. What is staged:
#
#   <LIBRARY_DIR>          the interpreter library the programs link (libpython3.X.so.1.0,
#                          libpython3.X.dylib; python3XX.dll)
#   <PYTHON_DIR>           the standard library, its extension modules and the bundled packages,
#                          in the shape CPython expects of its home: lib/python3.X, and on Windows
#                          Lib and DLLs (the modules with the libraries they load)
#   <BLOOM_PYTHON_DIR>     Bloom's own Python package
#
# src/scripting/python/runtime.cpp gives the interpreter that home. Nothing is rewritten: every
# file is the dependency prefix's, byte for byte. The license texts are staged by
# stage_records.cmake with every other component's.
#
# Left out: compiled caches, the files for building extensions against this interpreter, the
# interpreter programs themselves (Bloom's programs are the interpreter) and, on Windows, the
# Visual C++ runtime the prefix keeps beside python.exe (the package carries one copy for all its
# programs). The parts of the standard library that cannot work in the carried interpreter are
# already removed from the prefix (dependencies/superbuild/projects/python-install.cmake).
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS ROOT PYTHON_HOME BLOOM_PYTHON PYTHON_DIR BLOOM_PYTHON_DIR LIBRARY_DIR)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "stage_python.cmake needs ${argument}")
    endif()
endforeach()
foreach(argument IN ITEMS PYTHON_HOME BLOOM_PYTHON)
    if(NOT IS_DIRECTORY "${${argument}}")
        message(FATAL_ERROR "stage_python.cmake: ${argument} is not a directory: "
                            "'${${argument}}'")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${BLOOM_PYTHON}/bloom")
    message(FATAL_ERROR "stage_python.cmake: no Bloom package in ${BLOOM_PYTHON}")
endif()

set(home "${ROOT}/${PYTHON_DIR}")
set(libraries "${ROOT}/${LIBRARY_DIR}")
set(package "${ROOT}/${BLOOM_PYTHON_DIR}")
file(REMOVE_RECURSE "${home}" "${package}")

if(CMAKE_HOST_WIN32)
    file(GLOB interpreter "${PYTHON_HOME}/python3??.dll")
    list(LENGTH interpreter count)
    if(NOT count EQUAL 1 OR NOT EXISTS "${PYTHON_HOME}/Lib/os.py"
       OR NOT IS_DIRECTORY "${PYTHON_HOME}/DLLs")
        message(FATAL_ERROR "stage_python.cmake: ${PYTHON_HOME} is not a complete CPython "
                            "(python3XX.dll, Lib/os.py, DLLs)")
    endif()
    get_filename_component(name "${interpreter}" NAME)
    file(COPY "${PYTHON_HOME}/Lib" DESTINATION "${home}"
         PATTERN "__pycache__" EXCLUDE)
    file(COPY "${PYTHON_HOME}/DLLs" DESTINATION "${home}"
         PATTERN "*.pdb" EXCLUDE
         PATTERN "*.lib" EXCLUDE
         PATTERN "*.exp" EXCLUDE)
    set(numpy "${home}/Lib/site-packages/numpy/__init__.py")
else()
    file(GLOB standard "${PYTHON_HOME}/lib/python3.*")
    list(FILTER standard EXCLUDE REGEX "\\.(dylib|so)[^/]*$")
    list(LENGTH standard count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "stage_python.cmake: expected one standard library under "
                            "${PYTHON_HOME}/lib, found: ${standard}")
    endif()
    get_filename_component(version "${standard}" NAME)
    if(CMAKE_HOST_APPLE)
        set(name "lib${version}.dylib")
    else()
        set(name "lib${version}.so.1.0")
    endif()
    set(interpreter "${PYTHON_HOME}/lib/${name}")
    if(NOT EXISTS "${interpreter}" OR NOT EXISTS "${standard}/os.py")
        message(FATAL_ERROR "stage_python.cmake: ${PYTHON_HOME} is not a complete CPython "
                            "(${interpreter}, ${standard}/os.py)")
    endif()
    file(COPY "${standard}" DESTINATION "${home}/lib"
         PATTERN "__pycache__" EXCLUDE
         PATTERN "config-3.*" EXCLUDE)
    set(numpy "${home}/lib/${version}/site-packages/numpy/__init__.py")
endif()
if(NOT EXISTS "${numpy}")
    message(FATAL_ERROR "stage_python.cmake: ${PYTHON_HOME} has no numpy, the bundled package "
                        "set's one member")
endif()

file(COPY "${interpreter}" DESTINATION "${libraries}")
file(COPY "${BLOOM_PYTHON}/bloom" DESTINATION "${package}"
     PATTERN "__pycache__" EXCLUDE)
message(STATUS "Bloom package: carries ${name}, its standard library and numpy")

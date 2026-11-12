# Puts the Visual C++ runtime into a Windows Bloom package, beside every program
# (docs/architecture/platform-support.md, "Packages").
#
#   cmake -DROOT=<package root> "-DRUNTIME=<runtime library>|<runtime library>|..."
#         -DVERSION=<toolset the libraries are from> -DPYTHON_DIR=<interpreter home, relative>
#         -DRECORD_DIR=<record directory, relative to the root>
#         -P stage_msvc_runtime.cmake
#
# The Visual C++ runtime (vcruntime140.dll, msvcp140.dll and the libraries beside them) is not
# part of Windows: a computer without the build tools or the redistributable has none, and a
# program that needs it does not start. The libraries are the redistributable ones of the
# toolset that built Bloom (CMake's InstallRequiredSystemLibraries names them), copied
# unchanged. The universal C runtime is part of Windows 10 and later and is not copied.
#
# A Windows program looks for a library beside itself first, so every directory of the package
# that holds a program gets the libraries: the package folder, the shader tools and each media
# worker. (The carried interpreter's extension modules are loaded into those programs and use
# the libraries already loaded. The launchers inside the standard library's `venv` module are
# templates, not programs of the package.)
#
#   <directory of each program>/<runtime library>
#   <RECORD_DIR>/licenses/msvc-runtime/NOTICE.txt   what was copied, from which toolset, and
#                                                   the terms it is redistributed under
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS ROOT RUNTIME VERSION PYTHON_DIR RECORD_DIR)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "stage_msvc_runtime.cmake needs ${argument}")
    endif()
endforeach()
string(REPLACE "|" ";" libraries "${RUNTIME}")
set(names "")
foreach(library IN LISTS libraries)
    if(NOT EXISTS "${library}")
        message(FATAL_ERROR "stage_msvc_runtime.cmake: ${library} does not exist")
    endif()
    get_filename_component(name "${library}" NAME)
    list(APPEND names "${name}")
endforeach()
if(NOT "vcruntime140.dll" IN_LIST names OR NOT "msvcp140.dll" IN_LIST names)
    message(FATAL_ERROR "stage_msvc_runtime.cmake: the runtime libraries do not include "
                        "vcruntime140.dll and msvcp140.dll: ${names}")
endif()

file(GLOB_RECURSE programs LIST_DIRECTORIES false RELATIVE "${ROOT}" "${ROOT}/*.exe")
set(directories "")
foreach(program IN LISTS programs)
    string(FIND "${program}" "${PYTHON_DIR}/" position)
    if(position EQUAL 0)
        continue()
    endif()
    get_filename_component(directory "${program}" DIRECTORY)
    if(directory STREQUAL "")
        set(directory ".")
    endif()
    list(APPEND directories "${directory}")
endforeach()
list(REMOVE_DUPLICATES directories)
list(SORT directories)
if(NOT directories)
    message(FATAL_ERROR "stage_msvc_runtime.cmake: ${ROOT} holds no program")
endif()
foreach(directory IN LISTS directories)
    file(COPY ${libraries} DESTINATION "${ROOT}/${directory}")
endforeach()

set(notice
    "Microsoft Visual C++ runtime\n"
    "\n"
    "This package carries the redistributable Visual C++ runtime libraries of the toolset\n"
    "that built it (${VERSION}), unchanged, beside each of its programs. They are\n"
    "Microsoft's, and are redistributed under the Microsoft Software License Terms for\n"
    "Visual Studio, which list them as distributable code (the Visual C++ Redistributable\n"
    "files). They are not covered by Bloom's own license.\n"
    "\n"
    "Libraries (SHA-256):\n")
list(SORT libraries)
foreach(library IN LISTS libraries)
    get_filename_component(name "${library}" NAME)
    file(SHA256 "${library}" digest)
    string(APPEND notice "  ${digest}  ${name}\n")
endforeach()
string(APPEND notice "\nBeside the programs in:\n")
foreach(directory IN LISTS directories)
    string(APPEND notice "  ${directory}\n")
endforeach()
file(WRITE "${ROOT}/${RECORD_DIR}/licenses/msvc-runtime/NOTICE.txt" ${notice})
list(LENGTH libraries library_count)
list(LENGTH directories directory_count)
message(STATUS "Bloom package: carries ${library_count} Visual C++ runtime libraries beside "
               "the programs of ${directory_count} directories")

# Writes a release's install script for Linux from tools/install/install.sh.in
# (docs/architecture/platform-support.md, "Installers").
#
#   cmake -DVERSION=<the release's label, for example 0.1.0-alpha.1>
#         -DBASE_URL=<the address the release's files are under>
#         -DPUBLIC_KEYS=<the release keys, as in the build setting BLOOM_UPDATE_PUBLIC_KEYS>
#         -DOUTPUT=<install.sh>
#         -P make_install_script.cmake
#
# The script carries the list of system libraries a package expects
# (cmake/package/linux-base-libraries.txt) with the package that has each
# (tools/install/linux-library-packages.txt). A library that is in one file and not in the other
# stops this script, so the two cannot drift apart.
cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS VERSION BASE_URL PUBLIC_KEYS OUTPUT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "make_install_script.cmake: ${required} is not set")
    endif()
endforeach()
if(NOT VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.]+)?$")
    message(FATAL_ERROR "make_install_script.cmake: '${VERSION}' is not a version")
endif()
if(NOT BASE_URL MATCHES "^(https|file)://[-A-Za-z0-9_.~/:+]+$")
    message(FATAL_ERROR "make_install_script.cmake: '${BASE_URL}' is not an address the script "
                        "can carry")
endif()
if(NOT PUBLIC_KEYS MATCHES "^[A-Za-z0-9+/]+=(,[A-Za-z0-9+/]+=)*$")
    message(FATAL_ERROR "make_install_script.cmake: PUBLIC_KEYS must be Base64 keys of 32 bytes, "
                        "separated by commas")
endif()
string(REGEX REPLACE "/+$" "" BASE_URL "${BASE_URL}")

set(source "${CMAKE_CURRENT_LIST_DIR}/../..")

# The declared system libraries, without the patterns (a name with `*` stands for modules the C
# library loads by itself).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/linux-base-libraries.txt" lines)
set(declared "")
foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#" OR line MATCHES "\\*")
        continue()
    endif()
    list(APPEND declared "${line}")
endforeach()

file(STRINGS "${source}/tools/install/linux-library-packages.txt" lines)
set(mapped "")
set(table "")
foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    if(NOT line MATCHES
       "^([^ ]+) (required|optional) ([-A-Za-z0-9.+]+) ([-A-Za-z0-9.+]+) ([-A-Za-z0-9.+]+)$")
        message(FATAL_ERROR "tools/install/linux-library-packages.txt: this line is not "
                            "'<library> <required|optional> <package> <package> <package>': "
                            "${line}")
    endif()
    set(library "${CMAKE_MATCH_1}")
    if(library IN_LIST mapped)
        message(FATAL_ERROR "tools/install/linux-library-packages.txt names ${library} twice")
    endif()
    if(NOT library IN_LIST declared)
        message(FATAL_ERROR "tools/install/linux-library-packages.txt names ${library}, which "
                            "cmake/package/linux-base-libraries.txt does not")
    endif()
    list(APPEND mapped "${library}")
    string(APPEND table "${line}\n")
endforeach()
foreach(library IN LISTS declared)
    if(NOT library IN_LIST mapped)
        message(FATAL_ERROR "cmake/package/linux-base-libraries.txt names ${library}, which has "
                            "no line in tools/install/linux-library-packages.txt")
    endif()
endforeach()
string(REGEX REPLACE "\n$" "" table "${table}")

file(READ "${source}/tools/install/install.sh.in" script)
string(REPLACE "@BLOOM_INSTALL_VERSION@" "${VERSION}" script "${script}")
string(REPLACE "@BLOOM_INSTALL_BASE_URL@" "${BASE_URL}" script "${script}")
string(REPLACE "@BLOOM_INSTALL_PUBLIC_KEYS@" "${PUBLIC_KEYS}" script "${script}")
string(REPLACE "@BLOOM_INSTALL_LIBRARIES@" "${table}" script "${script}")
if(script MATCHES "@BLOOM_INSTALL_[A-Z_]+@")
    message(FATAL_ERROR "make_install_script.cmake: ${CMAKE_MATCH_0} was not filled in")
endif()
file(WRITE "${OUTPUT}" "${script}")
file(CHMOD "${OUTPUT}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                                   WORLD_READ WORLD_EXECUTE)

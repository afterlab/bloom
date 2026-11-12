# Checks that Bloom uses only the listed parts of Qt and that each is offered under the LGPL.
#
#   cmake -DBLOOM_SOURCE_ROOT=<repository> [-DBLOOM_QT_ROOT=<prefix>/qt]
#         [-DBLOOM_QT_MODULE_LIST=<file>] -P BloomQtModuleCheck.cmake
#
# Bloom links Qt dynamically under LGPL-3.0-only. Some Qt modules are published under the GPL or
# a commercial license only; linking one would put the whole application under that license.
# Two things are checked:
#
#   1. Every Qt part the build names (`Qt6::<name>`, and the components of `find_package(Qt6`
#      and `bloom_find_locked_qt(`) in the repository's CMake files is in the module list
#      (dependencies/qt-modules.txt).
#   2. With BLOOM_QT_ROOT, the locked Qt's own software bill of materials (`sbom/*.spdx`, which
#      the official binaries carry) offers every listed part of this operating system under
#      LGPL-3.0-only. A part the bill of materials does not have, or has under other licenses
#      only, is refused with its license expression.
#
# Without BLOOM_QT_ROOT only the first is checked (a build against the host's Qt has no bill of
# materials to read).
cmake_minimum_required(VERSION 3.25)
if(NOT BLOOM_SOURCE_ROOT)
    message(FATAL_ERROR "BloomQtModuleCheck.cmake needs BLOOM_SOURCE_ROOT")
endif()
if(NOT BLOOM_QT_MODULE_LIST)
    set(BLOOM_QT_MODULE_LIST "${BLOOM_SOURCE_ROOT}/dependencies/qt-modules.txt")
endif()
if(NOT BLOOM_QT_OPERATING_SYSTEM)
    if(CMAKE_HOST_WIN32)
        set(BLOOM_QT_OPERATING_SYSTEM windows)
    elseif(CMAKE_HOST_APPLE)
        set(BLOOM_QT_OPERATING_SYSTEM macos)
    else()
        set(BLOOM_QT_OPERATING_SYSTEM linux)
    endif()
endif()

# The list.
set(listed)
set(listed_here)
file(STRINGS "${BLOOM_QT_MODULE_LIST}" lines)
foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*(#|$)")
        continue()
    endif()
    if(NOT line MATCHES "^(module|plugin) ([A-Za-z0-9_]+) ([a-z ]+) (package|build)$")
        message(FATAL_ERROR "${BLOOM_QT_MODULE_LIST}: unreadable line '${line}'")
    endif()
    set(name "${CMAKE_MATCH_2}")
    string(REPLACE " " ";" systems "${CMAKE_MATCH_3}")
    foreach(system IN LISTS systems)
        if(NOT system MATCHES "^(all|linux|macos|windows)$")
            message(FATAL_ERROR "${BLOOM_QT_MODULE_LIST}: unknown operating system '${system}' "
                                "in '${line}'")
        endif()
    endforeach()
    if(name IN_LIST listed)
        message(FATAL_ERROR "${BLOOM_QT_MODULE_LIST}: ${name} is listed twice")
    endif()
    list(APPEND listed "${name}")
    if("all" IN_LIST systems OR BLOOM_QT_OPERATING_SYSTEM IN_LIST systems)
        list(APPEND listed_here "${name}")
    endif()
endforeach()
if(NOT listed)
    message(FATAL_ERROR "${BLOOM_QT_MODULE_LIST} lists nothing")
endif()

set(problems)

# 1. What the build names.
file(GLOB_RECURSE files
     "${BLOOM_SOURCE_ROOT}/CMakeLists.txt"
     "${BLOOM_SOURCE_ROOT}/cmake/*.cmake"
     "${BLOOM_SOURCE_ROOT}/src/*CMakeLists.txt" "${BLOOM_SOURCE_ROOT}/src/*.cmake"
     "${BLOOM_SOURCE_ROOT}/apps/*CMakeLists.txt" "${BLOOM_SOURCE_ROOT}/apps/*.cmake"
     "${BLOOM_SOURCE_ROOT}/tools/*CMakeLists.txt" "${BLOOM_SOURCE_ROOT}/tools/*.cmake"
     "${BLOOM_SOURCE_ROOT}/tests/*CMakeLists.txt" "${BLOOM_SOURCE_ROOT}/tests/*.cmake")
# This check and its test name Qt parts as examples.
list(FILTER files EXCLUDE REGEX "/BloomQtModuleCheck(Test)?\\.cmake$")
# Qt's own tools and the umbrella target are not modules.
set(tools qmake moc rcc uic qtpaths windeployqt macdeployqt lrelease lupdate Platform)
set(named 0)
foreach(file IN LISTS files)
    file(READ "${file}" text)
    # Comments name Qt parts without using them.
    string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" text "${text}")
    string(REGEX MATCHALL "Qt6?::[A-Za-z0-9_]+" targets "${text}")
    set(names)
    foreach(target IN LISTS targets)
        string(REGEX REPLACE "^Qt6?::" "" name "${target}")
        list(APPEND names "${name}")
    endforeach()
    string(REGEX MATCHALL
           "(find_package\\([ \t\r\n]*Qt6[^)]*COMPONENTS|bloom_find_locked_qt\\()[^)]*\\)" calls
           "${text}")
    foreach(call IN LISTS calls)
        string(REGEX REPLACE "^.*(COMPONENTS|bloom_find_locked_qt\\()" "" components "${call}")
        # Variable references and quoted arguments are not component names.
        string(REGEX REPLACE "\\$\\{[^}]*\\}|\"[^\"]*\"" " " components "${components}")
        string(REGEX MATCHALL "[A-Za-z][A-Za-z0-9_]*" components "${components}")
        list(APPEND names ${components})
    endforeach()
    if(names)
        list(REMOVE_DUPLICATES names)
    endif()
    foreach(name IN LISTS names)
        # An argument keyword (REQUIRED, PATHS, NO_DEFAULT_PATH) is all capitals; no Qt part is.
        if(name IN_LIST tools OR name MATCHES "^[A-Z][A-Z_]*$")
            continue()
        endif()
        math(EXPR named "${named} + 1")
        if(NOT name IN_LIST listed)
            file(RELATIVE_PATH relative "${BLOOM_SOURCE_ROOT}" "${file}")
            list(APPEND problems
                 "${relative} names the Qt part ${name}, which the Qt module list does not have")
        endif()
    endforeach()
endforeach()
if(named EQUAL 0)
    list(APPEND problems "no CMake file under ${BLOOM_SOURCE_ROOT} names a Qt part")
endif()

# 2. The license each listed part is offered under.
set(licensed 0)
if(BLOOM_QT_ROOT)
    file(GLOB documents "${BLOOM_QT_ROOT}/sbom/*.spdx")
    list(FILTER documents EXCLUDE REGEX "\\.source\\.spdx$")
    if(NOT documents)
        message(FATAL_ERROR "${BLOOM_QT_ROOT}/sbom has no software bill of materials (*.spdx)")
    endif()
    set(bill "")
    foreach(document IN LISTS documents)
        file(READ "${document}" text)
        # The Windows binaries' documents end their lines with a carriage return too.
        string(REPLACE "\r" "" text "${text}")
        string(APPEND bill "\n${text}")
    endforeach()
    foreach(name IN LISTS listed_here)
        string(FIND "${bill}" "\nPackageName: ${name}\n" position)
        if(position EQUAL -1)
            list(APPEND problems "the locked Qt's bill of materials has no part ${name}")
            continue()
        endif()
        string(SUBSTRING "${bill}" ${position} 4096 package)
        # Up to the next package.
        string(REGEX REPLACE "^(\nPackageName: [^\n]*\n)" "" package "${package}")
        string(REGEX REPLACE "\nPackageName: .*$" "" package "${package}")
        if(NOT package MATCHES "\nPackageLicenseConcluded: ([^\n]*)")
            list(APPEND problems "the locked Qt's bill of materials states no license of ${name}")
            continue()
        endif()
        set(expression "${CMAKE_MATCH_1}")
        # LGPL-3.0-only must be a whole alternative of the expression: "A OR LGPL-3.0-only OR B".
        string(REPLACE " OR " ";" alternatives "${expression}")
        if(NOT "LGPL-3.0-only" IN_LIST alternatives)
            list(APPEND problems
                 "Qt ${name} is offered under '${expression}' only, not under LGPL-3.0-only: "
                 "Bloom cannot link or carry a GPL-only or commercial Qt part")
            continue()
        endif()
        math(EXPR licensed "${licensed} + 1")
    endforeach()
endif()

if(problems)
    list(JOIN problems "\n  " report)
    message(FATAL_ERROR "Qt module check failed:\n  ${report}")
endif()
if(BLOOM_QT_ROOT)
    message(STATUS "Qt module check: ${named} uses of listed Qt parts; ${licensed} parts of "
                   "${BLOOM_QT_OPERATING_SYSTEM} offered under LGPL-3.0-only by the locked Qt's "
                   "bill of materials")
else()
    message(STATUS "Qt module check: ${named} uses of listed Qt parts (no locked Qt to read the "
                   "licenses from)")
endif()

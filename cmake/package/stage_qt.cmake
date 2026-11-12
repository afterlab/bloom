# Puts the locked Qt into a Bloom package: the libraries and plugins dependencies/qt-modules.txt
# lists for this operating system with `package`, and nothing else of Qt.
#
#   cmake -DROOT=<package root> -DQT=<dependency prefix>/qt
#         -DMODULES=<source tree>/dependencies/qt-modules.txt
#         -DOPERATING_SYSTEM=<linux|macos|windows>
#         -DQT_DIR=<Qt directory, relative to the root>
#         -DLIBRARY_DIR=<library directory, relative to the root>
#         -DPROGRAM_DIR=<program directory, relative to the root>
#         -P stage_qt.cmake
#
# Where each listed part's file is comes from the Qt binaries' own CMake records
# (lib/cmake/Qt6<Name>/Qt6<Name>Targets-<configuration>.cmake), so no file name is written here.
# Every file is copied as the Qt Project published it: nothing is patched, stripped or relinked,
# which is what the notice in the package says of them.
#
#   Linux    <QT_DIR>/lib       the libraries, each under its real name with the link the
#                               programs ask for (libQt6Core.so.6), and the other libraries in
#                               Qt's lib directory that they load (the Unicode libraries)
#            <QT_DIR>/plugins   the plugins, in Qt's own directories. They look for the
#                               libraries in ../../lib, and Qt looks for them beside its lib
#                               directory, so this shape needs no configuration file.
#   Windows  <LIBRARY_DIR>      the libraries, beside the programs
#            <QT_DIR>/plugins   the plugins
#            <PROGRAM_DIR>/qt.conf   names that plugin directory
#   macOS    <LIBRARY_DIR>      the frameworks, whole, as the links and directories they are
#            <QT_DIR>/PlugIns   the plugins
#            <QT_DIR>/Resources/qt.conf   names that plugin directory
#            The frameworks and plugins keep the Qt Project's own signatures. The application
#            finds the frameworks through its @executable_path/../Frameworks search path, which
#            the plugins loaded into it use as well.
#
# A listed part the Qt binaries do not have stops the staging. So does, on Linux, a staged file
# that loads a Qt library the list does not name: the list is what a package carries, and it
# has to be complete. (On Windows and for everything that is not Qt, cmake/package/
# check_package.cmake finds a library that is missing.)
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS ROOT QT MODULES OPERATING_SYSTEM QT_DIR LIBRARY_DIR PROGRAM_DIR)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "stage_qt.cmake needs ${argument}")
    endif()
endforeach()
if(NOT OPERATING_SYSTEM MATCHES "^(linux|macos|windows)$")
    message(FATAL_ERROR "stage_qt.cmake: unknown operating system '${OPERATING_SYSTEM}'")
endif()
if(NOT EXISTS "${QT}/bloom-prebuilt.txt")
    message(FATAL_ERROR "stage_qt.cmake: ${QT} is not the locked Qt of a dependency prefix "
                        "(it has no bloom-prebuilt.txt)")
endif()

# The listed parts of this operating system.
file(STRINGS "${MODULES}" lines)
set(parts "")
foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*(#|$)")
        continue()
    endif()
    string(REGEX REPLACE "[ \t]+" ";" words "${line}")
    list(GET words 0 kind)
    list(GET words 1 name)
    list(GET words -1 use)
    list(SUBLIST words 2 -1 where)
    list(REMOVE_AT where -1)
    if(use STREQUAL "package" AND ("all" IN_LIST where OR OPERATING_SYSTEM IN_LIST where))
        list(APPEND parts "${kind}:${name}")
    endif()
endforeach()
if(NOT parts)
    message(FATAL_ERROR "stage_qt.cmake: ${MODULES} lists nothing for ${OPERATING_SYSTEM}")
endif()

# The file of one part, relative to the Qt directory, from Qt's CMake record of it.
function(bloom_qt_file kind name variable)
    if(kind STREQUAL "module")
        file(GLOB records "${QT}/lib/cmake/Qt6${name}/Qt6${name}Targets-*.cmake")
    else()
        file(GLOB records "${QT}/lib/cmake/*/Qt6${name}Targets-*.cmake")
    endif()
    list(FILTER records EXCLUDE REGEX "Targets-debug\\.cmake$")
    list(LENGTH records count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "stage_qt.cmake: the locked Qt has no ${kind} ${name} "
                            "(${MODULES}); found: ${records}")
    endif()
    file(READ "${records}" record)
    if(NOT record MATCHES "IMPORTED_LOCATION_[A-Z]+ \"\\$\\{_IMPORT_PREFIX\\}/([^\"]+)\"")
        message(FATAL_ERROR "stage_qt.cmake: ${records} names no file")
    endif()
    set(file "${CMAKE_MATCH_1}")
    set(link "")
    if(record MATCHES "IMPORTED_SONAME_[A-Z]+ \"([^\"]+)\"")
        set(link "${CMAKE_MATCH_1}")
    endif()
    if(NOT EXISTS "${QT}/${file}")
        message(FATAL_ERROR "stage_qt.cmake: ${QT}/${file} (${kind} ${name}) does not exist")
    endif()
    set(${variable} "${file}" PARENT_SCOPE)
    set(${variable}_link "${link}" PARENT_SCOPE)
endfunction()

set(qt "${ROOT}/${QT_DIR}")
if(OPERATING_SYSTEM STREQUAL "linux")
    file(REMOVE_RECURSE "${qt}")
    set(libraries "${qt}/lib")
    set(plugins "${qt}/plugins")
    set(record "${qt}/bloom-prebuilt.txt")
elseif(OPERATING_SYSTEM STREQUAL "macos")
    # The Qt directory is the bundle's Contents, which holds everything else too.
    set(libraries "${ROOT}/${LIBRARY_DIR}")
    set(plugins "${qt}/PlugIns")
    set(record "${qt}/Resources/qt-prebuilt.txt")
    file(REMOVE_RECURSE "${plugins}")
else()
    file(REMOVE_RECURSE "${qt}")
    set(libraries "${ROOT}/${LIBRARY_DIR}")
    set(plugins "${qt}/plugins")
    set(record "${qt}/bloom-prebuilt.txt")
endif()
file(MAKE_DIRECTORY "${libraries}")

set(staged "")
set(library_count 0)
set(plugin_count 0)
foreach(part IN LISTS parts)
    string(REGEX MATCH "^([^:]+):(.+)$" matched "${part}")
    set(kind "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    bloom_qt_file("${kind}" "${name}" file)
    get_filename_component(leaf "${file}" NAME)
    if(kind STREQUAL "module" AND OPERATING_SYSTEM STREQUAL "macos")
        if(NOT file MATCHES "^lib/([^/]+\\.framework)/")
            message(FATAL_ERROR "stage_qt.cmake: module ${name} is not a framework: ${file}")
        endif()
        set(framework "${CMAKE_MATCH_1}")
        # Replaced whole with cp -R, which keeps the links inside a framework as links.
        file(REMOVE_RECURSE "${libraries}/${framework}")
        execute_process(COMMAND cp -R "${QT}/lib/${framework}" "${libraries}/"
                        COMMAND_ERROR_IS_FATAL ANY)
        math(EXPR library_count "${library_count} + 1")
    elseif(kind STREQUAL "module")
        file(COPY "${QT}/${file}" DESTINATION "${libraries}")
        if(file_link AND NOT file_link STREQUAL leaf)
            file(CREATE_LINK "${leaf}" "${libraries}/${file_link}" SYMBOLIC)
        endif()
        list(APPEND staged "${libraries}/${leaf}")
        math(EXPR library_count "${library_count} + 1")
    else()
        if(NOT file MATCHES "^plugins/([^/]+)/[^/]+$")
            message(FATAL_ERROR "stage_qt.cmake: plugin ${name} is not in a plugin directory: "
                                "${file}")
        endif()
        file(COPY "${QT}/${file}" DESTINATION "${plugins}/${CMAKE_MATCH_1}")
        list(APPEND staged "${plugins}/${CMAKE_MATCH_1}/${leaf}")
        math(EXPR plugin_count "${plugin_count} + 1")
    endif()
endforeach()

set(other_count 0)
if(OPERATING_SYSTEM STREQUAL "linux")
    # What the staged files load from Qt's own lib directory: a Qt library must be listed; any
    # other (the Unicode libraries the Qt Project publishes with its binaries) follows.
    set(pending ${staged})
    while(pending)
        list(POP_FRONT pending item)
        execute_process(COMMAND readelf -d "${item}" OUTPUT_VARIABLE output
                        RESULT_VARIABLE result ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "readelf -d ${item} failed: ${error}")
        endif()
        string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]\n]+\\]" needed "${output}")
        foreach(entry IN LISTS needed)
            string(REGEX REPLACE "^.*\\[([^]]+)\\]$" "\\1" name "${entry}")
            if(EXISTS "${libraries}/${name}" OR NOT EXISTS "${QT}/lib/${name}")
                continue()
            endif()
            file(RELATIVE_PATH shown "${ROOT}" "${item}")
            if(name MATCHES "^libQt6")
                message(FATAL_ERROR "stage_qt.cmake: ${shown} loads ${name}, a Qt library "
                                    "${MODULES} does not list for ${OPERATING_SYSTEM}")
            endif()
            file(REAL_PATH "${QT}/lib/${name}" real)
            get_filename_component(leaf "${real}" NAME)
            file(COPY "${real}" DESTINATION "${libraries}")
            if(NOT leaf STREQUAL name)
                file(CREATE_LINK "${leaf}" "${libraries}/${name}" SYMBOLIC)
            endif()
            list(APPEND pending "${libraries}/${leaf}")
            math(EXPR other_count "${other_count} + 1")
        endforeach()
    endwhile()
elseif(OPERATING_SYSTEM STREQUAL "macos")
    # Qt reads Contents/Resources/qt.conf; its prefix in a bundle is Contents.
    file(WRITE "${qt}/Resources/qt.conf" "[Paths]\nPlugins = PlugIns\n")
else()
    file(RELATIVE_PATH relative "${ROOT}/${PROGRAM_DIR}" "${plugins}")
    file(WRITE "${ROOT}/${PROGRAM_DIR}/qt.conf" "[Paths]\nPrefix = .\nPlugins = ${relative}\n")
endif()

# Which archives these files came from.
file(READ "${QT}/bloom-prebuilt.txt" archives)
file(WRITE "${record}" "${archives}")
message(STATUS "Bloom package: carries ${library_count} Qt libraries, ${plugin_count} Qt "
               "plugins and ${other_count} libraries published with them")

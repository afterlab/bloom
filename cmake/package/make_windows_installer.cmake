# Makes the Windows installer of a Bloom package with the Inno Setup compiler
# (packaging/windows/bloom.iss; docs/architecture/platform-support.md, "Windows Package").
#
#   cmake -DISCC=<ISCC.exe> -DPACKAGE=<package folder> -DSOURCE=<source tree>
#         -DVERSION=<version label> -DOUTPUT_DIR=<folder for the installer>
#         [-DOUTPUT_NAME=<file name>] [-DDEFINES=<name>=<value>;<name>=<value>...]
#         -P make_windows_installer.cmake
#
# The installer is OUTPUT_DIR/<OUTPUT_NAME>.exe; the name is the package's own folder name with
# `-setup` (Bloom-<version>-windows-x86_64-setup) unless OUTPUT_NAME says another, with or
# without `.exe`. DEFINES are more values for the script (bloom.iss lists them); the package
# check's test uses them to make an installer of its own identity.
#
# NumericVersion, the four numbers of the file's version resource, is read from the label:
# 0.1.0-alpha.1 becomes 0.1.0.0.
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS ISCC PACKAGE SOURCE VERSION OUTPUT_DIR)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "make_windows_installer.cmake needs ${argument}")
    endif()
endforeach()
if(NOT EXISTS "${ISCC}")
    message(FATAL_ERROR "make_windows_installer.cmake: the Inno Setup compiler '${ISCC}' does "
                        "not exist")
endif()
if(NOT IS_DIRECTORY "${PACKAGE}")
    message(FATAL_ERROR "make_windows_installer.cmake: PACKAGE is not a directory: '${PACKAGE}'")
endif()
set(script "${SOURCE}/packaging/windows/bloom.iss")
if(NOT EXISTS "${script}")
    message(FATAL_ERROR "make_windows_installer.cmake: '${script}' does not exist")
endif()
if(NOT VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "make_windows_installer.cmake: the version label '${VERSION}' does not "
                        "start with three numbers")
endif()
set(numeric_version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0")

if(NOT OUTPUT_NAME)
    get_filename_component(package_name "${PACKAGE}" NAME)
    set(OUTPUT_NAME "${package_name}-setup")
endif()
string(REGEX REPLACE "\\.[eE][xX][eE]$" "" OUTPUT_NAME "${OUTPUT_NAME}")
get_filename_component(OUTPUT_DIR "${OUTPUT_DIR}" ABSOLUTE)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(installer "${OUTPUT_DIR}/${OUTPUT_NAME}.exe")
# A file left from an earlier run must not pass for this one.
file(REMOVE "${installer}")

file(TO_NATIVE_PATH "${PACKAGE}" native_package)
file(TO_NATIVE_PATH "${SOURCE}" native_source)
file(TO_NATIVE_PATH "${OUTPUT_DIR}" native_output)
file(TO_NATIVE_PATH "${script}" native_script)
set(defines
    "--define=AppVersion=${VERSION}"
    "--define=NumericVersion=${numeric_version}"
    "--define=SourceDir=${native_package}"
    "--define=RepositoryDir=${native_source}"
    "--define=OutputDir=${native_output}"
    "--define=OutputBaseFilename=${OUTPUT_NAME}")
foreach(define IN LISTS DEFINES)
    list(APPEND defines "--define=${define}")
endforeach()

execute_process(
    COMMAND "${ISCC}" --quiet ${defines} "${native_script}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The Inno Setup compiler failed (${result}):\n${output}${error}")
endif()
if(NOT EXISTS "${installer}")
    message(FATAL_ERROR "The Inno Setup compiler did not make ${installer}:\n${output}${error}")
endif()
message(STATUS "Made ${installer}")

# Where each part of a Bloom package goes (docs/architecture/platform-support.md, "Packages").
#
# Every install rule and every packaging script names a place through these variables, so the
# packages of the three operating systems are built by the same code and differ only here. Each
# is a path relative to the package's root (the install prefix; on macOS the directory that holds
# Bloom.app).
#
#                               Linux                     macOS, in Bloom.app/Contents  Windows
#   BLOOM_PACKAGE_PROGRAM_DIR   bin                       MacOS                         .
#   BLOOM_PACKAGE_LIBRARY_DIR   lib                       Frameworks                    .
#   BLOOM_PACKAGE_QT_DIR        lib/qt                    (Frameworks and PlugIns)      qt
#   BLOOM_PACKAGE_COLOR_DIR     libexec/bloom             MacOS                         .
#   BLOOM_PACKAGE_MEDIA_DIR     libexec/bloom/media       libexec/bloom/media           media
#   BLOOM_PACKAGE_PYTHON_DIR    share/bloom/python        Resources/python              python
#   BLOOM_PACKAGE_BLOOM_PYTHON_DIR
#                               share/bloom/bloom-python  Resources/bloom-python        bloom-python
#   BLOOM_PACKAGE_RECORD_DIR    share/bloom               Resources                     .
#
# The programs find each other through the same places at run time: the workers in
# src/media/provider/media_workers.cpp and src/color/ocio_gpu_program_worker.cpp, the interpreter
# in src/scripting/python/interpreter_home.cpp. A change here is a change there.
#
# The record directory holds `licenses` (one directory per component), the dependency lock and
# Bloom's own license and notices.
#
# Linux keeps the Qt libraries and plugins in one directory shaped like the Qt binaries
# themselves (lib/qt/lib, lib/qt/plugins): the plugins look for the libraries two directories up,
# and Qt finds the plugins beside its libraries, so nothing in them is rewritten. Windows has no
# search path inside a library: the Qt libraries sit beside the programs and qt.conf names the
# plugin directory.
include_guard(GLOBAL)

if(APPLE)
    set(bloom_package_contents "Bloom.app/Contents")
    set(BLOOM_PACKAGE_PROGRAM_DIR "${bloom_package_contents}/MacOS")
    set(BLOOM_PACKAGE_LIBRARY_DIR "${bloom_package_contents}/Frameworks")
    set(BLOOM_PACKAGE_QT_DIR "${bloom_package_contents}")
    set(BLOOM_PACKAGE_COLOR_DIR "${bloom_package_contents}/MacOS")
    set(BLOOM_PACKAGE_MEDIA_DIR "${bloom_package_contents}/libexec/bloom/media")
    set(BLOOM_PACKAGE_PYTHON_DIR "${bloom_package_contents}/Resources/python")
    set(BLOOM_PACKAGE_BLOOM_PYTHON_DIR "${bloom_package_contents}/Resources/bloom-python")
    set(BLOOM_PACKAGE_RECORD_DIR "${bloom_package_contents}/Resources")
    set(BLOOM_PACKAGE_OPERATING_SYSTEM "macos")
elseif(WIN32)
    set(BLOOM_PACKAGE_PROGRAM_DIR ".")
    set(BLOOM_PACKAGE_LIBRARY_DIR ".")
    set(BLOOM_PACKAGE_QT_DIR "qt")
    set(BLOOM_PACKAGE_COLOR_DIR ".")
    set(BLOOM_PACKAGE_MEDIA_DIR "media")
    set(BLOOM_PACKAGE_PYTHON_DIR "python")
    set(BLOOM_PACKAGE_BLOOM_PYTHON_DIR "bloom-python")
    set(BLOOM_PACKAGE_RECORD_DIR ".")
    set(BLOOM_PACKAGE_OPERATING_SYSTEM "windows")
else()
    set(BLOOM_PACKAGE_PROGRAM_DIR "bin")
    set(BLOOM_PACKAGE_LIBRARY_DIR "lib")
    set(BLOOM_PACKAGE_QT_DIR "lib/qt")
    set(BLOOM_PACKAGE_COLOR_DIR "libexec/bloom")
    set(BLOOM_PACKAGE_MEDIA_DIR "libexec/bloom/media")
    set(BLOOM_PACKAGE_PYTHON_DIR "share/bloom/python")
    set(BLOOM_PACKAGE_BLOOM_PYTHON_DIR "share/bloom/bloom-python")
    set(BLOOM_PACKAGE_RECORD_DIR "share/bloom")
    set(BLOOM_PACKAGE_OPERATING_SYSTEM "linux")
endif()

# Gives an installed Linux program the search path of the package's libraries. (macOS programs
# name theirs where they are bundled; Windows has none.) `from` is the program's own directory
# in the package.
function(bloom_package_search_path target from)
    if(NOT UNIX OR APPLE)
        return()
    endif()
    file(RELATIVE_PATH up "/${from}" "/")
    string(REGEX REPLACE "/$" "" up "${up}")
    if(up STREQUAL "")
        set(up ".")
    endif()
    set_property(TARGET ${target} PROPERTY INSTALL_RPATH
        "$ORIGIN/${up}/${BLOOM_PACKAGE_LIBRARY_DIR}"
        "$ORIGIN/${up}/${BLOOM_PACKAGE_QT_DIR}/lib")
endfunction()

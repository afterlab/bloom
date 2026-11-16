# Writes a release's Arch Linux package description from packaging/aur/PKGBUILD.in
# (docs/architecture/platform-support.md, "Linux Package").
#
#   cmake -DVERSION=<the release's label, for example 0.1.0-alpha.1>
#         -DSHA256=<the checksum of Bloom-<label>-linux-x86_64.tar.xz>
#         -DOUTPUT_DIR=<the folder that receives PKGBUILD and .SRCINFO>
#         -P make_pkgbuild.cmake
#
# `depends` are the Arch Linux packages of the libraries marked `required` in
# tools/install/linux-library-packages.txt; `optdepends` are the `optional` ones, each with the
# reason the library is wanted. The license is named after the repository's LICENSE. A library the
# table names that this file has no reason for stops it, so the lists cannot drift apart.
cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS VERSION SHA256 OUTPUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "make_pkgbuild.cmake: ${required} is not set")
    endif()
endforeach()
if(NOT VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-([0-9A-Za-z.]+))?$")
    message(FATAL_ERROR "make_pkgbuild.cmake: '${VERSION}' is not a version")
endif()
set(core "${CMAKE_MATCH_1}")
set(prerelease "${CMAKE_MATCH_3}")
string(LENGTH "${SHA256}" sha_length)
if(NOT SHA256 MATCHES "^[0-9a-f]+$" OR NOT sha_length EQUAL 64)
    message(FATAL_ERROR "make_pkgbuild.cmake: SHA256 must be 64 lowercase hexadecimal digits")
endif()

# A pkgver cannot hold a hyphen, and the label's dots after it only separate its parts.
string(REPLACE "." "" prerelease "${prerelease}")
set(pkgver "${core}${prerelease}")

set(source "${CMAKE_CURRENT_LIST_DIR}/../..")

file(READ "${source}/LICENSE" license_text LIMIT 400)
if(license_text MATCHES "Apache License[ \r\n]+Version 2\\.0")
    set(license "Apache-2.0")
else()
    message(FATAL_ERROR "make_pkgbuild.cmake: LICENSE is not a license this file knows")
endif()

set(reason_libgbm.so.1 "hardware video decoding")
set(reason_libdrm.so.2 "hardware video decoding")
set(reason_libxcb-xinput.so.0 "tablet and touch input")
set(reason_libasound.so.2 "audio output through ALSA")
set(reason_libpulse.so.0 "audio output through PulseAudio")
set(reason_libjack.so.0 "audio output through JACK")
set(reason_libpipewire-0.3.so.0 "audio output through PipeWire")
set(reason_libcurl.so.4 "font downloads in bloom-cli")

file(STRINGS "${source}/tools/install/linux-library-packages.txt" lines)
set(depends "")
set(optional "")
set(optional_reasons "")
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
    set(arch_package "${CMAKE_MATCH_5}")
    if(CMAKE_MATCH_2 STREQUAL "required")
        list(APPEND depends "${arch_package}")
    else()
        if(NOT DEFINED reason_${library})
            message(FATAL_ERROR "make_pkgbuild.cmake: the optional library ${library} has no "
                                "reason for optdepends")
        endif()
        list(APPEND optional "${arch_package}")
        list(APPEND optional_reasons "${reason_${library}}")
    endif()
endforeach()
list(REMOVE_DUPLICATES depends)

# An optional package that is also required, or that several libraries ask for, is listed once
# with the first reason.
set(optdepends "")
set(seen "")
list(LENGTH optional count)
math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    list(GET optional ${index} arch_package)
    list(GET optional_reasons ${index} reason)
    if(arch_package IN_LIST depends OR arch_package IN_LIST seen)
        continue()
    endif()
    list(APPEND seen "${arch_package}")
    list(APPEND optdepends "${arch_package}: ${reason}")
endforeach()

set(depends_text "")
set(depends_info "")
foreach(item IN LISTS depends)
    string(APPEND depends_text " '${item}'")
    string(APPEND depends_info "\tdepends = ${item}\n")
endforeach()
string(STRIP "${depends_text}" depends_text)
set(optdepends_text "")
set(optdepends_info "")
foreach(item IN LISTS optdepends)
    string(APPEND optdepends_text " '${item}'")
    string(APPEND optdepends_info "\toptdepends = ${item}\n")
endforeach()
string(STRIP "${optdepends_text}" optdepends_text)

file(READ "${source}/packaging/aur/PKGBUILD.in" pkgbuild)
string(REPLACE "@BLOOM_PKG_LABEL@" "${VERSION}" pkgbuild "${pkgbuild}")
string(REPLACE "@BLOOM_PKG_VERSION@" "${pkgver}" pkgbuild "${pkgbuild}")
string(REPLACE "@BLOOM_PKG_LICENSE@" "${license}" pkgbuild "${pkgbuild}")
string(REPLACE "@BLOOM_PKG_DEPENDS@" "${depends_text}" pkgbuild "${pkgbuild}")
string(REPLACE "@BLOOM_PKG_OPTDEPENDS@" "${optdepends_text}" pkgbuild "${pkgbuild}")
string(REPLACE "@BLOOM_PKG_SHA256@" "${SHA256}" pkgbuild "${pkgbuild}")
if(pkgbuild MATCHES "@BLOOM_PKG_[A-Z_]+@")
    message(FATAL_ERROR "make_pkgbuild.cmake: ${CMAKE_MATCH_0} was not filled in")
endif()

set(url "https://github.com/afterlab/bloom/releases/download/v${VERSION}/Bloom-${VERSION}-linux-x86_64.tar.xz")
set(srcinfo "pkgbase = bloom-bin
\tpkgdesc = Artist-focused motion graphics and visual effects software (prebuilt release)
\tpkgver = ${pkgver}
\tpkgrel = 1
\turl = https://github.com/afterlab/bloom
\tarch = x86_64
\tlicense = ${license}
${depends_info}${optdepends_info}\tprovides = bloom
\tconflicts = bloom
\toptions = !strip
\tsource = ${url}
\tsha256sums = ${SHA256}

pkgname = bloom-bin
")

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/PKGBUILD" "${pkgbuild}")
file(WRITE "${OUTPUT_DIR}/.SRCINFO" "${srcinfo}")

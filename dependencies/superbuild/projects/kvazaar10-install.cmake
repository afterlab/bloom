# Installs the 10-bit Kvazaar build (kvazaar.cmake) beside the 8-bit one:
#   -DKVZ10_STAGE=<Kvazaar's own install tree> -DKVZ10_PREFIX=<dependency prefix>
#   -DKVZ10_SOURCE_DIR=<this directory> -DKVZ10_VERSION=<version>
#   -DKVZ10_NM=<nm> | -DKVZ10_DUMPBIN=<dumpbin> (MSVC)
# The archive becomes lib/libkvazaar10.a, the headers include/kvazaar10/, and the pkg-config
# record kvazaar10.pc, so nothing of the 8-bit install is overwritten. Before installing it checks
# that the archive defines no external symbol without the kvz10_ prefix (kvazaar10-symbols.h):
# an unprefixed symbol could resolve against the 8-bit archive inside libavcodec.
foreach(required KVZ10_STAGE KVZ10_PREFIX KVZ10_SOURCE_DIR KVZ10_VERSION)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "kvazaar10-install.cmake requires ${required}")
    endif()
endforeach()

# Kvazaar names its static archive libkvazaar.a, or libkvazaar.lib with MSVC.
set(kvz10_archive)
foreach(candidate libkvazaar.a libkvazaar.lib kvazaar.lib)
    if(EXISTS "${KVZ10_STAGE}/lib/${candidate}")
        set(kvz10_archive "${KVZ10_STAGE}/lib/${candidate}")
        break()
    endif()
endforeach()
if(NOT kvz10_archive)
    message(FATAL_ERROR "kvazaar10: no static archive under ${KVZ10_STAGE}/lib")
endif()

set(kvz10_unprefixed)
if(KVZ10_NM)
    # ELF and Mach-O: every defined external symbol must carry the prefix (Mach-O adds a leading
    # underscore to C names).
    execute_process(COMMAND "${KVZ10_NM}" -g "${kvz10_archive}"
        OUTPUT_VARIABLE kvz10_symbols RESULT_VARIABLE kvz10_status)
    if(NOT kvz10_status EQUAL 0)
        message(FATAL_ERROR "kvazaar10: ${KVZ10_NM} could not list ${kvz10_archive}")
    endif()
    string(REPLACE "\n" ";" kvz10_lines "${kvz10_symbols}")
    foreach(line IN LISTS kvz10_lines)
        if(line MATCHES "^[0-9A-Fa-f]+ [A-TV-Z] _?([^ ]+)$")
            if(NOT CMAKE_MATCH_1 MATCHES "^kvz10_")
                list(APPEND kvz10_unprefixed "${CMAKE_MATCH_1}")
            endif()
        endif()
    endforeach()
elseif(KVZ10_DUMPBIN)
    # MSVC objects also define shared COMDAT helpers (C++ templates, CRT inline functions,
    # floating-point constants) that the linker folds, so the check names Kvazaar's own symbols:
    # no kvz_ symbol and none of the MSVC-only replacements may stay unprefixed.
    execute_process(COMMAND "${KVZ10_DUMPBIN}" /nologo /symbols "${kvz10_archive}"
        OUTPUT_VARIABLE kvz10_symbols RESULT_VARIABLE kvz10_status)
    if(NOT kvz10_status EQUAL 0)
        message(FATAL_ERROR "kvazaar10: ${KVZ10_DUMPBIN} could not list ${kvz10_archive}")
    endif()
    string(REPLACE "\n" ";" kvz10_lines "${kvz10_symbols}")
    foreach(line IN LISTS kvz10_lines)
        if(line MATCHES "SECT[0-9A-F]+ .* External +\\| ([^ ]+)")
            if(CMAKE_MATCH_1 MATCHES "^(kvz_|getopt|opt(arg|err|ind|opt|reset)$|pthread_|sem_)")
                list(APPEND kvz10_unprefixed "${CMAKE_MATCH_1}")
            endif()
        endif()
    endforeach()
else()
    message(FATAL_ERROR "kvazaar10-install.cmake requires KVZ10_NM or KVZ10_DUMPBIN")
endif()
if(kvz10_unprefixed)
    list(REMOVE_DUPLICATES kvz10_unprefixed)
    list(JOIN kvz10_unprefixed ", " kvz10_unprefixed)
    message(FATAL_ERROR "kvazaar10: symbols without the kvz10_ prefix (add them to "
        "kvazaar10-symbols.h): ${kvz10_unprefixed}")
endif()

file(MAKE_DIRECTORY "${KVZ10_PREFIX}/lib/pkgconfig" "${KVZ10_PREFIX}/include/kvazaar10")
file(COPY_FILE "${kvz10_archive}" "${KVZ10_PREFIX}/lib/libkvazaar10.a")
file(COPY_FILE "${KVZ10_STAGE}/include/kvazaar.h" "${KVZ10_PREFIX}/include/kvazaar10/kvazaar.h")
foreach(header kvazaar10.h kvazaar10-symbols.h)
    file(COPY_FILE "${KVZ10_SOURCE_DIR}/${header}" "${KVZ10_PREFIX}/include/kvazaar10/${header}")
endforeach()
file(WRITE "${KVZ10_PREFIX}/lib/pkgconfig/kvazaar10.pc"
"prefix=${KVZ10_PREFIX}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libkvazaar10
Description: Kvazaar HEVC encoder built for 10-bit samples with kvz10_ symbols
URL: https://github.com/ultravideo/kvazaar
Version: ${KVZ10_VERSION}
Libs: -L\${libdir} -lkvazaar10
Cflags: -I\${includedir}
")

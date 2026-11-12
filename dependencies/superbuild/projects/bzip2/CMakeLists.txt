# Builds libbz2 as a static library for the carried Python's `bz2` module (../bzip2.cmake copies
# this file into the unpacked archive). The sources are the library objects of the archive's own
# Makefile; the programs are left out.
cmake_minimum_required(VERSION 3.25)
project(BloomBzip2 LANGUAGES C)

add_library(bz2 STATIC
    blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c)
target_compile_definitions(bz2 PRIVATE _FILE_OFFSET_BITS=64)

install(TARGETS bz2 ARCHIVE DESTINATION lib)
install(FILES bzlib.h DESTINATION include)

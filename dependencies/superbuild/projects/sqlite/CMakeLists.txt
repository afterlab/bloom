# Builds the SQLite amalgamation as a static library for the carried Python's `sqlite3` module
# (../sqlite.cmake copies this file into the unpacked archive). The compile-time options are the
# ones CPython's own Windows build of SQLite sets (PCbuild/sqlite3.vcxproj), so the module has the
# same features on every platform.
cmake_minimum_required(VERSION 3.25)
project(BloomSqlite LANGUAGES C)

add_library(sqlite3 STATIC sqlite3.c)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_MATH_FUNCTIONS
    SQLITE_ENABLE_FTS4
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_RTREE
    SQLITE_OMIT_AUTOINIT
    SQLITE_THREADSAFE=1)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

install(TARGETS sqlite3 ARCHIVE DESTINATION lib)
install(FILES sqlite3.h sqlite3ext.h DESTINATION include)

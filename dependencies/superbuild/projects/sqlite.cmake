# SQLite 3.54.1 (public domain, blessing; see dependencies/licenses/sqlite/) - the database library
# behind the carried Python's `sqlite3` module (and `dbm.sqlite3`). Bloom itself does not link it.
#
# Built for every profile (python.cmake, issue #677), so a script's database behaves the same on
# every platform instead of following the operating system's SQLite. The amalgamation archive's
# one source file is compiled by the adjacent sqlite/CMakeLists.txt into a static library, with
# the options CPython's own Windows build uses; the archive's configure script, shell program and
# Tcl extension are not used. Installed into the prefix's private `python-support` directory. On
# Windows CPython's own project files compile the same amalgamation with the same options
# (python-windows.cmake), so it is only unpacked there.
set(BLOOM_SQLITE_VERSION 3.54.1)
set(BLOOM_SQLITE_URL "https://www.sqlite.org/2026/sqlite-autoconf-3540100.tar.gz")
set(BLOOM_SQLITE_SHA256 926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e)

if(WIN32)
    # The archive is unpacked only; CPython's project files compile it (python-windows.cmake).
    ExternalProject_Add(bloom_dependency_sqlite
        URL "${BLOOM_SQLITE_URL}"
        URL_HASH SHA256=${BLOOM_SQLITE_SHA256}
        DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        DOWNLOAD_NAME sqlite-autoconf-3540100.tar.gz
        DOWNLOAD_NO_PROGRESS ON
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
        BUILD_ALWAYS OFF
        USES_TERMINAL_DOWNLOAD ON)
    ExternalProject_Get_Property(bloom_dependency_sqlite SOURCE_DIR)
    set(BLOOM_SQLITE_SOURCE_DIR "${SOURCE_DIR}")
    return()
endif()

ExternalProject_Add(bloom_dependency_sqlite
    URL "${BLOOM_SQLITE_URL}"
    URL_HASH SHA256=${BLOOM_SQLITE_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME sqlite-autoconf-3540100.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_LIST_DIR}/sqlite/CMakeLists.txt" "<SOURCE_DIR>/CMakeLists.txt"
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_INSTALL_PREFIX:PATH=${BLOOM_DEPENDENCY_PREFIX}/python-support
    BUILD_ALWAYS OFF
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

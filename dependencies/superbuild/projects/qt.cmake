# Qt (LGPL-3.0-only; see dependencies/licenses/qt/) - the desktop application's toolkit.
#
# Qt is not built here. Every profile takes the Qt Project's official open-source binaries of the
# one version the dependency lock records, the archives named by the `prebuilt` list of the lock's
# `qt` component: prebuilt-fetch.cmake checks each archive's size and SHA-256 against the lock and
# unpacks them, unmodified, into the prefix's `qt` directory. No version, URL or checksum is
# written here: the lock is the only place.
#
# Bloom links Qt dynamically and a qualified Bloom build takes Qt from that directory only
# (cmake/BloomLockedQt.cmake).
if(WIN32)
    set(bloom_qt_system windows)
elseif(APPLE)
    set(bloom_qt_system macos)
else()
    set(bloom_qt_system linux)
endif()
set(bloom_qt_fetch_arguments
    "-DBLOOM_LOCK=${CMAKE_CURRENT_LIST_DIR}/../../dependencies.lock.json"
    -DBLOOM_COMPONENT=qt
    -DBLOOM_OPERATING_SYSTEM=${bloom_qt_system}
    "-DBLOOM_DESTINATION=${BLOOM_DEPENDENCY_PREFIX}/qt"
    "-DBLOOM_DOWNLOAD_DIR=${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    -DBLOOM_ALLOW_DOWNLOADS=${BLOOM_DEPENDENCY_ALLOW_DOWNLOADS}
    # The Linux package's Unicode libraries are published as bare files (the archive's name is the
    # package version followed by "icu-").
    "-DBLOOM_LIBRARY_ARCHIVES=(^|[0-9])icu-")
# Strictly offline: like every recipe's archive, Qt's must already be in the cache with their
# locked SHA-256 when the superbuild is configured, so the build never reaches the network.
if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${bloom_qt_fetch_arguments} -DBLOOM_CHECK_ONLY=ON
                -P "${CMAKE_CURRENT_LIST_DIR}/prebuilt-fetch.cmake"
        RESULT_VARIABLE bloom_qt_cache_result
        OUTPUT_VARIABLE bloom_qt_cache_output
        ERROR_VARIABLE bloom_qt_cache_output)
    if(NOT bloom_qt_cache_result EQUAL 0)
        message(FATAL_ERROR
            "BLOOM_DEPENDENCY_ALLOW_DOWNLOADS is OFF and the archive cache "
            "${BLOOM_DEPENDENCY_DOWNLOAD_DIR} does not hold the locked Qt binaries:\n"
            "${bloom_qt_cache_output}")
    endif()
endif()
# Runs on every build: the script leaves a directory that already holds the locked archives
# untouched, and replaces one that holds anything else.
add_custom_target(bloom_dependency_qt ALL
    COMMAND "${CMAKE_COMMAND}" ${bloom_qt_fetch_arguments}
        -P "${CMAKE_CURRENT_LIST_DIR}/prebuilt-fetch.cmake"
    COMMENT "Checking the locked Qt binaries in the dependency prefix"
    VERBATIM)

# Tests the qualified build's refusal of a Qt that is not the locked one (the
# bloom.quality.locked_qt_refusal test).
#
#   cmake -DBLOOM_WORK=<empty scratch directory> -P BloomLockedQtTest.cmake
cmake_minimum_required(VERSION 3.25)
if(NOT BLOOM_WORK)
    message(FATAL_ERROR "BloomLockedQtTest.cmake needs BLOOM_WORK")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/BloomLockedQt.cmake")

bloom_locked_qt_version(locked)
if(NOT locked MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "the dependency lock records no exact Qt version: '${locked}'")
endif()

file(REMOVE_RECURSE "${BLOOM_WORK}")
file(MAKE_DIRECTORY "${BLOOM_WORK}/absent" "${BLOOM_WORK}/other" "${BLOOM_WORK}/locked")

bloom_locked_qt_problem(problem "${BLOOM_WORK}/absent" "${locked}")
if(NOT problem MATCHES "does not hold the locked Qt ${locked}")
    message(FATAL_ERROR "a prefix without Qt was not refused with the reason: '${problem}'")
endif()

# Another version: an older release and a newer patch of the locked series.
foreach(other IN ITEMS 6.8.3 "${locked}1")
    file(WRITE "${BLOOM_WORK}/other/bloom-prebuilt.txt" "qt ${other}\n0 0 archive.7z\n")
    bloom_locked_qt_problem(problem "${BLOOM_WORK}/other" "${locked}")
    if(NOT problem MATCHES "holds 'qt ${other}', but the dependency lock records Qt ${locked}")
        message(FATAL_ERROR "Qt ${other} was not refused with the reason: '${problem}'")
    endif()
endforeach()

file(WRITE "${BLOOM_WORK}/locked/bloom-prebuilt.txt" "qt ${locked}\n0 0 archive.7z\n")
bloom_locked_qt_problem(problem "${BLOOM_WORK}/locked" "${locked}")
if(problem)
    message(FATAL_ERROR "the locked Qt ${locked} was refused: '${problem}'")
endif()
file(REMOVE_RECURSE "${BLOOM_WORK}")
message(STATUS "BloomLockedQt.cmake: Qt ${locked} accepted; an absent and another Qt refused")

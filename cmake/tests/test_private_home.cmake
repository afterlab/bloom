# bloom.quality.test_private_home: on macOS no registered test reaches the user's real home.
#
#   cmake -DTREE=<build directory> [-DCONFIG=<configuration>] [-DEXPECT_REJECTED=<name,...>]
#         -P test_private_home.cmake
#
# macOS finds every per-user folder from the home folder: Bloom's cache and data folders
# (~/Library/Caches/bloom, ~/Library/Application Support/Afterlab/Bloom), Qt's standard locations
# and the preferences QSettings writes. The XDG_* and LOCALAPPDATA overrides that keep a test's
# per-user state away from the user's on Linux and Windows do not reach most of them there, so
# bloom_add_test gives each test a home of its own, <build>/test-home/<test name>, in HOME and in
# CFFIXED_USER_HOME, the variable CoreFoundation and Foundation take the home from
# (cmake/BloomTesting.cmake). This check reads the CTest files the build wrote, as
# test_memory_reading.cmake does, and rejects by name every test that runs without that home: no
# HOME, a HOME that is not test-home/<its own name>, or a CFFIXED_USER_HOME that is missing or
# names another folder. With EXPECT_REJECTED it passes only when it rejects exactly those names
# (the check's own test, against cmake/tests/test_private_home).

include("${CMAKE_CURRENT_LIST_DIR}/read_ctest_files.cmake")

get_property(tests GLOBAL PROPERTY bloom_ctest_tests)
list(LENGTH tests test_count)
if(test_count EQUAL 0)
    message(FATAL_ERROR "no tests were found under ${TREE}")
endif()
set(rejected)
foreach(name IN LISTS tests)
    get_property(environment GLOBAL PROPERTY "bloom_ctest_ENVIRONMENT_${name}")
    # The later entry wins, as in ctest.
    set(home "")
    set(fixed "")
    foreach(entry IN LISTS environment)
        if(entry MATCHES "^HOME=(.*)$")
            set(home "${CMAKE_MATCH_1}")
        elseif(entry MATCHES "^CFFIXED_USER_HOME=(.*)$")
            set(fixed "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    get_filename_component(home_name "${home}" NAME)
    get_filename_component(home_parent "${home}" DIRECTORY)
    get_filename_component(home_parent "${home_parent}" NAME)
    if(home STREQUAL "")
        message(STATUS "rejected: ${name} runs with the user's own home (no HOME of its own)")
    elseif(NOT home_parent STREQUAL "test-home" OR NOT home_name STREQUAL name)
        message(STATUS "rejected: ${name} runs with HOME=${home}, not test-home/${name}")
    elseif(NOT fixed STREQUAL home)
        message(STATUS "rejected: ${name} runs with CFFIXED_USER_HOME='${fixed}', not its HOME")
    else()
        continue()
    endif()
    list(APPEND rejected "${name}")
endforeach()

if(DEFINED EXPECT_REJECTED)
    list(SORT rejected)
    string(REPLACE "," ";" expected "${EXPECT_REJECTED}")
    list(SORT expected)
    if(NOT rejected STREQUAL expected)
        message(FATAL_ERROR "the check rejected [${rejected}], expected [${expected}]")
    endif()
    message(STATUS "the check rejected exactly [${rejected}] of ${test_count} tests")
    return()
endif()
list(LENGTH rejected rejected_count)
if(rejected_count GREATER 0)
    message(FATAL_ERROR
        "${rejected_count} of ${test_count} tests could reach the user's real home on macOS. "
        "Register them with bloom_add_test (cmake/BloomTesting.cmake), which gives every test a "
        "home of its own there, and do not replace their ENVIRONMENT afterwards.")
endif()
message(STATUS "all ${test_count} tests run with a home of their own")

# bloom.quality.test_memory_reading: no registered test plans against the real machine's free memory
# unless it is a named stress exception (#750).
#
#   cmake -DTREE=<build directory> [-DCONFIG=<configuration>] [-DEXPECT_REJECTED=<name,...>]
#         -P test_memory_reading.cmake
#
# It reads the CTest files the build wrote (CTestTestfile.cmake in TREE and every subdirectory they
# name), for CONFIG on a multi-configuration generator, the way ctest reads them. Every test ctest
# would run must have the fixed memory reading in its environment
# (BLOOM_TEST_FIXED_MEMORY_READING, cmake/BloomTesting.cmake), or be a named exception: the label
# `stress` and a non-empty reason in BLOOM_TEST_LIVE_MEMORY (bloom_add_test's LIVE_MEMORY). Any
# other test is rejected by name: it would plan its admission and device work against whatever else
# the machine runs. With EXPECT_REJECTED the script passes only when it rejects exactly those names
# (the check's own test, against cmake/tests/test_memory_reading).

include("${CMAKE_CURRENT_LIST_DIR}/read_ctest_files.cmake")

get_property(tests GLOBAL PROPERTY bloom_ctest_tests)
list(LENGTH tests test_count)
if(test_count EQUAL 0)
    message(FATAL_ERROR "no tests were found under ${TREE}")
endif()
set(rejected)
foreach(name IN LISTS tests)
    get_property(environment GLOBAL PROPERTY "bloom_ctest_ENVIRONMENT_${name}")
    get_property(labels GLOBAL PROPERTY "bloom_ctest_LABELS_${name}")
    set(fixed FALSE)
    set(reason "")
    foreach(entry IN LISTS environment)
        if(entry MATCHES "^BLOOM_TEST_FIXED_MEMORY_READING=.+")
            set(fixed TRUE)
        elseif(entry MATCHES "^BLOOM_TEST_LIVE_MEMORY=(.*)$")
            string(STRIP "${CMAKE_MATCH_1}" reason)
        endif()
    endforeach()
    if(fixed)
        continue()
    endif()
    if(NOT reason STREQUAL "" AND "stress" IN_LIST labels)
        message(STATUS "named live-memory stress test: ${name} (${reason})")
        continue()
    endif()
    if(NOT reason STREQUAL "")
        message(STATUS "rejected: ${name} names a live-memory reason but is in the default suite")
    else()
        message(STATUS "rejected: ${name} runs without the fixed memory reading and names no "
                       "live-memory reason")
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
        "${rejected_count} of ${test_count} tests plan against the real machine's free memory. "
        "Register them with bloom_add_test (cmake/BloomTesting.cmake), which gives every test the "
        "fixed memory reading, and do not replace their ENVIRONMENT afterwards; a test that exists "
        "to measure the real machine is STRESS with LIVE_MEMORY \"<reason>\".")
endif()
message(STATUS "all ${test_count} tests run with the fixed memory reading or are named stress "
               "exceptions")

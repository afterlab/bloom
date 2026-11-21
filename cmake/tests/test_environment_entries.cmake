# bloom.quality.test_environment_entries: every variable a test registration names reaches the
# test as its own variable, and a display-scale variant runs at its scale (#763).
#
#   cmake -DTREE=<build directory> [-DCONFIG=<configuration>] [-DEXPECT_REJECTED=<name,...>]
#         -P test_environment_entries.cmake
#
# bloom_add_test refuses ENVIRONMENT "A=1;B=2" when the build is configured
# (cmake/BloomTestEnvironment.cmake): cmake_parse_arguments(PARSE_ARGV) keeps that quoted argument
# whole, so CTest would set A to "1;B=2" and never set B. This check reads the CTest files the
# build wrote, as test_private_home.cmake does, so it also sees a test registered or changed some
# other way, and rejects by name every test that
#   - has an environment entry holding another NAME= after a semicolon (one joined variable), or
#   - is named for a display scale (<name>.dpr<scale> or <name>-scale-<scale>) and does not run
#     with QT_SCALE_FACTOR=<scale> as a variable of its own (the later entry wins, as in ctest).
# With EXPECT_REJECTED it passes only when it rejects exactly those names (the check's own test,
# against cmake/tests/test_environment_entries, which holds the joined form CMake wrote before).

include("${CMAKE_CURRENT_LIST_DIR}/read_ctest_files.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../BloomTestEnvironment.cmake")

get_property(tests GLOBAL PROPERTY bloom_ctest_tests)
list(LENGTH tests test_count)
if(test_count EQUAL 0)
    message(FATAL_ERROR "no tests were found under ${TREE}")
endif()
set(rejected)
set(scale_variants 0)
foreach(name IN LISTS tests)
    get_property(environment GLOBAL PROPERTY "bloom_ctest_ENVIRONMENT_${name}")
    bloom_joined_test_environment(joined environment)
    if(joined)
        list(JOIN joined ", " joined)
        message(STATUS "rejected: ${name} passes several variables as one (${joined})")
        list(APPEND rejected "${name}")
        continue()
    endif()
    if(name MATCHES "(\\.dpr|-scale-)([0-9]+(\\.[0-9]+)?)$")
        set(wanted "${CMAKE_MATCH_2}")
        math(EXPR scale_variants "${scale_variants} + 1")
        set(scale "")
        foreach(entry IN LISTS environment)
            if(entry MATCHES "^QT_SCALE_FACTOR=(.*)$")
                set(scale "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        if(NOT scale STREQUAL wanted)
            message(STATUS
                "rejected: ${name} is a scale ${wanted} variant and runs with QT_SCALE_FACTOR='${scale}'")
            list(APPEND rejected "${name}")
        endif()
    endif()
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
        "${rejected_count} of ${test_count} tests do not get the environment their registration "
        "names. Pass each variable as its own ENVIRONMENT argument (ENVIRONMENT \"A=1\" \"B=2\"), "
        "and give a scale variant QT_SCALE_FACTOR=<its scale>.")
endif()
message(STATUS "all ${test_count} tests get each variable on its own; "
               "${scale_variants} scale variants run at their scale")

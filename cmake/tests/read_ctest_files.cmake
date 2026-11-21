# Reads the CTest files a build wrote, the way ctest reads them, for the quality checks that look
# at every registered test (test_memory_reading.cmake, test_private_home.cmake). TREE names the
# build directory; CONFIG, when set, the configuration of a multi-configuration generator. After
# it, the global property bloom_ctest_tests lists every test ctest would run, and
# bloom_ctest_ENVIRONMENT_<name> and bloom_ctest_LABELS_<name> hold each one's properties.

if(NOT DEFINED TREE OR NOT EXISTS "${TREE}/CTestTestfile.cmake")
    message(FATAL_ERROR "TREE must name a build directory with a CTestTestfile.cmake")
endif()
set(CTEST_CONFIGURATION_TYPE "${CONFIG}")
set_property(GLOBAL PROPERTY bloom_ctest_tests "")

# ctest's own commands in a CTest file, recorded instead of run.
function(add_test name)
    if(ARGC GREATER 1 AND "${ARGV1}" STREQUAL "NOT_AVAILABLE")
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY bloom_ctest_tests "${name}")
endfunction()

function(set_tests_properties)
    set(names)
    set(index 0)
    while(index LESS ARGC AND NOT "${ARGV${index}}" STREQUAL "PROPERTIES")
        list(APPEND names "${ARGV${index}}")
        math(EXPR index "${index} + 1")
    endwhile()
    math(EXPR index "${index} + 1")
    while(index LESS ARGC)
        set(key "${ARGV${index}}")
        math(EXPR value_index "${index} + 1")
        if(value_index LESS ARGC)
            set(value "${ARGV${value_index}}")
        else()
            set(value "")
        endif()
        if(key STREQUAL "ENVIRONMENT" OR key STREQUAL "LABELS")
            foreach(name IN LISTS names)
                set_property(GLOBAL PROPERTY "bloom_ctest_${key}_${name}" "${value}")
            endforeach()
        endif()
        math(EXPR index "${index} + 2")
    endwhile()
endfunction()

function(subdirs)
    set(base "${CMAKE_CURRENT_LIST_DIR}")
    foreach(directory IN LISTS ARGV)
        if(NOT IS_ABSOLUTE "${directory}")
            set(directory "${base}/${directory}")
        endif()
        if(EXISTS "${directory}/CTestTestfile.cmake")
            include("${directory}/CTestTestfile.cmake")
        endif()
    endforeach()
endfunction()

include("${TREE}/CTestTestfile.cmake")


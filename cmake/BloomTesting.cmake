include_guard(GLOBAL)

include(BloomVulkanRuntime)

# Tests prove one behavior at the smallest size that shows it. Three kinds of run are not part of
# the default suite and are registered only when asked for:
#   STRESS      large sizes, long fills, soaks, real-machine memory (label `stress`)
#   BENCH       benchmark programs kept runnable (label `bench`)
#   COMPOSITOR  tests that need a private compositor session and would otherwise exit as skipped
#               (label `compositor`)
option(BLOOM_ENABLE_STRESS_TESTS "Register stress tests (large sizes, soaks, real-machine memory)" OFF)
option(BLOOM_ENABLE_BENCH_TESTS "Register benchmark smoke runs as tests" OFF)
option(BLOOM_ENABLE_COMPOSITOR_TESTS
       "Register tests that need a private compositor session (run them inside one)" OFF)

# The memory reading a test hands to Bloom programs it starts (bloom-mcp, bloom-cli, the Python
# module), which cannot be given a sampler the way an in-process test gives its own: 16 GiB of
# physical memory with 8 GiB available and an 8 GiB card, in bytes. The programs read it from
# BLOOM_TEST_FIXED_MEMORY_READING (bloom/core/test_fixed_memory_reading.hpp); it is test-only.
set(BLOOM_TEST_FIXED_MEMORY_READING "17179869184,8589934592,8589934592"
    CACHE INTERNAL "Fixed memory reading for tests that start Bloom programs")

# bloom_add_test(NAME <name> COMMAND <program> [args...]
#                [LABELS ...] [ENVIRONMENT ...] [TIMEOUT <seconds>] [WORKING_DIRECTORY <dir>]
#                [SKIP_RETURN_CODE <code>] [RUN_SERIAL] [FIXED_MEMORY] [STRESS] [BENCH] [COMPOSITOR])
# A test whose command names bloom-cli, bloom-mcp, the MCP HTTP test host or the built Python
# module directory gets the fixed memory reading by itself; a test program that starts one of them
# from its own code says FIXED_MEMORY. A stress test never gets it: it is there for the real machine.
# A test marked STRESS, BENCH or COMPOSITOR is not registered unless its option is on; set its
# properties through this function's arguments (or guard with `if(TEST <name>)`), because
# set_tests_properties fails for a test that is not registered.
function(bloom_add_test)
    set(options STRESS BENCH COMPOSITOR RUN_SERIAL FIXED_MEMORY)
    set(one_value_args NAME TIMEOUT WORKING_DIRECTORY SKIP_RETURN_CODE)
    set(multi_value_args COMMAND LABELS ENVIRONMENT)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${one_value_args}" "${multi_value_args}")

    if(NOT ARG_NAME)
        message(FATAL_ERROR "bloom_add_test requires NAME")
    endif()
    if(NOT ARG_COMMAND)
        message(FATAL_ERROR "bloom_add_test(${ARG_NAME}) requires COMMAND")
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()


    # Production targets are the always-on static-analysis boundary. Tests retain strict compiler
    # warnings, formatting, hygiene, and sanitizer coverage; their broader clang-tidy sweep is an
    # explicit opt-in so fixture assertions do not dilute the production signal.
    if(TARGET "${ARG_COMMAND}" AND NOT BLOOM_ENABLE_TEST_CLANG_TIDY)
        set_property(TARGET "${ARG_COMMAND}" PROPERTY CXX_CLANG_TIDY "")
    endif()

    # macOS has no system Vulkan; a test program finds the locked loader and driver beside itself
    # the way a packaged executable does. Nothing happens on other platforms.
    list(GET ARG_COMMAND 0 bloom_test_program)
    bloom_stage_test_vulkan_runtime("${bloom_test_program}")

    # The program still builds when its test is opt-in, so it cannot rot unnoticed.
    if(ARG_STRESS)
        list(APPEND ARG_LABELS stress)
        if(NOT BLOOM_ENABLE_STRESS_TESTS)
            return()
        endif()
    endif()
    if(ARG_BENCH)
        list(APPEND ARG_LABELS bench)
        if(NOT BLOOM_ENABLE_BENCH_TESTS)
            return()
        endif()
    endif()
    if(ARG_COMPOSITOR)
        list(APPEND ARG_LABELS compositor)
        if(NOT BLOOM_ENABLE_COMPOSITOR_TESTS)
            return()
        endif()
    endif()

    if(NOT ARG_STRESS)
        string(FIND "${ARG_COMMAND}" "${CMAKE_BINARY_DIR}/python" bloom_test_python_module)
        if(ARG_FIXED_MEMORY
           OR "${ARG_COMMAND}" MATCHES "bloom-cli|bloom-mcp|bloom_mcp_http_test_host"
           OR NOT bloom_test_python_module EQUAL -1)
            list(APPEND ARG_ENVIRONMENT
                 "BLOOM_TEST_FIXED_MEMORY_READING=${BLOOM_TEST_FIXED_MEMORY_READING}")
        endif()
    endif()

    add_test(NAME "${ARG_NAME}" COMMAND ${ARG_COMMAND})
    set_tests_properties("${ARG_NAME}" PROPERTIES TIMEOUT "${ARG_TIMEOUT}")
    if(ARG_SKIP_RETURN_CODE)
        set_tests_properties("${ARG_NAME}" PROPERTIES SKIP_RETURN_CODE "${ARG_SKIP_RETURN_CODE}")
    endif()
    if(ARG_RUN_SERIAL)
        set_tests_properties("${ARG_NAME}" PROPERTIES RUN_SERIAL TRUE)
    endif()
    if(ARG_LABELS)
        set_tests_properties("${ARG_NAME}" PROPERTIES LABELS "${ARG_LABELS}")
    endif()
    if(ARG_ENVIRONMENT)
        set_tests_properties("${ARG_NAME}" PROPERTIES ENVIRONMENT "${ARG_ENVIRONMENT}")
    endif()
    if(ARG_WORKING_DIRECTORY)
        set_tests_properties(
            "${ARG_NAME}"
            PROPERTIES WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}"
        )
    endif()
endfunction()

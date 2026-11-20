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

# The memory reading every test runs with: 16 GiB of physical memory with 8 GiB available and an
# 8 GiB card, in bytes. Bloom's memory sample and every GPU device read it from
# BLOOM_TEST_FIXED_MEMORY_READING (bloom/core/test_fixed_memory_reading.hpp; test-only), in the test
# program and in every Bloom program it starts (bloom-mcp, bloom-cli, the Python module). So no
# test in the suite plans against the free host memory or the live GPU budget that whatever else
# runs on the machine leaves: a test's admission and device plans are the same on an idle machine,
# under a parallel suite or beside another GPU program (#750). A device the test creates with its own
# card (GpuDeviceCreationOptions::device_memory_limit_bytes) keeps that card.
set(BLOOM_TEST_FIXED_MEMORY_READING "17179869184,8589934592,8589934592"
    CACHE INTERNAL "Fixed memory reading every registered test runs with")

# bloom_add_test(NAME <name> COMMAND <program> [args...]
#                [LABELS ...] [ENVIRONMENT ...] [TIMEOUT <seconds>] [WORKING_DIRECTORY <dir>]
#                [SKIP_RETURN_CODE <code>] [RUN_SERIAL] [STRESS] [BENCH] [COMPOSITOR]
#                [LIVE_MEMORY <reason>])
# Every test gets the fixed memory reading. The one exception is a test that exists to measure the
# real machine (its real card, its real memory over a soak): it says LIVE_MEMORY with the reason,
# which must name what it measures, and it must also be STRESS, so it never runs in the default
# suite. Its reason is kept in its environment (BLOOM_TEST_LIVE_MEMORY), where
# bloom.quality.test_memory_reading reads it: that check fails for any registered test that runs
# without the reading and is not such a named stress exception.
# A test marked STRESS, BENCH or COMPOSITOR is not registered unless its option is on; set its
# properties through this function's arguments (or guard with `if(TEST <name>)`), because
# set_tests_properties fails for a test that is not registered. Never replace a registered test's
# ENVIRONMENT afterwards (it would drop the reading); pass it here, or APPEND to the property.
function(bloom_add_test)
    set(options STRESS BENCH COMPOSITOR RUN_SERIAL)
    set(one_value_args NAME TIMEOUT WORKING_DIRECTORY SKIP_RETURN_CODE LIVE_MEMORY)
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
    if(DEFINED ARG_LIVE_MEMORY OR "LIVE_MEMORY" IN_LIST ARG_KEYWORDS_MISSING_VALUES)
        string(STRIP "${ARG_LIVE_MEMORY}" bloom_test_live_memory_reason)
        if(bloom_test_live_memory_reason STREQUAL "")
            message(FATAL_ERROR "bloom_add_test(${ARG_NAME}): LIVE_MEMORY needs its reason")
        endif()
        if(NOT ARG_STRESS)
            message(FATAL_ERROR
                "bloom_add_test(${ARG_NAME}): a LIVE_MEMORY test reads the real machine's memory, "
                "so it must be STRESS and stay out of the default suite")
        endif()
        string(FIND "${bloom_test_live_memory_reason}" ";" bloom_test_live_memory_separator)
        if(NOT bloom_test_live_memory_separator EQUAL -1)
            message(FATAL_ERROR "bloom_add_test(${ARG_NAME}): the LIVE_MEMORY reason has a ';'")
        endif()
    endif()


    # Production targets are the always-on static-analysis boundary. Tests retain strict compiler
    # warnings, formatting, hygiene, and sanitizer coverage; their broader clang-tidy sweep is an
    # explicit opt-in so fixture assertions do not dilute the production signal.
    if(TARGET "${ARG_COMMAND}" AND NOT BLOOM_ENABLE_TEST_CLANG_TIDY)
        set_property(TARGET "${ARG_COMMAND}" PROPERTY CXX_CLANG_TIDY "")
    endif()

    # Every test program finds the shared test support headers (tests/support), among them the
    # per-process fixture folders of unique_temp_dir.hpp (#749).
    list(GET ARG_COMMAND 0 bloom_test_target)
    if(TARGET "${bloom_test_target}")
        get_target_property(bloom_test_target_imported "${bloom_test_target}" IMPORTED)
        get_target_property(bloom_test_target_type "${bloom_test_target}" TYPE)
        if(NOT bloom_test_target_imported AND bloom_test_target_type STREQUAL "EXECUTABLE")
            target_include_directories("${bloom_test_target}"
                                       PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
            # macOS scans every new program file once, when it first starts, one file at a time
            # (Gatekeeper's online check and XProtect), and a test waits for that. A relink that
            # gives the same bytes keeps the old, already scanned file (#727;
            # cmake/BloomKeepUnchangedLink.cmake). Linux and Windows have no such per-file cost.
            if(APPLE)
                foreach(bloom_link_language IN ITEMS C CXX OBJC OBJCXX)
                    set_property(TARGET "${bloom_test_target}"
                        PROPERTY ${bloom_link_language}_LINKER_LAUNCHER "${CMAKE_COMMAND}" -P
                            "${PROJECT_SOURCE_DIR}/cmake/BloomKeepUnchangedLink.cmake" --)
                endforeach()
            endif()
        endif()
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

    # A test that states its own reading in ENVIRONMENT (the unsampled form, another card) keeps it:
    # the later entry wins.
    if(DEFINED bloom_test_live_memory_reason)
        list(APPEND ARG_ENVIRONMENT "BLOOM_TEST_LIVE_MEMORY=${bloom_test_live_memory_reason}")
    else()
        list(PREPEND ARG_ENVIRONMENT
             "BLOOM_TEST_FIXED_MEMORY_READING=${BLOOM_TEST_FIXED_MEMORY_READING}")
    endif()

    # A crash during the suite is recorded in the build tree, never in the user's crash folder
    # (src/crash/include/bloom/crash/crash_reports.hpp). A test that sets its own folder keeps it:
    # the later entry wins.
    list(PREPEND ARG_ENVIRONMENT "BLOOM_TEST_CRASH_DIRECTORY=${CMAKE_BINARY_DIR}/test-crash-reports")

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

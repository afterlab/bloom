include_guard(GLOBAL)

include(BloomTestEnvironment)
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
# ENVIRONMENT takes one argument per variable, ENVIRONMENT "A=1" "B=2": a quoted "A=1;B=2" is one
# argument, which would set A to "1;B=2" and never set B, so it stops the configuration (#763;
# bloom.quality.test_environment_entries checks the CTest files the same way).
# Whether `target` links Qt, directly or through the libraries it links (their interface and, for a
# static library, its own links). Sets `result` in the caller.
function(_bloom_links_qt target result)
    set(pending "${target}")
    set(seen)
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${current}")
        set(links)
        foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(values "${current}" ${property})
            if(values)
                list(APPEND links ${values})
            endif()
        endforeach()
        foreach(link IN LISTS links)
            string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" link "${link}")
            if(link MATCHES "^Qt6?::")
                set(${result} TRUE PARENT_SCOPE)
                return()
            endif()
            if(TARGET "${link}")
                get_target_property(imported "${link}" IMPORTED)
                if(NOT imported)
                    list(APPEND pending "${link}")
                endif()
            endif()
        endforeach()
    endwhile()
    set(${result} FALSE PARENT_SCOPE)
endfunction()

function(_bloom_isolate_test_settings target)
    _bloom_links_qt("${target}" links_qt)
    if(links_qt)
        target_sources("${target}"
                       PRIVATE "${PROJECT_SOURCE_DIR}/tests/support/isolated_qt_settings_startup.cpp")
    endif()
endfunction()

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
    # #763: each ENVIRONMENT argument names one variable (cmake/BloomTestEnvironment.cmake). A quoted
    # "A=1;B=2" stays one argument here and would set only A, to "1;B=2".
    bloom_joined_test_environment(bloom_test_joined_environment ARG_ENVIRONMENT)
    if(bloom_test_joined_environment)
        list(JOIN bloom_test_joined_environment ", " bloom_test_joined_environment)
        message(FATAL_ERROR
            "bloom_add_test(${ARG_NAME}): one ENVIRONMENT argument names several variables "
            "(${bloom_test_joined_environment}), so the test would get only the first, holding "
            "the rest in its value. Pass each variable as its own argument: "
            "ENVIRONMENT \"A=1\" \"B=2\", not ENVIRONMENT \"A=1;B=2\".")
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
            # #749: a test program that links Qt keeps its default settings in a file of its own
            # (tests/support/isolated_qt_settings_startup.cpp). Decided once the whole build is
            # read, when every target_link_libraries of the program has been seen.
            get_property(bloom_test_settings_deferred TARGET "${bloom_test_target}"
                         PROPERTY BLOOM_TEST_SETTINGS_DEFERRED)
            if(NOT bloom_test_settings_deferred)
                set_property(TARGET "${bloom_test_target}"
                             PROPERTY BLOOM_TEST_SETTINGS_DEFERRED TRUE)
                # A deferred call reads its arguments when it runs, so the name is put in now.
                cmake_language(EVAL CODE
                    "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
                                    CALL _bloom_isolate_test_settings [[${bloom_test_target}]])")
            endif()
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
    # #749: on macOS Bloom's cache and data folders (~/Library/Caches/bloom,
    # ~/Library/Application Support/Afterlab/Bloom) and Qt's standard locations are found from the
    # home folder. The XDG_* and LOCALAPPDATA overrides that keep a test away from the user's own
    # state on Linux and Windows do not reach most of them there, so each test gets a home of its
    # own in the build tree, in HOME and in CFFIXED_USER_HOME (the variable CoreFoundation and
    # Foundation take the home from). A test that sets its own keeps it: the later entry wins.
    # bloom.quality.test_private_home checks that every registered test has it. (The preferences
    # daemon ignores both, which is why a Qt test's settings are a file of its own instead.)
    if(APPLE)
        set(bloom_test_home "${CMAKE_BINARY_DIR}/test-home/${ARG_NAME}")
        file(MAKE_DIRECTORY "${bloom_test_home}")
        list(PREPEND ARG_ENVIRONMENT "HOME=${bloom_test_home}"
             "CFFIXED_USER_HOME=${bloom_test_home}")
    endif()
    # Every signal or termination a process supervisor sends during the suite is logged, with its
    # target and the target's name at that moment, beside CTest's own logs (local-validation.md):
    # a test killed with no line naming it was not killed by a supervisor.
    list(PREPEND ARG_ENVIRONMENT
         "BLOOM_PROCESS_SIGNAL_LOG=${CMAKE_BINARY_DIR}/Testing/Temporary/process-signals.log")

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

include(BloomTesting)

# The crash folder without a crash: minidump streams, the text report, offering once, pruning,
# the GitHub address and the held text report file.
add_executable(bloom_crash_reports_test crash_reports_tests.cpp)
target_include_directories(bloom_crash_reports_test PRIVATE "${PROJECT_SOURCE_DIR}/src/crash")
target_link_libraries(bloom_crash_reports_test PRIVATE bloom_crash)
bloom_enable_warnings(bloom_crash_reports_test)
bloom_add_test(NAME bloom.crash.reports
    COMMAND bloom_crash_reports_test "${CMAKE_CURRENT_BINARY_DIR}/reports-scratch"
    LABELS unit crash)

if(NOT BLOOM_CRASH_HAVE_CRASHPAD)
    return()
endif()

# Deliberate crashes in a child process, each checked by collecting the folder it left, as the
# next start of a Bloom program does (crash_test.cmake). Each test has its own folder.
add_executable(bloom_crash_child crash_child.cpp)
target_link_libraries(bloom_crash_child PRIVATE bloom_crash_program)
bloom_enable_warnings(bloom_crash_child)
bloom_crash_stage_handler(bloom_crash_child)
add_executable(bloom_crash_check crash_check.cpp)
target_link_libraries(bloom_crash_check PRIVATE bloom_crash)
bloom_enable_warnings(bloom_crash_check)

foreach(bloom_crash_mode IN ITEMS null-write worker-thread abort stack-overflow clean)
    bloom_add_test(NAME bloom.crash.child-${bloom_crash_mode}
        COMMAND "${CMAKE_COMMAND}"
            "-DCRASH=$<TARGET_FILE:bloom_crash_child>;${bloom_crash_mode};${CMAKE_CURRENT_BINARY_DIR}/child-${bloom_crash_mode}"
            "-DCHECK=$<TARGET_FILE:bloom_crash_check>"
            -DMODE=${bloom_crash_mode}
            "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/child-${bloom_crash_mode}"
            -DPROGRAM=bloom
            -P "${CMAKE_CURRENT_SOURCE_DIR}/crash_test.cmake"
        LABELS integration crash
        TIMEOUT 180)
endforeach()

# bloom-cli crashing headless: a script null-writes through ctypes inside the carried interpreter.
if(BLOOM_BUILD_PYTHON AND TARGET bloom-cli)
    bloom_add_test(NAME bloom.crash.cli-headless
        COMMAND "${CMAKE_COMMAND}"
            "-DCRASH=$<TARGET_FILE:bloom-cli>;run;${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/crash_null_write.py"
            "-DCHECK=$<TARGET_FILE:bloom_crash_check>"
            -DMODE=null-write
            "-DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/cli-headless"
            -DPROGRAM=bloom-cli
            -P "${CMAKE_CURRENT_SOURCE_DIR}/crash_test.cmake"
        LABELS integration crash scripting
        TIMEOUT 180)
endif()

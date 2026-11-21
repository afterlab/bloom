# A project for test_environment_registration.cmake: registers tests through the real
# bloom_add_test, with each variable as its own argument (FORM=split) or with the quoted, joined
# argument every scale variant used before #763 (FORM=joined), which must stop the configuration.
cmake_minimum_required(VERSION 3.25)
project(bloom_test_environment_registration NONE)
list(APPEND CMAKE_MODULE_PATH "${MODULE_DIR}")
include(BloomTesting)
enable_testing()
set(check "${CMAKE_CURRENT_LIST_DIR}/check_environment.cmake")
if(FORM STREQUAL "joined")
    bloom_add_test(NAME registration.view.dpr2
        COMMAND "${CMAKE_COMMAND}" -P "${check}"
        ENVIRONMENT "BLOOM_ENV_A=1;QT_SCALE_FACTOR=2;BLOOM_ENV_B=2")
elseif(FORM STREQUAL "split")
    bloom_add_test(NAME registration.view.dpr2
        COMMAND "${CMAKE_COMMAND}" -P "${check}"
        ENVIRONMENT "BLOOM_ENV_A=1" "QT_SCALE_FACTOR=2" "BLOOM_ENV_B=2" "BLOOM_ENV_LIST=one;two")
    set(entries "BLOOM_ENV_A=1" "QT_SCALE_FACTOR=2" "BLOOM_ENV_B=2" "BLOOM_ENV_LIST=one\;two")
    bloom_add_test(NAME registration.list
        COMMAND "${CMAKE_COMMAND}" -P "${check}"
        ENVIRONMENT ${entries})
else()
    message(FATAL_ERROR "FORM must be split or joined")
endif()

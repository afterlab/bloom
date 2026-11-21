# bloom.build.test-environment-registration: bloom_add_test hands every ENVIRONMENT variable to
# the test as its own variable, and the quoted, joined form scale variants used before #763 stops
# the configuration (cmake/BloomTestEnvironment.cmake).
#
#   cmake -DMODULE_DIR=<cmake/> -DWORK=<empty folder in the build tree> -DGENERATOR=<generator>
#         [-DMAKE_PROGRAM=<program>] -P <this file>
#
# It configures cmake/tests/test_environment_registration twice through the real bloom_add_test:
# the joined form must fail with the message that names the folded variables; the split form must
# configure, pass bloom.quality.test_environment_entries' check, and run under ctest with each
# variable (and a value that is a list) arriving as the registration named it.
cmake_minimum_required(VERSION 3.25)
if(NOT MODULE_DIR OR NOT WORK OR NOT GENERATOR)
    message(FATAL_ERROR "test_environment_registration.cmake needs MODULE_DIR, WORK and GENERATOR")
endif()
set(project "${CMAKE_CURRENT_LIST_DIR}/test_environment_registration")
set(generator_arguments -G "${GENERATOR}")
if(MAKE_PROGRAM)
    list(APPEND generator_arguments "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
set(failures "")

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" ${generator_arguments} -S "${project}" -B "${WORK}/joined"
            "-DMODULE_DIR=${MODULE_DIR}" -DFORM=joined
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
# CMake wraps a message's lines; read it as one line.
string(REGEX REPLACE "[ \t\r\n]+" " " error "${error}")
if(result EQUAL 0)
    string(APPEND failures "\nthe joined ENVIRONMENT argument was accepted")
elseif(NOT error MATCHES "one ENVIRONMENT argument names several variables"
       OR NOT error MATCHES "BLOOM_ENV_A \\+ QT_SCALE_FACTOR \\+ BLOOM_ENV_B")
    string(APPEND failures "\nthe joined form failed without naming its variables:\n${error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${generator_arguments} -S "${project}" -B "${WORK}/split"
            "-DMODULE_DIR=${MODULE_DIR}" -DFORM=split
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    string(APPEND failures "\nthe split form did not configure:\n${output}${error}")
else()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DTREE=${WORK}/split" -DCONFIG=Release
                -P "${CMAKE_CURRENT_LIST_DIR}/test_environment_entries.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        string(APPEND failures "\nthe entries check refused the split form:\n${output}${error}")
    endif()
    execute_process(
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${WORK}/split" -C Release
                --output-on-failure
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        string(APPEND failures "\nthe registered tests did not get their environment:\n${output}${error}")
    endif()
endif()

if(failures)
    message(FATAL_ERROR "test environment registration check failed:${failures}")
endif()
message(STATUS "the joined form stops the configuration; the split form reaches the test")

# The application must not start when the package check's profile variable is set to something
# that is not an absolute directory (src/ui/include/bloom/ui/application_identity.hpp): it exits
# with status 2 and says why, instead of running under Bloom's own names and settings.
#
#   cmake -DPROGRAM=<the Bloom application> -DWORK=<empty work directory> -P <this file>
#
# The home and settings directories of the run are the work directory's, so that even a program
# that wrongly started would not find the settings of the person whose computer it is.
cmake_minimum_required(VERSION 3.25)
if(NOT PROGRAM OR NOT WORK)
    message(FATAL_ERROR "application_refuses_relative_profile.cmake needs PROGRAM and WORK")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/home")
set(ENV{BLOOM_PACKAGE_CHECK_PROFILE} "relative/profile")
set(ENV{QT_QPA_PLATFORM} "offscreen")
set(ENV{HOME} "${WORK}/home")
set(ENV{XDG_CONFIG_HOME} "${WORK}/home/config")
set(ENV{XDG_DATA_HOME} "${WORK}/home/data")
set(ENV{XDG_CACHE_HOME} "${WORK}/home/cache")
set(ENV{XDG_STATE_HOME} "${WORK}/home/state")
file(TO_NATIVE_PATH "${WORK}/home" native_home)
set(ENV{APPDATA} "${native_home}")
set(ENV{LOCALAPPDATA} "${native_home}")
execute_process(COMMAND "${PROGRAM}" WORKING_DIRECTORY "${WORK}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
if(NOT result EQUAL 2)
    message(FATAL_ERROR "Bloom did not refuse a relative profile: result '${result}'\n"
                        "${output}${error}")
endif()
if(NOT error MATCHES "BLOOM_PACKAGE_CHECK_PROFILE.*not an absolute directory")
    message(FATAL_ERROR "Bloom refused without saying why:\n${output}${error}")
endif()
message(STATUS "Bloom refuses a relative profile: ${error}")

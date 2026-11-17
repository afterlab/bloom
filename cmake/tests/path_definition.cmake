# A path embedded in a compile definition carries no backslash (cmake/BloomPathDefinition.cmake).
# A hosted Windows runner passes the dependency prefix as D:\a\bloom\bloom\..., which a C++ string
# literal reads as escape sequences (`\a`, `\b` become control characters, `\d` is an error).
# The check runs the same way on every system, with the Windows spelling as its input.
#
#   cmake -DMODULE_DIR=<cmake/> -P <this file>
cmake_minimum_required(VERSION 3.25)
if(NOT MODULE_DIR)
    message(FATAL_ERROR "path_definition.cmake needs MODULE_DIR")
endif()
include("${MODULE_DIR}/BloomPathDefinition.cmake")

set(failures "")
function(expect what actual wanted)
    if(NOT actual STREQUAL wanted)
        set(failures "${failures}\n${what}: got '${actual}', wanted '${wanted}'" PARENT_SCOPE)
    endif()
endfunction()

# The failing input of the release rehearsal, and the spellings around it.
bloom_cmake_path(a "D:\\a\\bloom\\x")
expect("hosted runner path" "${a}" "D:/a/bloom/x")
bloom_cmake_path(b "D:\\a\\bloom\\bloom\\build\\dependency-prefix/python/DLLs")
expect("mixed path" "${b}" "D:/a/bloom/bloom/build/dependency-prefix/python/DLLs")
bloom_cmake_path(c "/home/user/prefix/python/DLLs")
expect("forward slashes stay" "${c}" "/home/user/prefix/python/DLLs")
bloom_cmake_path(d "\\\\server\\share\\prefix")
expect("network path" "${d}" "//server/share/prefix")

bloom_path_definition(def "BLOOM_X_DIRECTORY" "D:\\a\\bloom\\x/python/DLLs")
expect("definition" "${def}" "BLOOM_X_DIRECTORY=\"D:/a/bloom/x/python/DLLs\"")
if(def MATCHES "\\\\")
    set(failures "${failures}\ndefinition '${def}' holds a backslash")
endif()

# A path a string literal cannot carry stops the configuration.
foreach(bad IN ITEMS "C:/a\"b" "C:/a;b")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DMODULE_DIR=${MODULE_DIR}" "-DBAD=${bad}"
                -P "${CMAKE_CURRENT_LIST_DIR}/path_definition_refusal.cmake"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(result EQUAL 0)
        set(failures "${failures}\na path with '${bad}' was accepted")
    endif()
endforeach()

if(failures)
    message(FATAL_ERROR "path definition check failed:${failures}")
endif()

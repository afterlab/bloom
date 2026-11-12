# Puts one of Bloom's command-line programs inside Bloom.app, beside the application.
#
#   cmake -DTOOL=<built program> -DBUNDLE=<Bloom.app> -P bundle_tool.cmake
#
# The built program looks for the libraries it links (the interpreter) in the dependency prefix,
# which a package does not have. The copy in Contents/MacOS looks in the bundle's Frameworks
# directory and nowhere else, as the application does, and is signed again ad hoc because its
# bytes changed. Everything else it needs it finds from its own place, like the application: the
# interpreter's home, the workers, the shader tools and the Vulkan loader.
#
# The copy is rewritten only when the built program changed, so an unchanged bundle keeps its
# bytes and its signature.
cmake_minimum_required(VERSION 3.25)

if(NOT TOOL OR NOT EXISTS "${TOOL}" OR NOT BUNDLE OR NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "bundle_tool.cmake needs TOOL and BUNDLE: '${TOOL}', '${BUNDLE}'")
endif()
get_filename_component(name "${TOOL}" NAME)
set(destination "${BUNDLE}/Contents/MacOS/${name}")
set(stamp "${BUNDLE}/../${name}.bundled")
file(SHA256 "${TOOL}" digest)
if(EXISTS "${destination}" AND EXISTS "${stamp}")
    file(READ "${stamp}" recorded)
    if(recorded STREQUAL digest)
        return()
    endif()
endif()

file(COPY_FILE "${TOOL}" "${destination}")
execute_process(COMMAND otool -l "${destination}" OUTPUT_VARIABLE output
                COMMAND_ERROR_IS_FATAL ANY)
string(REGEX MATCHALL "cmd LC_RPATH\n[^\n]*\n +path [^\n]+ \\(offset [0-9]+\\)" entries
       "${output}")
set(wanted "@executable_path/../Frameworks")
set(present FALSE)
foreach(entry IN LISTS entries)
    string(REGEX REPLACE "^.*\n +path (.+) \\(offset [0-9]+\\)$" "\\1" path "${entry}")
    if(path STREQUAL wanted)
        set(present TRUE)
    else()
        execute_process(COMMAND install_name_tool -delete_rpath "${path}" "${destination}"
                        ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)
    endif()
endforeach()
if(NOT present)
    execute_process(COMMAND install_name_tool -add_rpath "${wanted}" "${destination}"
                    ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)
endif()
execute_process(COMMAND codesign --force --sign - "${destination}"
                OUTPUT_QUIET ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "codesign failed for ${destination}: ${error}")
endif()
file(WRITE "${stamp}" "${digest}")
message(STATUS "Bloom.app carries ${name}")

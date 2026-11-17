# A path that becomes part of a C++ string literal on a compiler command line.
#
# A compile definition such as BLOOM_UPDATE_CRYPTO_BUILD_DIRECTORY="<path>" is source text: a
# backslash in the path starts an escape sequence. A hosted Windows runner hands the dependency
# prefix to CMake with backslashes (D:\a\bloom\bloom\...), where `\a` and `\b` silently became
# control characters and `\d` is a compile error. CMake spells its own paths with forward slashes
# on every system, and every system reads them, so a path that comes from outside is turned into
# that spelling before it is used in a definition.
#
#   bloom_cmake_path(<out> <path>)           <path> with every backslash a forward slash
#   bloom_path_definition(<out> <name> <path>)
#       <out> is the definition <name>="<path>" for target_compile_definitions or
#       COMPILE_DEFINITIONS, with <path> in that spelling; a path that still holds a character a
#       string literal cannot carry as it stands (a double quote, a semicolon, a line break)
#       stops the configuration with a message instead of reaching the compiler.
#
# Test: cmake/tests/path_definition.cmake.

include_guard(GLOBAL)

function(bloom_cmake_path output path)
    string(REPLACE "\\" "/" path "${path}")
    set(${output} "${path}" PARENT_SCOPE)
endfunction()

function(bloom_path_definition output name path)
    bloom_cmake_path(path "${path}")
    if(path MATCHES "[\"\n\r;]")
        message(FATAL_ERROR
            "${name}: the path '${path}' holds a double quote, a semicolon or a line break, "
            "which cannot be carried in a C++ string literal on a compiler command line.")
    endif()
    set(${output} "${name}=\"${path}\"" PARENT_SCOPE)
endfunction()

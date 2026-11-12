# Gives a deployed Bloom.app its ad hoc signature.
#
#   cmake -DBUNDLE=<path to Bloom.app> -P sign_bundle.cmake
#
# Only what carries no valid signature is signed here. Then the bundle itself is signed, which
# seals everything inside it.
#
#   * The locked Qt's frameworks and plugins (cmake/package/stage_qt.cmake) carry the Qt
#     Project's own signatures and are left byte for byte as they are. In a developer build,
#     where macdeployqt copies the host's Qt and rewrites it, the rewritten files no longer
#     verify and are signed again.
#   * The programs Bloom packages (the shader tools, the workers, the Vulkan loader and driver,
#     the interpreter and its modules) already carry the linker's ad hoc signature and are left
#     as they are: Bloom checks the shader tools against the digests in their inventory before
#     it runs them, and signing them again changes those bytes, so a bundle signed with --deep
#     refuses its own tools and renders everything on the CPU.

if(NOT BUNDLE OR NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "sign_bundle.cmake: BUNDLE is not a directory: '${BUNDLE}'")
endif()

function(bloom_sign path)
    execute_process(COMMAND codesign --force --sign - "${path}"
                    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "codesign failed for ${path}: ${error}")
    endif()
endfunction()

# Signs the item unless it already carries a signature that verifies.
function(bloom_sign_unsigned path)
    execute_process(COMMAND codesign --verify "${path}"
                    RESULT_VARIABLE unsigned OUTPUT_QUIET ERROR_QUIET)
    if(NOT unsigned EQUAL 0)
        bloom_sign("${path}")
    endif()
endfunction()

file(GLOB_RECURSE plugins "${BUNDLE}/Contents/PlugIns/*.dylib")
file(GLOB frameworks LIST_DIRECTORIES TRUE "${BUNDLE}/Contents/Frameworks/*.framework")
foreach(item IN LISTS plugins frameworks)
    bloom_sign_unsigned("${item}")
endforeach()

# The libraries Bloom put in Contents/Frameworks (the Vulkan loader and driver, the Python
# interpreter) and the interpreter's extension modules carry the linker's signature. One that
# no longer verifies is signed again; the others are left as they are.
file(GLOB libraries "${BUNDLE}/Contents/Frameworks/*.dylib")
file(GLOB_RECURSE modules "${BUNDLE}/Contents/Resources/python/*.so"
                          "${BUNDLE}/Contents/Resources/python/*.dylib")
foreach(item IN LISTS libraries modules)
    if(IS_SYMLINK "${item}")
        continue()
    endif()
    execute_process(COMMAND codesign --verify "${item}"
                    RESULT_VARIABLE unsigned OUTPUT_QUIET ERROR_QUIET)
    if(NOT unsigned EQUAL 0)
        bloom_sign("${item}")
    endif()
endforeach()

# Everything under Contents/MacOS counts as code to the bundle's seal, including the shader tools'
# inventory, lock and license texts. A file there that carries no signature yet gets one; for a
# file that is not a program the signature is stored beside the file's contents, not in them, so
# its bytes and digest stay as they are. The programs already verify and are skipped.
# The bundle's own executable is signed with the bundle, below.
execute_process(COMMAND /usr/libexec/PlistBuddy -c "Print CFBundleExecutable"
                        "${BUNDLE}/Contents/Info.plist"
                OUTPUT_VARIABLE bundle_name OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT bundle_name)
    message(FATAL_ERROR "sign_bundle.cmake: ${BUNDLE} names no executable in its Info.plist")
endif()
file(GLOB_RECURSE beside_executable "${BUNDLE}/Contents/MacOS/*")
list(REMOVE_ITEM beside_executable "${BUNDLE}/Contents/MacOS/${bundle_name}")
foreach(item IN LISTS beside_executable)
    execute_process(COMMAND codesign --verify "${item}"
                    RESULT_VARIABLE unsigned OUTPUT_QUIET ERROR_QUIET)
    if(NOT unsigned EQUAL 0)
        bloom_sign("${item}")
    endif()
endforeach()
bloom_sign("${BUNDLE}")

execute_process(COMMAND codesign --verify --deep --strict "${BUNDLE}"
                RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The signed bundle does not verify: ${error}")
endif()

# The shader tools must still be the files their inventory describes; Bloom refuses them otherwise
# and renders on the CPU without saying why in the window.
set(tools "${BUNDLE}/Contents/MacOS/bloom-gpu-tools")
if(EXISTS "${tools}/inventory.json")
    file(READ "${tools}/inventory.json" inventory)
    string(JSON count LENGTH "${inventory}" tools)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON name GET "${inventory}" tools ${index} file)
        string(JSON expected GET "${inventory}" tools ${index} stagedSha256)
        file(SHA256 "${tools}/${name}" actual)
        if(NOT "sha256:${actual}" STREQUAL expected)
            message(FATAL_ERROR "Signing changed the shader tool ${name}: its digest is sha256:${actual}, "
                                "the inventory records ${expected}")
        endif()
    endforeach()
endif()
message(STATUS "Bloom.app signed ad hoc and verified")

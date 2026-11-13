# The package check (check_package.cmake) on a real package: it passes, with a run of the
# packaged programs, and it refuses the package once it is broken in each way that matters.
#
#   cmake (-DBUILD=<build tree> | -DBUNDLE=<deployed Bloom.app>) -DWORK=<work directory>
#         [-DPATCHELF=<patchelf>] [-DGPU=ON] -P check_package_test.cmake
#
# With BUILD the package is installed from the build tree into the work directory; with BUNDLE
# (macOS) the deployed bundle is copied there. The copy is this test's own: it is broken and
# mended in place, and the build's own files are never touched.
#
# The cases the check must refuse, each with the text it must give:
#
#   the interpreter removed          "no interpreter library"
#   a license file removed           "no license file"
#   a license file changed           "is not the file the dependency lock records"
#   a library pointed outside        a program made to load a library that is not in the
#                                    package ("loads ...")
#   an outside search path           a program given a search path outside the package
#                                    (Linux and macOS; a Windows program has none)
#   a carried distribution library   (Linux) one of the libraries carried beside Qt removed
#   removed                          ("no libxcb-cursor.so.0 ... beside Qt")
#
# With GPU, the packaged application must also show a Viewer frame evaluated on the GPU (rule
# `gpu`), and on Linux the same run with no GPU driver offered must be refused ("was evaluated
# on the CPU, not the GPU").
cmake_minimum_required(VERSION 3.25)

if(NOT WORK OR (NOT BUILD AND NOT BUNDLE))
    message(FATAL_ERROR "check_package_test.cmake needs WORK and BUILD or BUNDLE")
endif()
set(check "${CMAKE_CURRENT_LIST_DIR}/check_package.cmake")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

if(BUNDLE)
    # cp -R keeps the links inside the frameworks as links.
    execute_process(COMMAND cp -R "${BUNDLE}" "${WORK}/" COMMAND_ERROR_IS_FATAL ANY)
    get_filename_component(name "${BUNDLE}" NAME)
    set(package "${WORK}/${name}")
else()
    set(package "${WORK}/package")
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${package}"
                    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the package could not be installed:\n${output}${error}")
    endif()
endif()

if(CMAKE_HOST_APPLE)
    set(programs "${package}/Contents/MacOS")
    set(libraries "${package}/Contents/Frameworks")
    set(records "${package}/Contents/Resources")
    set(worker "${programs}/bloom-color-worker")
    file(GLOB interpreter "${libraries}/libpython3.*.dylib")
elseif(CMAKE_HOST_WIN32)
    set(programs "${package}")
    set(libraries "${package}")
    set(records "${package}")
    set(worker "${programs}/bloom-color-worker.exe")
    file(GLOB interpreter "${libraries}/python3??.dll")
else()
    set(programs "${package}/bin")
    set(libraries "${package}/lib")
    set(records "${package}/share/bloom")
    set(worker "${package}/libexec/bloom/bloom-color-worker")
    file(GLOB interpreter "${libraries}/libpython3.*.so.1.0")
endif()

# 1. The package as built passes, with a run.
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DCOMPLETE=ON -DRUN=ON
        "-DWORK=${WORK}/run" -P "${check}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the package does not pass its check:\n${output}${error}")
endif()
string(STRIP "${output}${error}" passed)
message(STATUS "${passed}")

# 1b. With GPU (a build with the Vulkan backend): the packaged application shows a Viewer frame
# that was evaluated on the GPU. A package whose application finds no device, or whose Viewer
# falls back to the CPU reference path, fails here (#717).
if(GPU)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DGPU=ON -DONLY=gpu
            "-DWORK=${WORK}/gpu" -P "${check}"
        OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the packaged application does not render on the GPU:\n"
                            "${output}${error}")
    endif()
    string(STRIP "${output}${error}" passed)
    message(STATUS "${passed}")
    if(NOT CMAKE_HOST_APPLE AND NOT CMAKE_HOST_WIN32)
        # The same run with no GPU driver offered to the loader is a genuine CPU fallback, and
        # the rule must refuse it and say so. (Linux only: the loader reads these variables
        # everywhere, but the macOS package names its own driver and Windows ignores them in an
        # elevated process.)
        file(MAKE_DIRECTORY "${WORK}/no-drivers")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "VK_DRIVER_FILES=${WORK}/no-drivers"
                "VK_ICD_FILENAMES=${WORK}/no-drivers" "VK_LOADER_LAYERS_DISABLE=~all~"
                "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DGPU=ON -DONLY=gpu
                "-DWORK=${WORK}/gpu-hidden" -P "${check}"
            OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
        if(result EQUAL 0)
            message(FATAL_ERROR "with no GPU driver offered, the GPU rule passed:\n"
                                "${output}${error}")
        elseif(NOT "${output}${error}" MATCHES "was evaluated on the CPU, not the GPU")
            message(FATAL_ERROR "with no GPU driver offered, the GPU rule failed without saying "
                                "that the frame was evaluated on the CPU:\n${output}${error}")
        endif()
        message(STATUS "refused, a Viewer frame from the CPU reference path")
    endif()
endif()

# The check, without a run, must fail with `expected` in what it says.
set(problems "")
function(bloom_expect_refusal what expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DCOMPLETE=ON
            "-DONLY=contents;resolution" -P "${check}"
        OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
    if(result EQUAL 0)
        list(APPEND problems "${what}: the check passed")
    elseif(NOT "${output}${error}" MATCHES "${expected}")
        list(APPEND problems "${what}: the check failed without saying '${expected}':\n"
                             "${output}${error}")
    else()
        message(STATUS "refused, ${what}")
    endif()
    set(problems "${problems}" PARENT_SCOPE)
endfunction()

# 2. The interpreter removed.
list(LENGTH interpreter count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "the package has no single interpreter library: ${interpreter}")
endif()
file(RENAME "${interpreter}" "${WORK}/interpreter-set-aside")
bloom_expect_refusal("the interpreter removed" "no interpreter library")
file(RENAME "${WORK}/interpreter-set-aside" "${interpreter}")

# 3. A license file removed, then changed.
set(license "${records}/licenses/zlib/LICENSE")
if(NOT EXISTS "${license}")
    message(FATAL_ERROR "the package has no ${license}")
endif()
file(RENAME "${license}" "${WORK}/license-set-aside")
bloom_expect_refusal("a license file removed" "no license file licenses/zlib/LICENSE")
file(WRITE "${license}" "not the license\n")
bloom_expect_refusal("a license file changed"
                     "licenses/zlib/LICENSE is not the file the dependency lock records")
file(REMOVE "${license}")
file(RENAME "${WORK}/license-set-aside" "${license}")

# 4. A library pointed outside, and 5. an outside search path: a copy of a small program of the
# package, changed and put where the check reads it.
if(NOT EXISTS "${worker}")
    message(FATAL_ERROR "the package has no ${worker}")
endif()
if(CMAKE_HOST_WIN32)
    # A Windows program finds its libraries beside itself: alone in a directory of its own it
    # has none of the package's.
    file(COPY "${worker}" DESTINATION "${package}/check-probe")
    bloom_expect_refusal("a library pointed outside" "check-probe/bloom-color-worker.exe: loads ")
    file(REMOVE_RECURSE "${package}/check-probe")
else()
    set(probe "${programs}/bloom-check-probe")
    if(CMAKE_HOST_APPLE)
        file(COPY_FILE "${worker}" "${probe}")
        execute_process(COMMAND install_name_tool -change /usr/lib/libSystem.B.dylib
                                /opt/outside/libnotbloom.dylib "${probe}"
                        ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)
        bloom_expect_refusal("a library pointed outside"
                             "bloom-check-probe: loads /opt/outside/libnotbloom.dylib")
        file(REMOVE "${probe}")
        file(COPY_FILE "${worker}" "${probe}")
        execute_process(COMMAND install_name_tool -add_rpath /opt/outside "${probe}"
                        ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)
    else()
        if(NOT PATCHELF OR NOT EXISTS "${PATCHELF}")
            message(FATAL_ERROR "check_package_test.cmake needs PATCHELF on Linux")
        endif()
        file(COPY_FILE "${worker}" "${probe}")
        execute_process(COMMAND "${PATCHELF}" --add-needed libnotbloom.so.1 "${probe}"
                        COMMAND_ERROR_IS_FATAL ANY)
        bloom_expect_refusal("a library pointed outside"
                             "bloom-check-probe: loads libnotbloom.so.1")
        file(REMOVE "${probe}")
        file(COPY_FILE "${worker}" "${probe}")
        execute_process(COMMAND "${PATCHELF}" --set-rpath /opt/outside "${probe}"
                        COMMAND_ERROR_IS_FATAL ANY)
    endif()
    bloom_expect_refusal("an outside search path"
                         "bloom-check-probe: library search path outside the package: /opt/outside")
    file(REMOVE "${probe}")
endif()

# 6. Linux: one of the distribution libraries the package carries beside Qt removed.
if(NOT CMAKE_HOST_APPLE AND NOT CMAKE_HOST_WIN32)
    set(carried "${package}/lib/qt/lib/libxcb-cursor.so.0")
    if(NOT EXISTS "${carried}")
        message(FATAL_ERROR "the package has no ${carried}")
    endif()
    file(RENAME "${carried}" "${WORK}/library-set-aside")
    bloom_expect_refusal("a carried distribution library removed"
                         "no libxcb-cursor.so.0 \\(libxcb-cursor0\\) beside Qt")
    file(RENAME "${WORK}/library-set-aside" "${carried}")
endif()

# The mended package passes again: nothing above was left behind.
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DCOMPLETE=ON "-DONLY=contents;resolution"
        -P "${check}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    list(APPEND problems "the mended package no longer passes:\n${output}${error}")
endif()

if(problems)
    list(JOIN problems "\n  " text)
    message(FATAL_ERROR "the package check did not behave:\n  ${text}")
endif()
message(STATUS "package check: the package passes and every broken form of it is refused")

# The package check (check_package.cmake) on a real package: it passes, with a run of the
# packaged programs, and it refuses the package once it is broken in each way that matters.
#
#   cmake (-DBUILD=<build tree> | -DBUNDLE=<deployed Bloom.app>) -DWORK=<work directory>
#         [-DPATCHELF=<patchelf>] [-DGPU=ON] [-DVERSION=<the package's version label>]
#         -P check_package_test.cmake
#
# VERSION is the label the package's programs must report; with BUILD it is read from the build
# tree's BLOOM_PACKAGE_VERSION when not given, and with BUNDLE it is required.
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
#   the file type description        (Linux) share/mime/packages/id.afterlab.bloom.xml removed
#   removed                          ("no file type description")
#   the update helper removed        "no update helper"
#   the update helper made to load   the helper replaced by a program that loads the package's
#   the package's libraries          libraries ("the update helper loads ..." or "has a library
#                                    search path"): it runs from a copy outside the package
#   one icon size removed            (Linux) the 64 pixel icon removed, or replaced by a 16 pixel
#   or of the wrong size             one ("no 64 pixel icon", "is 16 x 16 pixels, not 64 x 64")
#   Bloom.icns without sizes         (macOS) Bloom.icns cut short ("has no 1024 pixel image")
#   the asset catalog removed        (macOS) Assets.car removed ("no asset catalog")
#   another version                  the package checked against a label other than the one its
#                                    programs report (each program: "reports 'name label'")
#
# With GPU, the packaged application must also show a Viewer frame evaluated on the GPU (rule
# `gpu`), and on Linux the same run with no GPU driver offered must be refused ("was evaluated
# on the CPU, not the GPU").
cmake_minimum_required(VERSION 3.25)

if(NOT WORK OR (NOT BUILD AND NOT BUNDLE))
    message(FATAL_ERROR "check_package_test.cmake needs WORK and BUILD or BUNDLE")
endif()
if(NOT VERSION AND BUILD AND EXISTS "${BUILD}/CMakeCache.txt")
    file(STRINGS "${BUILD}/CMakeCache.txt" version_line
         REGEX "^BLOOM_PACKAGE_VERSION:[A-Z]+=")
    if(version_line MATCHES "=(.+)$")
        set(VERSION "${CMAKE_MATCH_1}")
    endif()
endif()
if(NOT VERSION)
    message(FATAL_ERROR "check_package_test.cmake needs VERSION, the label the programs report")
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
        "-DVERSION=${VERSION}" "-DWORK=${WORK}/run" -P "${check}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the package does not pass its check:\n${output}${error}")
endif()
string(STRIP "${output}${error}" passed)
message(STATUS "${passed}")

# 1a. A link to bloom-cli in another folder, as the Linux install script and the macOS
# installers make one: started through it, the program finds its package and renders a frame
# headless. (Windows has no such link: its PATH entry is the launcher in the cli folder, which
# the installer's test starts the same way.)
if(NOT CMAKE_HOST_WIN32)
    file(MAKE_DIRECTORY "${WORK}/link")
    file(CREATE_LINK "${programs}/bloom-cli" "${WORK}/link/bloom-cli" SYMBOLIC)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DRUN=ON -DONLY=run
            "-DVERSION=${VERSION}" "-DWORK=${WORK}/link-run"
            "-DCLI_PROGRAM=${WORK}/link/bloom-cli" -P "${check}"
        OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "bloom-cli started through a link outside the package does not "
                            "run or render:\n${output}${error}")
    endif()
    message(STATUS "a linked bloom-cli runs and renders a frame through the package")
endif()

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

# 5a. The update helper removed, then replaced by a program that needs the package's libraries.
get_filename_component(worker_directory "${worker}" DIRECTORY)
get_filename_component(worker_extension "${worker}" LAST_EXT)
set(helper "${worker_directory}/bloom-update-helper${worker_extension}")
if(NOT EXISTS "${helper}")
    message(FATAL_ERROR "the package has no ${helper}")
endif()
file(RENAME "${helper}" "${WORK}/helper-set-aside")
bloom_expect_refusal("the update helper removed" "the package has no update helper")
file(COPY_FILE "${programs}/bloom-cli${worker_extension}" "${helper}")
bloom_expect_refusal("the update helper made to load the package's libraries"
                     "the update helper (loads|has a library search path)")
file(REMOVE "${helper}")
file(RENAME "${WORK}/helper-set-aside" "${helper}")

# 5a2. Linux: the file type description removed.
if(NOT CMAKE_HOST_APPLE AND NOT CMAKE_HOST_WIN32)
    set(mime_file "${package}/share/mime/packages/id.afterlab.bloom.xml")
    if(NOT EXISTS "${mime_file}")
        message(FATAL_ERROR "the package has no ${mime_file}")
    endif()
    file(RENAME "${mime_file}" "${WORK}/mime-set-aside")
    bloom_expect_refusal("the file type description removed" "no file type description")
    file(RENAME "${WORK}/mime-set-aside" "${mime_file}")
endif()

# 5b. Another version: the programs report the label they were built under, so a package checked
# against any other label is refused, once for each program. The right label passes the rule.
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" "-DVERSION=${VERSION}" -DONLY=version
        -P "${check}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    list(APPEND problems "the version rule refused the package's own version:\n${output}${error}")
endif()
set(other_version "9.9.9-other.1")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" "-DVERSION=${other_version}" -DONLY=version
        -P "${check}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(result EQUAL 0)
    list(APPEND problems "another version: the check passed")
else()
    foreach(program_name IN ITEMS Bloom bloom-cli bloom-mcp)
        if(NOT "${output}${error}" MATCHES
           "${program_name} reports '${program_name} ${VERSION}', not '${program_name} 9.9.9-other.1'")
            list(APPEND problems "another version: the check failed without saying what "
                                 "${program_name} reports:\n${output}${error}")
        endif()
    endforeach()
    message(STATUS "refused, another version")
endif()

# 5c. The release rule: a package from a build that was not set up with a release's feed address
# and public key (or that is a test build) is not one a release may publish. The package of an
# ordinary build is such a package; a release's own package is checked by the release workflow.
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DONLY=release
        -DFEED_URL=https://updates.example.org/alpha.json
        -DPUBLIC_KEYS=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
        -P "${check}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(result EQUAL 0)
    list(APPEND problems "the release rule passed a package that is not a release's")
elseif(NOT "${output}${error}" MATCHES
       "not set up for updates|is a test build|another address than the release's feed")
    list(APPEND problems "the release rule failed without saying why the package is not a "
                         "release's:\n${output}${error}")
else()
    message(STATUS "refused, not a release's package")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DONLY=release -P "${check}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "needs the release's settings")
    list(APPEND problems "the release rule without the release's settings was not refused:\n"
                         "${output}${error}")
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

# 7. The icon rule: the check with only that rule must fail with `expected` in what it says.
function(bloom_expect_icon_refusal what expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${package}" -DCOMPLETE=ON -DONLY=icons -P "${check}"
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
if(CMAKE_HOST_APPLE)
    set(icns_file "${package}/Contents/Resources/Bloom.icns")
    file(RENAME "${icns_file}" "${WORK}/icns-set-aside")
    # Bloom.icns cut after its first image: sizes missing.
    execute_process(COMMAND head -c 4096 "${WORK}/icns-set-aside" OUTPUT_FILE "${icns_file}"
                    COMMAND_ERROR_IS_FATAL ANY)
    bloom_expect_icon_refusal("Bloom.icns without sizes" "Bloom.icns has no 1024 pixel image")
    file(REMOVE "${icns_file}")
    file(RENAME "${WORK}/icns-set-aside" "${icns_file}")
    set(catalog_file "${package}/Contents/Resources/Assets.car")
    if(EXISTS "${catalog_file}")
        file(RENAME "${catalog_file}" "${WORK}/catalog-set-aside")
        bloom_expect_icon_refusal("the asset catalog removed" "no asset catalog \\(Assets.car\\)")
        file(RENAME "${WORK}/catalog-set-aside" "${catalog_file}")
    endif()
elseif(NOT CMAKE_HOST_WIN32)
    set(icon_file "${package}/share/icons/hicolor/64x64/apps/id.afterlab.bloom.png")
    file(RENAME "${icon_file}" "${WORK}/icon-set-aside")
    bloom_expect_icon_refusal("one icon size removed" "no 64 pixel icon")
    # A 16 pixel icon in the 64 pixel place: the size is read from the file.
    file(COPY_FILE "${package}/share/icons/hicolor/16x16/apps/id.afterlab.bloom.png" "${icon_file}")
    bloom_expect_icon_refusal("an icon of the wrong size" "is 16 x 16 pixels, not 64 x 64")
    file(REMOVE "${icon_file}")
    file(RENAME "${WORK}/icon-set-aside" "${icon_file}")
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

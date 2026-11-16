# Checks the Windows installer (packaging/windows/bloom.iss) by installing, upgrading and
# uninstalling a real package with it, under a test identity of its own.
#
#   cmake -DBUILD=<build tree> -DISCC=<ISCC.exe> -DSOURCE=<source tree>
#         -DVERSION=<the package's version label> -DWORK=<work directory>
#         -P check_windows_installer.cmake
#
# The test identity (an application id, a name, a file type and a PATH value of its own) means
# that nothing of an installed Bloom is read or changed: the installers made here install
# "Bloom Package Check" for the current user, into WORK, and register the file type
# .bloomcheck and the user environment value BloomPackageCheckPath. Nothing is written under
# HKEY_LOCAL_MACHINE.
#
# Steps, each reported on one line as it passes:
#
#   1  the package is installed from the build tree and two installers are made from it, the
#      label's own and one of version 9.9.9
#   2  silent install of the first, into WORK/installed, with the PATH task: the programs, the
#      launcher, the install mark and the uninstaller are there; the Start menu shortcut, the
#      uninstall entry (with the label), the file type keys and the PATH value are there;
#      nothing is under HKEY_LOCAL_MACHINE
#   3  the launcher in the installed cli folder starts the real bloom-cli, which renders
#      headless through the installed package (check_package.cmake, rule `run`)
#   4  silent install of the second, with no folder given: it finds the first by its id and
#      upgrades it in place; the entry says 9.9.9, the programs are there, the previous version
#      is kept as WORK/installed.previous, and PATH still holds the folder once
#   5  silent uninstall: the programs, the previous version, the shortcut, the uninstall entry,
#      the file type keys and the PATH value are gone
#
# Whatever fails, the uninstaller is run (when it is there) and the test's keys, value and
# shortcut are removed before the result is reported.
#
# Without the Inno Setup compiler, or on another host than Windows, the script says
# "SKIPPED:" and why, and the test is reported as skipped (cmake/BloomPackage.cmake).
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS BUILD ISCC SOURCE VERSION WORK)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "check_windows_installer.cmake needs ${argument}")
    endif()
endforeach()
if(NOT CMAKE_HOST_WIN32)
    message(STATUS "SKIPPED: the Windows installer is checked on Windows only")
    return()
endif()
if(NOT EXISTS "${ISCC}")
    message(STATUS "SKIPPED: there is no Inno Setup compiler at ${ISCC}, so no installer can be "
                   "made here")
    return()
endif()

# --- The test identity ---------------------------------------------------------------------------
# Not the installer's own application id (bloom.iss), so that an installed Bloom is never found.
set(test_id "{5BEE37DA-3902-4302-9768-146EC14F0F15}")
set(test_name "Bloom Package Check")
set(test_extension ".bloomcheck")
set(test_prog_id "BloomPackageCheck.Project")
set(test_path_value "BloomPackageCheckPath")
set(defines "AppId={${test_id}" "AppName=${test_name}" "ProjectExtension=${test_extension}"
            "ProjectProgId=${test_prog_id}" "PathValueName=${test_path_value}")
set(upgrade_version "9.9.9")

set(uninstall_key
    "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${test_id}_is1")
set(extension_key "HKCU\\Software\\Classes\\${test_extension}")
set(prog_id_key "HKCU\\Software\\Classes\\${test_prog_id}")
set(environment_key "HKCU\\Environment")

get_filename_component(WORK "${WORK}" ABSOLUTE)
set(package_name "Bloom-${VERSION}-windows-x86_64")
set(package "${WORK}/package/${package_name}")
set(installed "${WORK}/installed")
file(TO_NATIVE_PATH "${installed}" native_installed)
file(TO_NATIVE_PATH "${installed}.previous" native_previous)
file(TO_CMAKE_PATH "$ENV{APPDATA}" roaming_data)
set(shortcut "${roaming_data}/Microsoft/Windows/Start Menu/Programs/${test_name}.lnk")
set(scripts "${CMAKE_CURRENT_LIST_DIR}")

# --- Helpers -------------------------------------------------------------------------------------
function(fail text)
    set_property(GLOBAL APPEND_STRING PROPERTY bloom_failures "${text}\n")
endfunction()

function(failed result_variable)
    get_property(text GLOBAL PROPERTY bloom_failures)
    if(text)
        set(${result_variable} TRUE PARENT_SCOPE)
    else()
        set(${result_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(passed text)
    failed(bad)
    if(NOT bad)
        message(STATUS "ok: ${text}")
    endif()
endfunction()

# Reads one registry value. `name` empty reads the key's default value. found is TRUE when the
# value exists; type and data are what reg.exe prints (REG_SZ, REG_EXPAND_SZ and the text).
function(read_registry key name found type data)
    if(name STREQUAL "")
        set(selector /ve)
        set(pattern "\\(Default\\)")
    else()
        set(selector /v "${name}")
        set(pattern "${name}")
    endif()
    set(${found} FALSE PARENT_SCOPE)
    set(${type} "" PARENT_SCOPE)
    set(${data} "" PARENT_SCOPE)
    execute_process(COMMAND reg query "${key}" ${selector}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_QUIET)
    if(result EQUAL 0 AND output MATCHES "[ \t]${pattern}[ \t]+(REG_[A-Z_]+)[ \t]+([^\r\n]*)")
        set(${found} TRUE PARENT_SCOPE)
        set(${type} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        string(STRIP "${CMAKE_MATCH_2}" value)
        set(${data} "${value}" PARENT_SCOPE)
    endif()
endfunction()

function(key_exists key result_variable)
    execute_process(COMMAND reg query "${key}" RESULT_VARIABLE result
        OUTPUT_QUIET ERROR_QUIET)
    if(result EQUAL 0)
        set(${result_variable} TRUE PARENT_SCOPE)
    else()
        set(${result_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

# The value must exist and equal `expected`, whatever the case (Windows paths).
function(expect_registry what key name expected)
    read_registry("${key}" "${name}" found type data)
    string(TOLOWER "${data}" lower_data)
    string(TOLOWER "${expected}" lower_expected)
    if(NOT found)
        fail("${what}: ${key} ${name} is not there")
    elseif(NOT lower_data STREQUAL lower_expected)
        fail("${what}: ${key} ${name} is '${data}', not '${expected}'")
    endif()
endfunction()

function(expect_file what path)
    if(NOT EXISTS "${path}")
        fail("${what}: ${path} does not exist")
    endif()
endfunction()

function(expect_no_file what path)
    if(EXISTS "${path}")
        fail("${what}: ${path} still exists")
    endif()
endfunction()

# Runs an installer or uninstaller silently; it must exit with 0.
function(run_silent what program log)
    file(TO_NATIVE_PATH "${log}" native_log)
    execute_process(
        COMMAND "${program}" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/LOG=${native_log}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 900)
    if(NOT result EQUAL 0)
        fail("${what} ended with ${result} (log: ${log}): ${output}${error}")
    endif()
endfunction()

# The uninstaller starts a copy of itself and may end before that copy has finished, so the
# end is waited for: it is over when the uninstall entry and the PATH value are gone.
function(wait_for_uninstall)
    foreach(attempt RANGE 120)
        key_exists("${uninstall_key}" present)
        read_registry("${environment_key}" "${test_path_value}" found type data)
        if(NOT present AND NOT found)
            return()
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
    endforeach()
endfunction()

# Makes an installer of the package under the test identity.
function(make_installer version output_name)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DISCC=${ISCC}" "-DPACKAGE=${package}" "-DSOURCE=${SOURCE}"
            "-DVERSION=${version}" "-DOUTPUT_DIR=${WORK}/installers"
            "-DOUTPUT_NAME=${output_name}" "-DDEFINES=${defines}"
            -P "${scripts}/make_windows_installer.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        fail("the installer ${output_name} could not be made: ${output}${error}")
    endif()
endfunction()

# The PATH value must hold the installed cli folder, once and alone.
function(expect_path_entry what)
    read_registry("${environment_key}" "${test_path_value}" found type data)
    if(NOT found)
        fail("${what}: HKCU\\Environment ${test_path_value} is not there")
        return()
    endif()
    if(NOT type STREQUAL "REG_EXPAND_SZ")
        fail("${what}: the PATH value is ${type}, not REG_EXPAND_SZ")
    endif()
    string(TOLOWER "${data}" lower_data)
    string(TOLOWER "${native_installed}\\cli" lower_entry)
    string(REGEX MATCHALL ";" separators "${lower_data}")
    if(separators OR NOT lower_data STREQUAL lower_entry)
        fail("${what}: the PATH value is '${data}', not the one entry '${native_installed}\\cli'")
    endif()
endfunction()

# --- Steps ---------------------------------------------------------------------------------------
function(step_make_installers)
    file(REMOVE_RECURSE "${WORK}/package" "${WORK}/installers")
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${package}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        fail("the package could not be installed from the build tree: ${output}${error}")
        return()
    endif()
    make_installer("${VERSION}" "check-setup-first")
    make_installer("${upgrade_version}" "check-setup-upgrade")
    passed("two installers made from the package (${VERSION} and ${upgrade_version}) under "
           "the test identity")
endfunction()

function(step_install)
    # Nothing of an earlier, failed run is left.
    file(REMOVE_RECURSE "${installed}" "${installed}.previous")
    run_silent("the silent install" "${WORK}/installers/check-setup-first.exe"
        "${WORK}/install.log" "/DIR=${native_installed}" "/TASKS=addtopath")
    failed(bad)
    if(bad)
        return()
    endif()
    foreach(file IN ITEMS Bloom.exe bloom-cli.exe cli/bloom-cli.exe unins000.exe)
        expect_file("the install" "${installed}/${file}")
    endforeach()
    expect_file("the install" "${installed}/install-kind")
    if(EXISTS "${installed}/install-kind")
        file(READ "${installed}/install-kind" mark)
        if(NOT mark MATCHES "(^|\n)kind=windows-installer[ \t\r]*(\n|$)")
            fail("the install: install-kind does not hold kind=windows-installer: '${mark}'")
        endif()
    endif()
    expect_file("the Start menu entry" "${shortcut}")
    expect_registry("the uninstall entry" "${uninstall_key}" DisplayVersion "${VERSION}")
    expect_registry("the file type" "${extension_key}" "" "${test_prog_id}")
    expect_registry("the file type name" "${prog_id_key}" "" "Bloom Project")
    expect_registry("the file type icon" "${prog_id_key}\\DefaultIcon" ""
        "${native_installed}\\Bloom.exe,0")
    expect_registry("the file type command" "${prog_id_key}\\shell\\open\\command" ""
        "\"${native_installed}\\Bloom.exe\" \"%1\"")
    expect_path_entry("the PATH option")
    # Per user: nothing was written for all users.
    foreach(machine_key IN ITEMS
            "HKLM\\Software\\Classes\\${test_extension}"
            "HKLM\\Software\\Classes\\${test_prog_id}"
            "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${test_id}_is1"
            "HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${test_id}_is1")
        key_exists("${machine_key}" present)
        if(present)
            fail("the install wrote ${machine_key}, which is for all users")
        endif()
    endforeach()
    passed("silent install without administrator rights: programs, install mark, uninstaller, "
           "Start menu entry, uninstall entry, file type and PATH entry")
endfunction()

function(step_launcher)
    # The launcher in the cli folder starts the real bloom-cli, which renders headless through
    # the installed package.
    set(launcher "${installed}/cli/bloom-cli.exe")
    execute_process(COMMAND "${launcher}" --version
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    string(STRIP "${output}" reported)
    if(NOT result EQUAL 0 OR NOT reported STREQUAL "bloom-cli ${VERSION}")
        fail("the launcher's --version gave (${result}) '${reported}${error}', not "
             "'bloom-cli ${VERSION}'")
        return()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${installed}" -DRUN=ON -DONLY=run
            "-DVERSION=${VERSION}" "-DWORK=${WORK}/run" "-DCLI_PROGRAM=${launcher}"
            -P "${scripts}/check_package.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        fail("the launcher did not render through the installed package: ${output}${error}")
        return()
    endif()
    passed("bloom-cli from the cli folder renders headless through the installed package")
endfunction()

function(step_upgrade)
    run_silent("the silent upgrade" "${WORK}/installers/check-setup-upgrade.exe"
        "${WORK}/upgrade.log")
    failed(bad)
    if(bad)
        return()
    endif()
    expect_registry("the upgrade" "${uninstall_key}" DisplayVersion "${upgrade_version}")
    foreach(file IN ITEMS Bloom.exe bloom-cli.exe cli/bloom-cli.exe unins000.exe install-kind)
        expect_file("the upgrade" "${installed}/${file}")
    endforeach()
    expect_file("the kept previous version" "${installed}.previous/Bloom.exe")
    expect_path_entry("the PATH entry after the upgrade")
    passed("silent upgrade in place to ${upgrade_version}: the previous version is kept and "
           "PATH holds the folder once")
endfunction()

function(step_uninstall)
    run_silent("the silent uninstall" "${installed}/unins000.exe" "${WORK}/uninstall.log")
    wait_for_uninstall()
    expect_no_file("the uninstall" "${installed}.previous")
    expect_no_file("the uninstall" "${shortcut}")
    key_exists("${uninstall_key}" present)
    if(present)
        fail("the uninstall left ${uninstall_key}")
    endif()
    foreach(key IN ITEMS "${extension_key}" "${prog_id_key}")
        key_exists("${key}" present)
        if(present)
            fail("the uninstall left ${key}")
        endif()
    endforeach()
    read_registry("${environment_key}" "${test_path_value}" found type data)
    if(found)
        fail("the uninstall left the PATH value '${data}'")
    endif()
    # Only the uninstaller's own files may be left, while it removes itself.
    file(GLOB_RECURSE programs LIST_DIRECTORIES false "${installed}/*.exe")
    list(FILTER programs EXCLUDE REGEX "/unins[0-9]+\\.exe$")
    if(programs)
        fail("the uninstall left programs: ${programs}")
    endif()
    passed("silent uninstall: programs, previous version, Start menu entry, uninstall entry, "
           "file type and PATH entry are gone")
endfunction()

# Leaves nothing of the test behind, whatever happened.
function(clean_up)
    # The uninstaller runs only while its data file is there: a copy of it that is still
    # removing itself after an uninstall must not be started again.
    if(EXISTS "${installed}/unins000.exe" AND EXISTS "${installed}/unins000.dat")
        execute_process(
            COMMAND "${installed}/unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
            RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET TIMEOUT 900)
        wait_for_uninstall()
    endif()
    foreach(key IN ITEMS "${extension_key}" "${prog_id_key}" "${uninstall_key}")
        execute_process(COMMAND reg delete "${key}" /f
            RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    endforeach()
    execute_process(COMMAND reg delete "${environment_key}" /v "${test_path_value}" /f
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE "${shortcut}")
    file(REMOVE_RECURSE "${installed}" "${installed}.previous")
endfunction()

# --- Run -----------------------------------------------------------------------------------------
file(MAKE_DIRECTORY "${WORK}")
foreach(step IN ITEMS make_installers install launcher upgrade uninstall)
    cmake_language(CALL "step_${step}")
    failed(bad)
    if(bad)
        break()
    endif()
endforeach()
clean_up()
get_property(failures GLOBAL PROPERTY bloom_failures)
if(failures)
    message(FATAL_ERROR "The Windows installer check failed:\n${failures}")
endif()
message(STATUS "The Windows installer installs, upgrades and uninstalls as required")

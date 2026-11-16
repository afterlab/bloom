# Checks the macOS disk image and installer package (cmake/package/make_macos_installers.cmake)
# by making them from a copy of the built bundle and looking inside them.
#
#   cmake -DBUNDLE=<built Bloom.app> -DSOURCE=<source tree> -DWORK=<work directory>
#         [-DPLISTBUDDY=<PlistBuddy>] -P check_macos_installers.cmake
#
# Everything happens inside WORK: the image is attached read-only at WORK/mount and not shown,
# the package is expanded, never installed (`installer -pkg` needs an administrator), and the
# postinstall script links inside WORK/volume, never in the real /usr/local/bin. The steps, each
# reported on one line as it passes:
#
#   1  the bundle is copied and signed as bloom-deploy-macos signs it, and both installers are
#      made from the copy
#   2  the disk image holds Bloom.app (the same program, a signature that still verifies), the
#      Applications link to /Applications, the background picture with its two sizes (660 x 400
#      and 1320 x 800) and, when the source tree has one, the window layout
#   3  the package installs Bloom.app and bloom-cli into /Applications, is not relocatable, is
#      not version checked, and carries the checked-in postinstall script, executable
#   4  the postinstall script links bloom-cli inside the target volume it is given, links again
#      without complaint, and leaves a file that is not a link alone
#
# Whatever fails, the image is detached before the result is reported.
#
# On another host than macOS the script says "SKIPPED:" and why, and the test is reported as
# skipped (cmake/BloomPackage.cmake).
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS BUNDLE SOURCE WORK)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "check_macos_installers.cmake needs ${argument}")
    endif()
endforeach()
if(NOT CMAKE_HOST_APPLE)
    message(STATUS "SKIPPED: the macOS installers are checked on macOS only")
    return()
endif()
if(NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "check_macos_installers.cmake: BUNDLE is not a directory: '${BUNDLE}'")
endif()
if(NOT DEFINED PLISTBUDDY OR "${PLISTBUDDY}" STREQUAL "")
    set(PLISTBUDDY "/usr/libexec/PlistBuddy")
endif()

get_filename_component(WORK "${WORK}" ABSOLUTE)
set(scripts "${CMAKE_CURRENT_LIST_DIR}")
set(copy "${WORK}/Bloom.app")
set(output "${WORK}/installers")
set(make_work "${WORK}/make")
set(mount "${WORK}/mount")
set(expanded "${WORK}/expanded")
set(name "Bloom-installer-check")
set(dmg "${output}/${name}.dmg")
set(pkg "${output}/${name}.pkg")
set(layout "${SOURCE}/apps/bloom/installer/macos/dmg-DS_Store")
set(postinstall "${SOURCE}/packaging/macos/postinstall")
set(mounted FALSE)

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

function(expect_file what path)
    if(NOT EXISTS "${path}")
        fail("${what}: ${path} is missing")
    endif()
endfunction()

# Runs one command and records a failure with its output; the result and output are returned.
function(run what result_variable output_variable)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " command)
        fail("${what} failed (${result}): ${command}\n${output}${error}")
    endif()
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

# A picture's size in pixels as "<width> x <height>", read by sips.
function(picture_size what path result_variable)
    set(${result_variable} "" PARENT_SCOPE)
    run("${what}" status text sips -g pixelWidth -g pixelHeight "${path}")
    if(text MATCHES "pixelWidth: *([0-9]+)" )
        set(width "${CMAKE_MATCH_1}")
        if(text MATCHES "pixelHeight: *([0-9]+)")
            set(${result_variable} "${width} x ${CMAKE_MATCH_1}" PARENT_SCOPE)
            return()
        endif()
    endif()
    fail("${what}: sips gave no size for ${path}:\n${text}")
endfunction()

function(step_make)
    file(MAKE_DIRECTORY "${WORK}")
    run("copying the bundle" status text ditto "${BUNDLE}" "${copy}")
    # As bloom-deploy-macos does (apps/bloom/macos/sign_bundle.cmake).
    run("signing the copy" status text "${CMAKE_COMMAND}" "-DBUNDLE=${copy}"
        -P "${SOURCE}/apps/bloom/macos/sign_bundle.cmake")
    run("reading the bundle's build version" status build_version
        "${PLISTBUDDY}" -c "Print CFBundleVersion" "${copy}/Contents/Info.plist")
    string(STRIP "${build_version}" build_version)
    run("reading the bundle's short version" status short_version
        "${PLISTBUDDY}" -c "Print CFBundleShortVersionString" "${copy}/Contents/Info.plist")
    string(STRIP "${short_version}" short_version)
    set(build_version "${build_version}" PARENT_SCOPE)
    if(build_version STREQUAL "" OR short_version STREQUAL "")
        fail("the bundle's Info.plist has no version")
        return()
    endif()
    run("making the installers" status text "${CMAKE_COMMAND}" "-DBUNDLE=${copy}"
        "-DSOURCE=${SOURCE}" "-DVERSION=${short_version}" "-DBUILD_VERSION=${build_version}"
        "-DOUTPUT=${output}" "-DWORK=${make_work}" "-DNAME=${name}"
        "-DPLISTBUDDY=${PLISTBUDDY}" -P "${scripts}/make_macos_installers.cmake")
    expect_file("the disk image was made" "${dmg}")
    expect_file("the installer package was made" "${pkg}")
    passed("both installers were made from the signed copy")
endfunction()

function(step_image)
    file(MAKE_DIRECTORY "${mount}")
    execute_process(COMMAND hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "${mount}"
                            "${dmg}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        fail("the disk image could not be attached (${result}):\n${output}${error}")
        return()
    endif()
    set(mounted TRUE PARENT_SCOPE)

    expect_file("the image holds the application" "${mount}/Bloom.app/Contents/MacOS/Bloom")
    expect_file("the image holds the command-line program"
                "${mount}/Bloom.app/Contents/MacOS/bloom-cli")
    if(EXISTS "${mount}/Bloom.app/Contents/MacOS/Bloom" AND
       EXISTS "${copy}/Contents/MacOS/Bloom")
        file(SHA256 "${mount}/Bloom.app/Contents/MacOS/Bloom" in_image)
        file(SHA256 "${copy}/Contents/MacOS/Bloom" in_copy)
        if(NOT in_image STREQUAL in_copy)
            fail("the application in the image is not the application that was copied")
        endif()
    endif()
    # The link is read as a link: it must name /Applications and not be a copy of the folder.
    if(NOT IS_SYMLINK "${mount}/Applications")
        fail("the image's Applications is not a link")
    else()
        file(READ_SYMLINK "${mount}/Applications" target)
        if(NOT target STREQUAL "/Applications")
            fail("the image's Applications link names '${target}', not /Applications")
        endif()
    endif()
    run("verifying the application's signature in the image" status text
        codesign --verify --deep --strict "${mount}/Bloom.app")

    # The background: one file holding the 660 x 400 and the 1320 x 800 pictures.
    set(background "${mount}/.background/dmg-background.tiff")
    expect_file("the image holds the background picture" "${background}")
    if(EXISTS "${background}")
        set(sizes "")
        foreach(index IN ITEMS 0 1)
            set(part "${WORK}/background-${index}.tiff")
            run("taking picture ${index} out of the background" status text
                tiffutil -extract ${index} "${background}" -out "${part}")
            if(EXISTS "${part}")
                picture_size("measuring picture ${index} of the background" "${part}" size)
                list(APPEND sizes "${size}")
            endif()
        endforeach()
        list(SORT sizes)
        if(NOT sizes STREQUAL "1320 x 800;660 x 400")
            fail("the background holds pictures of '${sizes}', not 660 x 400 and 1320 x 800")
        endif()
    endif()
    if(EXISTS "${layout}")
        expect_file("the image holds the window layout" "${mount}/.DS_Store")
    endif()
    passed("the disk image holds the application, the Applications link, the two background "
           "pictures and a signature that verifies")
endfunction()

# The value of a key of one entry of the component plist, empty when it is not there.
function(plist_value plist index key result_variable)
    execute_process(COMMAND "${PLISTBUDDY}" -c "Print :${index}:${key}" "${plist}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_QUIET)
    string(STRIP "${output}" output)
    if(NOT result EQUAL 0)
        set(output "")
    endif()
    set(${result_variable} "${output}" PARENT_SCOPE)
endfunction()

function(step_package)
    file(REMOVE_RECURSE "${expanded}")
    run("expanding the package" status text pkgutil --expand-full "${pkg}" "${expanded}")
    file(GLOB components LIST_DIRECTORIES TRUE "${expanded}/*.pkg")
    list(LENGTH components count)
    if(NOT count EQUAL 1)
        fail("the package holds ${count} components, not 1: ${components}")
        return()
    endif()
    list(GET components 0 component)
    set(payload "${component}/Payload")
    expect_file("the package installs the application" "${payload}/Bloom.app/Contents/MacOS/Bloom")
    expect_file("the package installs the command-line program"
                "${payload}/Bloom.app/Contents/MacOS/bloom-cli")

    expect_file("the package describes itself" "${component}/PackageInfo")
    if(EXISTS "${component}/PackageInfo")
        file(READ "${component}/PackageInfo" info)
        if(NOT info MATCHES "install-location=\"/Applications\"")
            fail("the package does not install into /Applications:\n${info}")
        endif()
        if(NOT info MATCHES "identifier=\"id\\.afterlab\\.bloom\\.pkg\"")
            fail("the package has not the identifier id.afterlab.bloom.pkg:\n${info}")
        endif()
        string(REPLACE "." "\\." wanted_version "${build_version}")
        if(NOT info MATCHES "[ \t]version=\"${wanted_version}\"")
            fail("the package's version is not the bundle's build version ${build_version}:\n${info}")
        endif()
        # A relocatable bundle is listed under <relocate>, a version checked one under
        # <bundle-version>.
        if(info MATCHES "<relocate")
            fail("the package lets the installer move the application to another copy:\n${info}")
        endif()
        if(info MATCHES "<bundle-version")
            fail("the package checks the version of the application it replaces:\n${info}")
        endif()
    endif()
    # What was asked of the package, read from the component plist the installers were made with.
    set(plist "${make_work}/pkg-components.plist")
    expect_file("the component plist was kept" "${plist}")
    if(EXISTS "${plist}")
        plist_value("${plist}" 0 RootRelativeBundlePath first)
        if(first STREQUAL "")
            fail("the component plist lists no bundle")
        endif()
        foreach(index RANGE 0 9999)
            plist_value("${plist}" ${index} RootRelativeBundlePath path)
            if(path STREQUAL "")
                break()
            endif()
            foreach(key IN ITEMS BundleIsRelocatable BundleIsVersionChecked)
                plist_value("${plist}" ${index} ${key} value)
                if(NOT value STREQUAL "false")
                    fail("the component plist has ${key} '${value}' for ${path}, not false")
                endif()
            endforeach()
        endforeach()
    endif()

    set(installed_script "${component}/Scripts/postinstall")
    expect_file("the package carries a postinstall script" "${installed_script}")
    if(EXISTS "${installed_script}")
        execute_process(COMMAND test -x "${installed_script}" RESULT_VARIABLE not_executable)
        if(NOT not_executable EQUAL 0)
            fail("the package's postinstall script is not executable")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${installed_script}"
                                "${postinstall}" RESULT_VARIABLE different)
        if(NOT different EQUAL 0)
            fail("the package's postinstall script is not ${postinstall}")
        endif()
    endif()
    set(postinstall_in_package "${installed_script}" PARENT_SCOPE)
    passed("the package installs Bloom.app and bloom-cli into /Applications, is not relocatable "
           "or version checked, and carries the postinstall script")
endfunction()

function(step_postinstall)
    if(NOT EXISTS "${postinstall_in_package}")
        fail("the postinstall script was not found, so it was not run")
        return()
    endif()
    # The installer's four arguments: the package, the install location, the target volume and
    # the system root of the target volume.
    set(volume "${WORK}/volume")
    file(MAKE_DIRECTORY "${volume}")
    run("running the postinstall script" status text
        "${postinstall_in_package}" "${pkg}" /Applications "${volume}" "${volume}")
    set(link "${volume}/usr/local/bin/bloom-cli")
    if(NOT IS_SYMLINK "${link}")
        fail("the postinstall script made no link at ${link}")
    else()
        file(READ_SYMLINK "${link}" target)
        if(NOT target STREQUAL "/Applications/Bloom.app/Contents/MacOS/bloom-cli")
            fail("the link names '${target}', not the program in /Applications/Bloom.app")
        endif()
    endif()
    # Again, as an upgrade does: the link stays and nothing is said.
    run("running the postinstall script again" status text
        "${postinstall_in_package}" "${pkg}" /Applications "${volume}/" "${volume}")
    if(NOT IS_SYMLINK "${link}")
        fail("the second run of the postinstall script lost the link")
    endif()
    # A file that is not a link is left alone, and the install still succeeds.
    set(other "${WORK}/other-volume")
    file(MAKE_DIRECTORY "${other}/usr/local/bin")
    file(WRITE "${other}/usr/local/bin/bloom-cli" "not a link\n")
    run("running the postinstall script over a file" status text
        "${postinstall_in_package}" "${pkg}" /Applications "${other}" "${other}")
    file(READ "${other}/usr/local/bin/bloom-cli" kept)
    if(IS_SYMLINK "${other}/usr/local/bin/bloom-cli" OR NOT kept STREQUAL "not a link\n")
        fail("the postinstall script changed a file that is not a link")
    endif()
    passed("the postinstall script links bloom-cli inside its target volume and leaves a file alone")
endfunction()

# --- Run -----------------------------------------------------------------------------------------
# A volume left mounted from an earlier run would hide this run's image under the same mount point.
if(IS_DIRECTORY "${mount}")
    execute_process(COMMAND hdiutil detach -force "${mount}" OUTPUT_QUIET ERROR_QUIET)
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

step_make()
failed(bad)
if(NOT bad)
    step_image()
    # Detached whatever happened, before anything else is read from the work folder.
    if(mounted)
        execute_process(COMMAND hdiutil detach -force "${mount}"
            RESULT_VARIABLE detached OUTPUT_VARIABLE detach_output ERROR_VARIABLE detach_error)
        if(NOT detached EQUAL 0)
            fail("the disk image could not be detached (${detached}):\n${detach_output}${detach_error}")
        endif()
    endif()
    step_package()
    step_postinstall()
endif()

failed(bad)
get_property(text GLOBAL PROPERTY bloom_failures)
if(bad)
    message(FATAL_ERROR "The macOS installers do not pass:\n${text}")
endif()
message(STATUS "The macOS installers pass")

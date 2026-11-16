# Makes the two macOS installers of a signed Bloom.app: the disk image and the installer package
# (docs/architecture/platform-support.md, "macOS Installers").
#
#   cmake -DBUNDLE=<signed Bloom.app> -DSOURCE=<source tree> -DVERSION=<version label>
#         -DBUILD_VERSION=<compact form> -DOUTPUT=<folder for the installers>
#         -DWORK=<work folder> [-DNAME=<file name without extension>]
#         [-DPLISTBUDDY=<PlistBuddy>] [-DPYTHON=<python program>] -P make_macos_installers.cmake
#
# NAME defaults to Bloom-<VERSION>-macos-<processor>, the name of the zip the updater downloads.
# BUILD_VERSION is the bundle's build version (cmake/BloomVersion.cmake), which the package
# records as its version. The work folder keeps only the package's component plist
# (pkg-components.plist); this script removes the parts it makes before it makes them again, and
# touches nothing else in it.
#
# PYTHON is the program that runs tools/release/dmg_layout.py; without it the first python3 on
# the path is used.
#
# <NAME>.dmg: the window a person sees after opening it. The volume is named Bloom, so that the
# window layout finds its picture. It holds Bloom.app, a link named Applications, and the
# background picture in the hidden folder .background as one TIFF with both sizes. The window
# layout (the .DS_Store Finder reads) is generated, with no Finder step: the image is made
# read-write, attached, given its .DS_Store by tools/release/dmg_layout.py, detached, and
# converted to the compressed image. The image is detached whatever the tool said.
#
# <NAME>.pkg: installs Bloom.app into /Applications with no window (`installer -pkg`). Its
# postinstall script (packaging/macos/postinstall) links /usr/local/bin/bloom-cli to the program
# in the bundle. The bundle is not relocatable, so the installer never updates a copy found
# elsewhere, and its version is not checked, so an older package may be installed over a newer
# one on purpose.
#
# Every command that fails stops the script with the command line and its output. Nothing here
# is signed again: the bundle is copied as it is, and `ditto` keeps its links and signatures.
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS BUNDLE SOURCE VERSION BUILD_VERSION OUTPUT WORK)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "make_macos_installers.cmake needs ${argument}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "make_macos_installers.cmake: BUNDLE is not a directory: '${BUNDLE}'")
endif()
get_filename_component(bundle_name "${BUNDLE}" NAME)
if(NOT bundle_name STREQUAL "Bloom.app")
    message(FATAL_ERROR "make_macos_installers.cmake: the bundle must be named Bloom.app, not "
                        "'${bundle_name}'")
endif()
set(picture "${SOURCE}/apps/bloom/installer/macos/dmg-background.png")
set(picture_2x "${SOURCE}/apps/bloom/installer/macos/dmg-background@2x.png")
set(layout_tool "${SOURCE}/tools/release/dmg_layout.py")
set(postinstall "${SOURCE}/packaging/macos/postinstall")
foreach(needed IN ITEMS "${picture}" "${picture_2x}" "${postinstall}" "${layout_tool}")
    if(NOT EXISTS "${needed}")
        message(FATAL_ERROR "make_macos_installers.cmake: '${needed}' does not exist")
    endif()
endforeach()
if(NOT DEFINED PLISTBUDDY OR "${PLISTBUDDY}" STREQUAL "")
    set(PLISTBUDDY "/usr/libexec/PlistBuddy")
endif()
if(NOT DEFINED PYTHON OR "${PYTHON}" STREQUAL "")
    find_program(PYTHON NAMES python3 REQUIRED)
endif()

if(NOT NAME)
    # Script mode does not set the processor variables: the machine's own name is what the
    # zip's name (apps/bloom/CMakeLists.txt) is made from.
    execute_process(COMMAND uname -m OUTPUT_VARIABLE processor OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
    string(TOLOWER "${processor}" processor)
    if(processor MATCHES "^(arm64|aarch64)$")
        set(processor "arm64")
    elseif(processor MATCHES "^(x86_64|amd64)$")
        set(processor "x86_64")
    endif()
    set(NAME "Bloom-${VERSION}-macos-${processor}")
endif()
get_filename_component(OUTPUT "${OUTPUT}" ABSOLUTE)
get_filename_component(WORK "${WORK}" ABSOLUTE)
file(MAKE_DIRECTORY "${OUTPUT}" "${WORK}")
set(dmg "${OUTPUT}/${NAME}.dmg")
set(pkg "${OUTPUT}/${NAME}.pkg")
# A file left from an earlier run must not pass for this one.
file(REMOVE "${dmg}" "${pkg}")

# Runs one command; any failure stops the script and shows what was run and what it said.
function(bloom_run what)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " command)
        message(FATAL_ERROR "make_macos_installers.cmake: ${what} failed (${result}):\n"
                            "${command}\n${output}${error}")
    endif()
endfunction()

# One value of a property list file, empty when it is not there.
function(plist_entry plist entry result_variable)
    execute_process(COMMAND "${PLISTBUDDY}" -c "Print ${entry}" "${plist}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_QUIET)
    string(STRIP "${output}" output)
    if(NOT result EQUAL 0)
        set(output "")
    endif()
    set(${result_variable} "${output}" PARENT_SCOPE)
endfunction()

# Writes the property list a hdiutil command printed, without anything printed around it.
function(write_plist text path)
    string(FIND "${text}" "<?xml" first)
    string(FIND "${text}" "</plist>" last REVERSE)
    if(first EQUAL -1 OR last EQUAL -1)
        file(WRITE "${path}" "")
        return()
    endif()
    math(EXPR length "${last} + 8 - ${first}")
    string(SUBSTRING "${text}" ${first} ${length} text)
    file(WRITE "${path}" "${text}\n")
endfunction()

# --- The disk image ------------------------------------------------------------------------------
set(staging "${WORK}/dmg-staging")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}/.background")
bloom_run("copying the bundle for the disk image" ditto "${BUNDLE}" "${staging}/Bloom.app")
file(CREATE_LINK "/Applications" "${staging}/Applications" SYMBOLIC RESULT link_result)
if(NOT link_result STREQUAL "0")
    message(FATAL_ERROR "make_macos_installers.cmake: the Applications link could not be made: "
                        "${link_result}")
endif()
# One TIFF with both sizes: the window shows the picture that fits the display. The pictures
# carry no resolution, and the check of tiffutil -cathidpicheck wants 72 dots per inch for the
# standard picture and 144 for the double one, so the copies it reads are tagged with those.
set(pictures "${WORK}/dmg-pictures")
file(REMOVE_RECURSE "${pictures}")
file(MAKE_DIRECTORY "${pictures}")
bloom_run("tagging the standard background picture" sips -s dpiWidth 72 -s dpiHeight 72
    "${picture}" --out "${pictures}/dmg-background.png")
bloom_run("tagging the double-size background picture" sips -s dpiWidth 144 -s dpiHeight 144
    "${picture_2x}" --out "${pictures}/dmg-background@2x.png")
bloom_run("combining the two background pictures" tiffutil -cathidpicheck
    "${pictures}/dmg-background.png" "${pictures}/dmg-background@2x.png"
    -out "${staging}/.background/dmg-background.tiff")
file(REMOVE_RECURSE "${pictures}")
# A zero-filled .DS_Store reserves room in the image; the tool replaces it below.
bloom_run("reserving room for the window layout" dd "if=/dev/zero" "of=${staging}/.DS_Store"
    bs=1024 count=16)

# The layout is written into a read-write image and the image is then compressed. The image is
# attached without a mount point of our own, which a remote session refuses.
set(layout_image "${WORK}/dmg-layout.dmg")
file(REMOVE "${layout_image}")
bloom_run("making the read-write disk image" hdiutil create -volname Bloom -srcfolder "${staging}"
    -fs HFS+ -format UDRW -ov "${layout_image}")
execute_process(COMMAND hdiutil attach -readwrite -nobrowse -noautoopen -plist "${layout_image}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "make_macos_installers.cmake: attaching the read-write disk image failed "
                        "(${result}):\n${output}${error}")
endif()
set(attach_plist "${WORK}/dmg-attached.plist")
write_plist("${output}" "${attach_plist}")
plist_entry("${attach_plist}" ":system-entities:0:dev-entry" device)
set(volume "")
foreach(entity RANGE 0 31)
    plist_entry("${attach_plist}" ":system-entities:${entity}:dev-entry" entity_device)
    if(entity_device STREQUAL "")
        break()
    endif()
    plist_entry("${attach_plist}" ":system-entities:${entity}:mount-point" point)
    if(NOT point STREQUAL "")
        set(volume "${point}")
    endif()
endforeach()
file(REMOVE "${attach_plist}")

# Whatever happens next, the image is detached before the script stops.
set(layout_problem "")
if(device STREQUAL "")
    set(layout_problem
        "hdiutil did not say which device the image is attached as:\n${output}${error}")
elseif(volume STREQUAL "" OR NOT IS_DIRECTORY "${volume}")
    set(layout_problem "the read-write image was attached without a volume:\n${output}${error}")
else()
    execute_process(COMMAND "${PYTHON}" -I "${layout_tool}" write --volume "${volume}" --bookmark
        RESULT_VARIABLE result OUTPUT_VARIABLE tool_output ERROR_VARIABLE tool_error)
    if(NOT result EQUAL 0)
        string(CONCAT layout_problem "writing the window layout failed (${result}):\n"
            "${PYTHON} -I ${layout_tool} write --volume ${volume} --bookmark\n"
            "${tool_output}${tool_error}")
    endif()
endif()
if(NOT device STREQUAL "")
    execute_process(COMMAND hdiutil detach "${device}"
        RESULT_VARIABLE detached OUTPUT_VARIABLE detach_output ERROR_VARIABLE detach_error)
    if(NOT detached EQUAL 0)
        execute_process(COMMAND hdiutil detach -force "${device}"
            RESULT_VARIABLE detached OUTPUT_VARIABLE detach_output ERROR_VARIABLE detach_error)
    endif()
    if(NOT detached EQUAL 0)
        string(APPEND layout_problem
            "\nthe read-write image could not be detached (${detached}):\n"
            "hdiutil detach ${device}\n${detach_output}${detach_error}")
    endif()
endif()
if(NOT layout_problem STREQUAL "")
    message(FATAL_ERROR "make_macos_installers.cmake: ${layout_problem}")
endif()
bloom_run("compressing the disk image" hdiutil convert "${layout_image}" -format UDZO -ov
    -o "${dmg}")
file(REMOVE "${layout_image}")
if(NOT EXISTS "${dmg}")
    message(FATAL_ERROR "make_macos_installers.cmake: hdiutil did not make ${dmg}")
endif()
file(REMOVE_RECURSE "${staging}")
message(STATUS "Made ${dmg}")

# --- The installer package -----------------------------------------------------------------------
set(root "${WORK}/pkg-root")
set(scripts "${WORK}/pkg-scripts")
set(component_plist "${WORK}/pkg-components.plist")
set(component "${WORK}/pkg-component.pkg")
file(REMOVE_RECURSE "${root}" "${scripts}")
file(REMOVE "${component_plist}" "${component}")
file(MAKE_DIRECTORY "${root}" "${scripts}")
bloom_run("copying the bundle for the package" ditto "${BUNDLE}" "${root}/Bloom.app")
file(COPY "${postinstall}" DESTINATION "${scripts}"
     FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                      WORLD_READ WORLD_EXECUTE)

# The analysis lists every bundle in the payload: the application and the frameworks and plug-ins
# inside it. None may move to another copy found elsewhere, and none is version checked.
bloom_run("analyzing the package contents" pkgbuild --analyze --root "${root}" "${component_plist}")
if(NOT EXISTS "${component_plist}")
    message(FATAL_ERROR "make_macos_installers.cmake: pkgbuild did not write ${component_plist}")
endif()
set(entries 0)
foreach(index RANGE 0 9999)
    execute_process(COMMAND "${PLISTBUDDY}" -c "Print :${index}:RootRelativeBundlePath"
                            "${component_plist}"
                    RESULT_VARIABLE found OUTPUT_QUIET ERROR_QUIET)
    if(NOT found EQUAL 0)
        break()
    endif()
    math(EXPR entries "${entries} + 1")
    foreach(key IN ITEMS BundleIsRelocatable BundleIsVersionChecked)
        execute_process(COMMAND "${PLISTBUDDY}" -c "Set :${index}:${key} false" "${component_plist}"
                        RESULT_VARIABLE set_result OUTPUT_QUIET ERROR_QUIET)
        if(NOT set_result EQUAL 0)
            # The analysis did not write the key: add it.
            bloom_run("setting ${key} of bundle ${index}" "${PLISTBUDDY}"
                -c "Add :${index}:${key} bool false" "${component_plist}")
        endif()
    endforeach()
endforeach()
if(entries EQUAL 0)
    message(FATAL_ERROR "make_macos_installers.cmake: the package analysis lists no bundle")
endif()

bloom_run("building the component package" pkgbuild
    --root "${root}" --install-location /Applications
    --component-plist "${component_plist}" --scripts "${scripts}"
    --identifier id.afterlab.bloom.pkg --version "${BUILD_VERSION}" "${component}")
bloom_run("building the installer package" productbuild --package "${component}" "${pkg}")
if(NOT EXISTS "${pkg}")
    message(FATAL_ERROR "make_macos_installers.cmake: productbuild did not make ${pkg}")
endif()
# The component plist stays in the work folder: the package check reads what was set in it.
file(REMOVE_RECURSE "${root}" "${scripts}")
file(REMOVE "${component}")
message(STATUS "Made ${pkg}")

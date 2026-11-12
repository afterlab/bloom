# Makes the two forms a Linux Bloom package is published in from one package tree
# (docs/architecture/platform-support.md, "Packages"): a tar, and an AppImage of the same tree.
#
#   cmake -DTREE=<package tree, a directory named like the package>
#         -DOUTPUT=<directory for the two files> -DWORK=<work directory>
#         -DSOURCE=<source tree> -DRUNTIME=<dependency prefix>/appimage-runtime
#         [-DEPOCH=<seconds; the time every file in both forms carries, default 0>]
#         -P make_archives.cmake
#
#   <OUTPUT>/<name>.tar.xz     the tree under a top directory of its own name
#   <OUTPUT>/<name>.AppImage   the AppImage project's runtime followed by the tree as a
#                              zstd-compressed SquashFS image
#
# The image holds the tree unchanged, and beside it what an AppImage has at its top: `AppRun`
# (a link to bin/Bloom), the desktop entry and the icon (copies of the tree's own), and `.DirIcon`.
# It also holds what only this form carries: the runtime's record and the license files of the
# runtime and of everything linked into it (dependencies/appimage-runtime), each compared with
# the record's SHA-256 first, in share/bloom/licenses/appimage-runtime.
#
# The runtime is the file the superbuild put in the prefix; its SHA-256 is compared with the
# record again here. No AppImage tool is used: the format is the runtime with the image
# appended. Both forms are made with fixed times, owners and file order, so the same tree gives
# the same bytes.
cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS TREE OUTPUT WORK SOURCE RUNTIME)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "make_archives.cmake needs ${argument}")
    endif()
endforeach()
if(NOT DEFINED EPOCH OR "${EPOCH}" STREQUAL "")
    set(EPOCH 0)
endif()
if(NOT EXISTS "${TREE}/bin/Bloom" OR NOT EXISTS "${TREE}/share/bloom/dependencies.lock.json")
    message(FATAL_ERROR "make_archives.cmake: ${TREE} is not a Linux Bloom package tree")
endif()
get_filename_component(name "${TREE}" NAME)
get_filename_component(parent "${TREE}" DIRECTORY)
find_program(BLOOM_TAR tar REQUIRED)
find_program(BLOOM_MKSQUASHFS mksquashfs REQUIRED)

set(record_file "${SOURCE}/dependencies/appimage-runtime/runtime.json")
file(READ "${record_file}" record)
string(JSON architecture GET "${record}" architecture)
string(JSON runtime_digest GET "${record}" sha256)
set(runtime "${RUNTIME}/runtime-${architecture}")
if(NOT EXISTS "${runtime}")
    message(FATAL_ERROR "make_archives.cmake: ${RUNTIME} has no runtime-${architecture}. Build "
                        "the superbuild's bloom_dependency_appimage_runtime.")
endif()
file(SHA256 "${runtime}" actual)
if(NOT actual STREQUAL runtime_digest)
    message(FATAL_ERROR "make_archives.cmake: ${runtime} is not the runtime the record names")
endif()

file(MAKE_DIRECTORY "${OUTPUT}")
set(tar "${OUTPUT}/${name}.tar.xz")
set(appimage "${OUTPUT}/${name}.AppImage")
file(REMOVE "${tar}" "${appimage}")

# The tar.
execute_process(
    COMMAND "${BLOOM_TAR}" --sort=name "--mtime=@${EPOCH}" --owner=0 --group=0 --numeric-owner
            --format=gnu -C "${parent}" -cJf "${tar}" "${name}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "tar failed: ${error}")
endif()

# The AppImage's directory: the tree, and what is at the top of an AppImage.
set(directory "${WORK}/${name}.AppDir")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND cp -a "${TREE}" "${directory}" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the tree could not be copied: ${error}")
endif()
set(entry "share/applications/org.kinetik.bloom.desktop")
set(icon "share/icons/hicolor/256x256/apps/org.kinetik.bloom.png")
if(NOT EXISTS "${directory}/${entry}" OR NOT EXISTS "${directory}/${icon}")
    message(FATAL_ERROR "make_archives.cmake: the tree has no desktop entry or no icon")
endif()
file(CREATE_LINK "bin/Bloom" "${directory}/AppRun" SYMBOLIC)
file(COPY "${directory}/${entry}" "${directory}/${icon}" DESTINATION "${directory}")
file(CREATE_LINK "org.kinetik.bloom.png" "${directory}/.DirIcon" SYMBOLIC)

# What only this form carries: the runtime's record and license files.
set(licenses "${directory}/share/bloom/licenses/appimage-runtime")
set(files "")
string(JSON path GET "${record}" license file path)
string(JSON digest GET "${record}" license file sha256)
list(APPEND files "${path}|${digest}")
string(JSON component_count LENGTH "${record}" components)
math(EXPR component_last "${component_count} - 1")
foreach(component RANGE 0 ${component_last})
    string(JSON file_count LENGTH "${record}" components ${component} files)
    if(file_count EQUAL 0)
        string(JSON component_name GET "${record}" components ${component} name)
        message(FATAL_ERROR "make_archives.cmake: the record names no license file for "
                            "${component_name}")
    endif()
    math(EXPR file_last "${file_count} - 1")
    foreach(file RANGE 0 ${file_last})
        string(JSON path GET "${record}" components ${component} files ${file} path)
        string(JSON digest GET "${record}" components ${component} files ${file} sha256)
        list(APPEND files "${path}|${digest}")
    endforeach()
endforeach()
foreach(item IN LISTS files)
    string(REGEX MATCH "^(.*)\\|([0-9a-f]+)$" matched "${item}")
    set(path "${CMAKE_MATCH_1}")
    set(digest "${CMAKE_MATCH_2}")
    if(NOT path MATCHES "^dependencies/appimage-runtime/licenses/(.+)$")
        message(FATAL_ERROR "make_archives.cmake: ${path} is not a license file of the runtime")
    endif()
    set(relative "${CMAKE_MATCH_1}")
    if(NOT EXISTS "${SOURCE}/${path}")
        message(FATAL_ERROR "make_archives.cmake: ${path} does not exist")
    endif()
    file(SHA256 "${SOURCE}/${path}" actual)
    if(NOT actual STREQUAL digest)
        message(FATAL_ERROR "make_archives.cmake: ${path} is not the file the record names")
    endif()
    get_filename_component(destination "${licenses}/${relative}" DIRECTORY)
    file(COPY "${SOURCE}/${path}" DESTINATION "${destination}")
endforeach()
file(COPY "${record_file}" DESTINATION "${directory}/share/bloom")
file(RENAME "${directory}/share/bloom/runtime.json"
            "${directory}/share/bloom/appimage-runtime.json")

# The image, then the runtime with the image after it. mksquashfs refuses a time given twice,
# so the environment's own is taken away.
set(image "${WORK}/${name}.squashfs")
unset(ENV{SOURCE_DATE_EPOCH})
execute_process(
    COMMAND "${BLOOM_MKSQUASHFS}" "${directory}" "${image}" -noappend -no-progress -comp zstd
            -Xcompression-level 19 -root-owned -no-xattrs -all-time ${EPOCH} -mkfs-time ${EPOCH}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mksquashfs failed: ${output}${error}")
endif()
execute_process(COMMAND cat "${runtime}" "${image}" OUTPUT_FILE "${appimage}"
                RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the AppImage could not be written: ${error}")
endif()
file(CHMOD "${appimage}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
     GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(REMOVE_RECURSE "${WORK}")

file(SIZE "${tar}" tar_size)
file(SIZE "${appimage}" appimage_size)
file(SHA256 "${tar}" tar_digest)
file(SHA256 "${appimage}" appimage_digest)
message(STATUS "Bloom package: ${name}.tar.xz ${tar_size} bytes, sha256 ${tar_digest}")
message(STATUS "Bloom package: ${name}.AppImage ${appimage_size} bytes, sha256 "
               "${appimage_digest}")

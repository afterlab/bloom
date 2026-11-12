# Generates bloom::ui::thirdPartyLicenseCatalog() (task U2, issue #118, decision 3) from the real,
# checked-in license records -- never hand-maintained -- so the "Open Source Licenses..." window
# always lists exactly what this build actually ships:
#
#   * every component of the dependency lock (dependencies/dependencies.lock.json), with every
#     license and notice file the lock names for it, whatever those files are called (issue #674)
#   * every other dependencies/licenses/<component>/LICENSE, src/media/third_party/<component>/
#     LICENSE and src/ui/kit/third_party/<component>/LICENSE (the vendored sources and the
#     vendored icon/font assets, which the lock does not list)
#   * the locked Qt's notice (dependencies/licenses/qt/NOTICE.txt) with the text of every license
#     it names (dependencies/licenses/qt/texts) -- see
#     docs/decisions/0014-apache-license-and-qt-distribution.md
#   * on Linux, the copyright file of every distribution package a Bloom package carries a
#     library from (dependencies/linux-runtime)
#
# Technique: mirrors src/color/ocio_builtin_payload.inc.in's file(READ ... HEX) -> "0x..," byte
# array embed, generalized from one frozen asset to N discovered files, because license text has
# no fixed byte count to assert against. An entry with several files embeds them one after the
# other, each under a line that names it. Runs at CMake configure time; included (not
# add_subdirectory'd) from src/ui/CMakeLists.txt so relative paths resolve against that file's own
# CMAKE_CURRENT_SOURCE_DIR/BINARY_DIR. Every source file is registered with
# CMAKE_CONFIGURE_DEPENDS so editing one alone triggers a reconfigure and regeneration.

# Entry i has the name at index i of _bloom_license_component_names and the files in
# _bloom_license_entry_files_<i>.
set(_bloom_license_component_names "")
function(_bloom_license_add_entry name)
    list(LENGTH _bloom_license_component_names index)
    list(APPEND _bloom_license_component_names "${name}")
    set(_bloom_license_component_names "${_bloom_license_component_names}" PARENT_SCOPE)
    set(_bloom_license_entry_files_${index} "${ARGN}" PARENT_SCOPE)
endfunction()

# The locked components. The lock names each one's license and notice files; a component the
# lock gives no license file, or names a file that is not there, stops the configure, because the
# window would ship without that license.
# Normalized, so a file another directory also registers is the same path there and here.
get_filename_component(_bloom_repository_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_bloom_lock_path "${_bloom_repository_root}/dependencies/dependencies.lock.json")
file(READ "${_bloom_lock_path}" _bloom_lock)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bloom_lock_path}")
string(JSON _bloom_lock_count LENGTH "${_bloom_lock}" components)
math(EXPR _bloom_lock_last "${_bloom_lock_count} - 1")
foreach(_bloom_i RANGE 0 ${_bloom_lock_last})
    string(JSON _bloom_lock_component GET "${_bloom_lock}" components ${_bloom_i})
    string(JSON _bloom_lock_name GET "${_bloom_lock_component}" name)
    if(_bloom_lock_name STREQUAL "qt")
        # Qt's entry is built below from its notice and license texts.
        continue()
    endif()
    set(_bloom_lock_files "")
    foreach(_bloom_lock_list IN ITEMS licenseFiles noticeFiles)
        string(JSON _bloom_lock_list_count ERROR_VARIABLE _bloom_lock_list_error
               LENGTH "${_bloom_lock_component}" license ${_bloom_lock_list})
        if(_bloom_lock_list_error OR _bloom_lock_list_count EQUAL 0)
            if(_bloom_lock_list STREQUAL "licenseFiles")
                message(FATAL_ERROR
                    "The dependency lock names no license file for ${_bloom_lock_name} -- the "
                    "Open Source Licenses window would ship without its license.")
            endif()
            continue()
        endif()
        math(EXPR _bloom_lock_list_last "${_bloom_lock_list_count} - 1")
        foreach(_bloom_j RANGE 0 ${_bloom_lock_list_last})
            string(JSON _bloom_lock_file GET "${_bloom_lock_component}"
                   license ${_bloom_lock_list} ${_bloom_j} path)
            if(NOT EXISTS "${_bloom_repository_root}/${_bloom_lock_file}")
                message(FATAL_ERROR
                    "${_bloom_lock_file}, which the dependency lock names for "
                    "${_bloom_lock_name}, does not exist -- the Open Source Licenses window "
                    "would ship without it.")
            endif()
            list(APPEND _bloom_lock_files "${_bloom_repository_root}/${_bloom_lock_file}")
        endforeach()
    endforeach()
    _bloom_license_add_entry("${_bloom_lock_name}" ${_bloom_lock_files})
endforeach()

# Everything else that is vendored rather than locked: one LICENSE file per folder. A folder of
# a locked component is already listed above; its name can differ from the lock's only by
# punctuation.
file(GLOB _bloom_dependency_license_dirs LIST_DIRECTORIES true
    "${_bloom_repository_root}/dependencies/licenses/*")
file(GLOB _bloom_media_license_dirs LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/../media/third_party/*")
file(GLOB _bloom_kit_license_dirs LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/kit/third_party/*")
set(_bloom_license_listed_keys "")
foreach(_bloom_component_name IN LISTS _bloom_license_component_names)
    string(REGEX REPLACE "[^a-z0-9]" "" _bloom_license_key "${_bloom_component_name}")
    list(APPEND _bloom_license_listed_keys "${_bloom_license_key}")
endforeach()
foreach(_bloom_license_dir IN LISTS _bloom_dependency_license_dirs _bloom_media_license_dirs
                                    _bloom_kit_license_dirs)
    if(IS_DIRECTORY "${_bloom_license_dir}" AND EXISTS "${_bloom_license_dir}/LICENSE")
        get_filename_component(_bloom_component_name "${_bloom_license_dir}" NAME)
        string(REGEX REPLACE "[^a-z0-9]" "" _bloom_license_key "${_bloom_component_name}")
        if(_bloom_license_key IN_LIST _bloom_license_listed_keys)
            continue()
        endif()
        list(APPEND _bloom_license_listed_keys "${_bloom_license_key}")
        _bloom_license_add_entry("${_bloom_component_name}" "${_bloom_license_dir}/LICENSE")
    endif()
endforeach()

# The Qt entry: the locked Qt's notice file (dependencies/licenses/qt/NOTICE.txt, generated from
# the bills of materials inside the official Qt binaries: the Qt libraries and plugins carried,
# every third-party component inside them, and the exact source archive) followed by the text of
# every license it names, the LGPL and the GPL it supplements first. They are joined into one
# generated text file and routed through the same hex-embed path as every other entry.
set(_bloom_qt_license_root "${CMAKE_CURRENT_SOURCE_DIR}/../../dependencies/licenses/qt")
if(NOT EXISTS "${_bloom_qt_license_root}/NOTICE.txt"
   OR NOT EXISTS "${_bloom_qt_license_root}/texts/LGPL-3.0-only.txt"
   OR NOT EXISTS "${_bloom_qt_license_root}/texts/GPL-3.0-only.txt")
    message(FATAL_ERROR
        "dependencies/licenses/qt has no NOTICE.txt or no LGPL and GPL texts -- the Open Source "
        "Licenses window would ship without Qt's license. Run tools/quality/generate_qt_notices.")
endif()
file(GLOB _bloom_qt_license_texts "${_bloom_qt_license_root}/texts/*.txt")
list(SORT _bloom_qt_license_texts)
list(REMOVE_ITEM _bloom_qt_license_texts
    "${_bloom_qt_license_root}/texts/LGPL-3.0-only.txt"
    "${_bloom_qt_license_root}/texts/GPL-3.0-only.txt")
list(PREPEND _bloom_qt_license_texts
    "${_bloom_qt_license_root}/texts/LGPL-3.0-only.txt"
    "${_bloom_qt_license_root}/texts/GPL-3.0-only.txt")
file(READ "${_bloom_qt_license_root}/NOTICE.txt" _bloom_qt_notice_text)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_bloom_qt_license_root}/NOTICE.txt")
set(_bloom_qt_notice_path "${CMAKE_CURRENT_BINARY_DIR}/generated/qt6-notice.txt")
file(WRITE "${_bloom_qt_notice_path}" "${_bloom_qt_notice_text}")
foreach(_bloom_qt_license_text IN LISTS _bloom_qt_license_texts)
    get_filename_component(_bloom_qt_license_name "${_bloom_qt_license_text}" NAME)
    file(READ "${_bloom_qt_license_text}" _bloom_qt_license_body)
    file(APPEND "${_bloom_qt_notice_path}"
        "\n\n==== texts/${_bloom_qt_license_name} ====\n\n" "${_bloom_qt_license_body}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bloom_qt_license_text}")
endforeach()
_bloom_license_add_entry("Qt 6" "${_bloom_qt_notice_path}")

# A Linux package carries a few distribution libraries beside Qt
# (dependencies/linux-runtime/packages.json): each one's own copyright file, under the name of
# the package it came from.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_bloom_linux_runtime_root "${CMAKE_CURRENT_SOURCE_DIR}/../../dependencies/linux-runtime")
    file(READ "${_bloom_linux_runtime_root}/packages.json" _bloom_linux_runtime_inventory)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_bloom_linux_runtime_root}/packages.json")
    string(JSON _bloom_linux_runtime_count LENGTH "${_bloom_linux_runtime_inventory}" packages)
    math(EXPR _bloom_linux_runtime_last "${_bloom_linux_runtime_count} - 1")
    foreach(_bloom_i RANGE 0 ${_bloom_linux_runtime_last})
        string(JSON _bloom_linux_runtime_package GET "${_bloom_linux_runtime_inventory}"
               packages ${_bloom_i} package)
        string(JSON _bloom_linux_runtime_copyright GET "${_bloom_linux_runtime_inventory}"
               packages ${_bloom_i} copyright path)
        set(_bloom_linux_runtime_file
            "${CMAKE_CURRENT_SOURCE_DIR}/../../${_bloom_linux_runtime_copyright}")
        if(NOT EXISTS "${_bloom_linux_runtime_file}")
            message(FATAL_ERROR
                "${_bloom_linux_runtime_copyright} does not exist -- the Open Source Licenses "
                "window would ship without the license of ${_bloom_linux_runtime_package}.")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_bloom_linux_runtime_file}")
        _bloom_license_add_entry("${_bloom_linux_runtime_package}" "${_bloom_linux_runtime_file}")
    endforeach()
endif()

list(LENGTH _bloom_license_component_names _bloom_license_component_count)
if(_bloom_license_component_count EQUAL 0)
    message(FATAL_ERROR
        "No third-party license files discovered in the dependency lock or under the license "
        "roots -- the Open Source Licenses window would ship empty. This means the license "
        "roots moved or are missing; fix the roots or this script.")
endif()

set(_bloom_catalog_body "")
string(APPEND _bloom_catalog_body
    "// GENERATED FILE -- produced by src/ui/generate_third_party_license_catalog.cmake at CMake\n"
    "// configure time. Do not edit by hand; edit the source LICENSE files or this script instead.\n"
    "#include <bloom/ui/third_party_license_catalog.hpp>\n"
    "\n"
    "namespace bloom::ui {\n"
    "namespace {\n"
    "\n")

set(_bloom_catalog_entries "")
math(EXPR _bloom_license_last_index "${_bloom_license_component_count} - 1")
foreach(_bloom_i RANGE 0 ${_bloom_license_last_index})
    list(GET _bloom_license_component_names ${_bloom_i} _bloom_component_name)
    set(_bloom_component_files ${_bloom_license_entry_files_${_bloom_i}})
    list(LENGTH _bloom_component_files _bloom_component_file_count)

    # One file is embedded as it is. Several are embedded one after the other, each under a line
    # naming it; the bytes of every file stay exactly as they are on disk.
    set(_bloom_hex "")
    foreach(_bloom_component_file IN LISTS _bloom_component_files)
        if(_bloom_component_file_count GREATER 1)
            get_filename_component(_bloom_component_file_name "${_bloom_component_file}" NAME)
            set(_bloom_heading "==== ${_bloom_component_file_name} ====\n\n")
            if(NOT _bloom_hex STREQUAL "")
                string(PREPEND _bloom_heading "\n\n")
            endif()
            string(HEX "${_bloom_heading}" _bloom_heading_hex)
            string(APPEND _bloom_hex "${_bloom_heading_hex}")
        endif()
        file(READ "${_bloom_component_file}" _bloom_file_hex HEX)
        string(APPEND _bloom_hex "${_bloom_file_hex}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bloom_component_file}")
    endforeach()
    string(LENGTH "${_bloom_hex}" _bloom_hex_length)
    math(EXPR _bloom_byte_count "${_bloom_hex_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bloom_bytes "${_bloom_hex}")

    string(REPLACE "\\" "\\\\" _bloom_name_escaped "${_bloom_component_name}")
    string(REPLACE "\"" "\\\"" _bloom_name_escaped "${_bloom_name_escaped}")

    string(APPEND _bloom_catalog_body
        "constexpr unsigned char kLicenseBytes${_bloom_i}[] = { ${_bloom_bytes} };\n")
    string(APPEND _bloom_catalog_entries
        "    { std::string_view(\"${_bloom_name_escaped}\"), "
        "std::string_view(reinterpret_cast<const char*>(kLicenseBytes${_bloom_i}), "
        "${_bloom_byte_count}) },\n")
endforeach()

string(APPEND _bloom_catalog_body
    "\n"
    "const ThirdPartyLicenseEntry kEntries[] = {\n"
    "${_bloom_catalog_entries}"
    "};\n"
    "\n"
    "} // namespace\n"
    "\n"
    "std::span<const ThirdPartyLicenseEntry> thirdPartyLicenseCatalog() {\n"
    "    return kEntries;\n"
    "}\n"
    "\n"
    "} // namespace bloom::ui\n")

set(BLOOM_THIRD_PARTY_LICENSE_CATALOG_CPP
    "${CMAKE_CURRENT_BINARY_DIR}/generated/third_party_license_catalog.generated.cpp")
file(WRITE "${BLOOM_THIRD_PARTY_LICENSE_CATALOG_CPP}" "${_bloom_catalog_body}")

# Reconfigure (and so regenerate) whenever a source license file -- registered above -- or this
# script changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_third_party_license_catalog.cmake")
# A file named twice (the lock is also read elsewhere in this directory) must be listed once.
get_property(_bloom_license_configure_depends DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
list(REMOVE_DUPLICATES _bloom_license_configure_depends)
set_property(DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS ${_bloom_license_configure_depends})

# Generates bloom::ui::thirdPartyLicenseCatalog() (task U2, issue #118, decision 3) from the real,
# checked-in license records -- never hand-maintained -- so the "Open Source Licenses..." window
# always lists exactly what this build actually ships:
#
#   * every dependencies/licenses/<component>/LICENSE  (the qualified-prefix superbuild libraries)
#   * every src/ui/kit/third_party/<component>/LICENSE (the vendored icon/font assets)
#   * the locked Qt's notice (dependencies/licenses/qt/NOTICE.txt) with the text of every license
#     it names (dependencies/licenses/qt/texts) -- see
#     docs/decisions/0014-apache-license-and-qt-distribution.md
#
# Technique: mirrors src/color/ocio_builtin_payload.inc.in's file(READ ... HEX) -> "0x..," byte
# array embed, generalized from one frozen asset to N discovered files, because license text has
# no fixed byte count to assert against. Runs at CMake configure time; included (not
# add_subdirectory'd) from src/ui/CMakeLists.txt so relative paths resolve against that file's own
# CMAKE_CURRENT_SOURCE_DIR/BINARY_DIR. Every source LICENSE file is registered with
# CMAKE_CONFIGURE_DEPENDS so editing one alone triggers a reconfigure and regeneration.

set(_bloom_license_component_names "")
set(_bloom_license_component_files "")

file(GLOB _bloom_dependency_license_dirs LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/../../dependencies/licenses/*")
file(GLOB _bloom_kit_license_dirs LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/kit/third_party/*")

foreach(_bloom_license_dir IN LISTS _bloom_dependency_license_dirs _bloom_kit_license_dirs)
    if(IS_DIRECTORY "${_bloom_license_dir}" AND EXISTS "${_bloom_license_dir}/LICENSE")
        get_filename_component(_bloom_component_name "${_bloom_license_dir}" NAME)
        list(APPEND _bloom_license_component_names "${_bloom_component_name}")
        list(APPEND _bloom_license_component_files "${_bloom_license_dir}/LICENSE")
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
list(APPEND _bloom_license_component_names "Qt 6")
list(APPEND _bloom_license_component_files "${_bloom_qt_notice_path}")

list(LENGTH _bloom_license_component_names _bloom_license_component_count)
if(_bloom_license_component_count EQUAL 0)
    message(FATAL_ERROR
        "No third-party license files discovered under dependencies/licenses/*/LICENSE or "
        "src/ui/kit/third_party/*/LICENSE -- the Open Source Licenses window would ship empty. "
        "This means the license roots moved or are missing; fix the roots or this script.")
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
    list(GET _bloom_license_component_files ${_bloom_i} _bloom_component_file)

    file(READ "${_bloom_component_file}" _bloom_hex HEX)
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

# Reconfigure (and so regenerate) whenever a source LICENSE file -- or this script -- changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_bloom_license_component_files}
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_third_party_license_catalog.cmake")

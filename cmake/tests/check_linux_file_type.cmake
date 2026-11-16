# The Linux file type files and the macOS document type agree (checked-in text, so the test runs
# on every platform).
#
#   cmake -DLINUX_DIR=<apps/bloom/linux> -DINFO_PLIST=<apps/bloom/macos/Info.plist.in>
#         -P check_linux_file_type.cmake
cmake_minimum_required(VERSION 3.25)

if(NOT LINUX_DIR OR NOT INFO_PLIST)
    message(FATAL_ERROR "check_linux_file_type.cmake needs LINUX_DIR and INFO_PLIST")
endif()
file(READ "${LINUX_DIR}/id.afterlab.bloom.xml" mime_xml)
file(READ "${LINUX_DIR}/id.afterlab.bloom.desktop" desktop)
file(READ "${INFO_PLIST}" plist)
set(failures "")

# The description: one type, one glob, the application icon.
if(NOT mime_xml MATCHES "<mime-type type=\"([^\"]+)\"")
    list(APPEND failures "the MIME description has no mime-type")
else()
    set(mime_type "${CMAKE_MATCH_1}")
endif()
if(NOT mime_xml MATCHES "<glob pattern=\"\\*\\.([A-Za-z0-9]+)\"")
    list(APPEND failures "the MIME description has no glob pattern for an extension")
else()
    set(extension "${CMAKE_MATCH_1}")
endif()
if(NOT mime_xml MATCHES "<icon name=\"id\\.afterlab\\.bloom\"")
    list(APPEND failures "the MIME description does not name the application icon")
endif()

# The desktop entry names the same type and passes the file.
if(mime_type)
    string(FIND "${desktop}" "\nMimeType=${mime_type};\n" at)
    if(at EQUAL -1)
        list(APPEND failures "the desktop entry has no 'MimeType=${mime_type};' line")
    endif()
endif()
if(NOT desktop MATCHES "\nExec=[^\n]* %f\n")
    list(APPEND failures "the desktop entry's Exec line does not end in %f")
endif()

# The macOS document type uses the same extension.
if(extension)
    if(NOT plist MATCHES "public\\.filename-extension</key>[ \t\r\n]*<array>[ \t\r\n]*<string>${extension}</string>")
        list(APPEND failures "Info.plist.in does not declare the extension '${extension}'")
    endif()
endif()

if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "the Linux file type is inconsistent:\n  ${text}")
endif()
message(STATUS "the MIME type ${mime_type} and the extension .${extension} agree")

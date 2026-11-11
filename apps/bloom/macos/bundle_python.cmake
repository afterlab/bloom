# Puts the locked CPython inside Bloom.app, so scripting starts on a Mac that has no Python.
#
#   cmake -DBUNDLE=<Bloom.app> -DPYTHON_HOME=<dependency prefix>/python
#         -DPACKAGE_DIR=<build tree>/python -DLICENSES=<source tree>/dependencies/licenses
#         -P bundle_python.cmake
#
#   Contents/Frameworks/libpython3.X.dylib          the interpreter, found through the executable's
#                                                   @executable_path/../Frameworks search path
#   Contents/Resources/python/lib/python3.X         the standard library and its extension modules
#   Contents/Resources/python/licenses              the license texts of CPython and of the
#                                                   libraries linked into its modules
#   Contents/Resources/bloom-python/bloom           Bloom's own Python package
#
# runtime.cpp gives the interpreter Contents/Resources/python as its home. Nothing is rewritten:
# the libraries keep the signature the linker gave them.

foreach(argument IN ITEMS BUNDLE PYTHON_HOME PACKAGE_DIR LICENSES)
    if(NOT ${argument} OR NOT IS_DIRECTORY "${${argument}}")
        message(FATAL_ERROR "bundle_python.cmake: ${argument} is not a directory: '${${argument}}'")
    endif()
endforeach()

file(GLOB libraries "${PYTHON_HOME}/lib/python3.*")
list(FILTER libraries EXCLUDE REGEX "\\.dylib$")
list(LENGTH libraries count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "bundle_python.cmake: expected one standard library under "
                        "${PYTHON_HOME}/lib, found: ${libraries}")
endif()
list(GET libraries 0 library)
get_filename_component(library_name "${library}" NAME)
set(interpreter "${PYTHON_HOME}/lib/lib${library_name}.dylib")
if(NOT EXISTS "${interpreter}" OR NOT EXISTS "${library}/os.py")
    message(FATAL_ERROR "bundle_python.cmake: ${PYTHON_HOME} is not a complete CPython "
                        "(${interpreter}, ${library}/os.py)")
endif()
if(NOT IS_DIRECTORY "${PACKAGE_DIR}/bloom")
    message(FATAL_ERROR "bundle_python.cmake: no Bloom package in ${PACKAGE_DIR}")
endif()

set(home "${BUNDLE}/Contents/Resources/python")
file(REMOVE_RECURSE "${home}" "${BUNDLE}/Contents/Resources/bloom-python")

# Left out: compiled caches and the files for building extensions against this interpreter, the
# Tk programs (there is no _tkinter), pip's installer (the bundled interpreter installs nothing)
# and the empty third-party directory.
file(COPY "${library}" DESTINATION "${home}/lib"
     PATTERN "__pycache__" EXCLUDE
     PATTERN "config-3.*" EXCLUDE
     PATTERN "site-packages" EXCLUDE
     PATTERN "ensurepip" EXCLUDE
     PATTERN "idlelib" EXCLUDE
     PATTERN "tkinter" EXCLUDE
     PATTERN "turtledemo" EXCLUDE
     PATTERN "turtle.py" EXCLUDE)
file(COPY "${interpreter}" DESTINATION "${BUNDLE}/Contents/Frameworks")
file(COPY "${PACKAGE_DIR}/bloom" DESTINATION "${BUNDLE}/Contents/Resources/bloom-python"
     PATTERN "__pycache__" EXCLUDE)

# CPython, then what its modules link statically: OpenSSL (ssl, hashlib), liblzma (lzma),
# Zstandard (compression.zstd) and Expat (pyexpat).
foreach(component IN ITEMS python openssl xz zstd expat)
    file(GLOB texts "${LICENSES}/${component}/LICENSE*" "${LICENSES}/${component}/COPYING*"
                    "${LICENSES}/${component}/HACL-LICENSE"
                    "${LICENSES}/${component}/incorporated-software.rst")
    if(NOT texts)
        message(FATAL_ERROR "bundle_python.cmake: no license text for ${component} in ${LICENSES}")
    endif()
    file(COPY ${texts} DESTINATION "${home}/licenses/${component}")
endforeach()
message(STATUS "Bloom.app carries ${library_name}")

# Puts the locked CPython inside Bloom.app, so scripting starts on a Mac that has no Python.
#
#   cmake -DBUNDLE=<Bloom.app> -DPYTHON_HOME=<dependency prefix>/python
#         -DPACKAGE_DIR=<build tree>/python -DLICENSES=<source tree>/dependencies/licenses
#         -P bundle_python.cmake
#
#   Contents/Frameworks/libpython3.X.dylib          the interpreter, found through the executable's
#                                                   @executable_path/../Frameworks search path
#   Contents/Resources/python/lib/python3.X         the standard library, its extension modules
#                                                   and the bundled packages (site-packages)
#   Contents/Resources/python/licenses              the license texts of CPython, of the
#                                                   libraries linked into its modules and of
#                                                   the bundled packages
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

# Left out: compiled caches and the files for building extensions against this interpreter. (The
# parts of the standard library that cannot work in the carried interpreter are already removed
# from the prefix; see dependencies/superbuild/projects/python-install.cmake.)
file(COPY "${library}" DESTINATION "${home}/lib"
     PATTERN "__pycache__" EXCLUDE
     PATTERN "config-3.*" EXCLUDE)
if(NOT EXISTS "${home}/lib/${library_name}/site-packages/numpy/__init__.py")
    message(FATAL_ERROR "bundle_python.cmake: ${PYTHON_HOME} has no numpy, the bundled package "
                        "set's one member")
endif()
file(COPY "${interpreter}" DESTINATION "${BUNDLE}/Contents/Frameworks")
file(COPY "${PACKAGE_DIR}/bloom" DESTINATION "${BUNDLE}/Contents/Resources/bloom-python"
     PATTERN "__pycache__" EXCLUDE)

# CPython, then what its modules link statically: OpenSSL (ssl, hashlib), liblzma (lzma),
# Zstandard (compression.zstd), zlib, bzip2 (bz2), SQLite (sqlite3), libffi (ctypes) and Expat
# (pyexpat); then the bundled packages. Every file of a component's record directory is a license
# or notice text except the intake records and the archive signatures.
foreach(component IN ITEMS python openssl xz zstd zlib bzip2 sqlite libffi expat numpy)
    file(GLOB texts "${LICENSES}/${component}/*")
    list(FILTER texts EXCLUDE REGEX
         "/(provenance|review|security)\\.md$|\\.(asc|sig|sigstore|provenance\\.json)$")
    if(NOT texts)
        message(FATAL_ERROR "bundle_python.cmake: no license text for ${component} in ${LICENSES}")
    endif()
    file(COPY ${texts} DESTINATION "${home}/licenses/${component}")
endforeach()
message(STATUS "Bloom.app carries ${library_name}")

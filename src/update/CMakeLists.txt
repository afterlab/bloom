# ADR 0041, #734: the self-update core. Qt-free: the signed update feed and its reader, Ed25519
# and checksum verification of downloaded files, the check policy, and how this copy of Bloom was
# installed. The application (src/ui, apps/bloom) adds the transport, the dialogs and the platform
# updaters; the release tools (tools/release) write what this module reads.
bloom_find_dependency(yyjson)

# Where a build looks for updates and whose signatures it accepts. Empty in a developer build,
# which then says updates are not configured and contacts nothing; a release build is given them
# by the release workflow. The source tree carries no address and no key.
set(BLOOM_UPDATE_FEED_URL "" CACHE STRING
    "The HTTPS address of the update feed this build reads (empty: updates are not configured)")
set(BLOOM_UPDATE_PUBLIC_KEYS "" CACHE STRING
    "The Ed25519 public keys this build accepts updates from: Base64 of each key's 32 bytes, separated by commas")
set(BLOOM_UPDATE_APPIMAGE_INFORMATION "" CACHE STRING
    "The update information an AppImage of this build carries")
option(BLOOM_UPDATE_TEST_BUILD
       "A build for the update tests: it may read a feed on this machine with a test key, and the package check refuses to publish it"
       OFF)
if(BLOOM_UPDATE_FEED_URL AND NOT BLOOM_UPDATE_TEST_BUILD
   AND NOT BLOOM_UPDATE_FEED_URL MATCHES "^https://[^/ ]+/")
    message(FATAL_ERROR "BLOOM_UPDATE_FEED_URL must be an HTTPS address")
endif()
if(BLOOM_UPDATE_FEED_URL AND NOT BLOOM_UPDATE_PUBLIC_KEYS)
    message(FATAL_ERROR "BLOOM_UPDATE_FEED_URL is set without BLOOM_UPDATE_PUBLIC_KEYS")
endif()
if(BLOOM_UPDATE_PUBLIC_KEYS
   AND NOT BLOOM_UPDATE_PUBLIC_KEYS MATCHES "^[A-Za-z0-9+/]+=(,[A-Za-z0-9+/]+=)*$")
    message(FATAL_ERROR
        "BLOOM_UPDATE_PUBLIC_KEYS must be Base64 keys of 32 bytes, separated by commas")
endif()
foreach(bloom_update_setting IN ITEMS BLOOM_UPDATE_FEED_URL BLOOM_UPDATE_PUBLIC_KEYS
                                      BLOOM_UPDATE_APPIMAGE_INFORMATION)
    if("${${bloom_update_setting}}" MATCHES "[\"\\\\\n;]")
        message(FATAL_ERROR "${bloom_update_setting} holds a character a setting cannot")
    endif()
endforeach()

# Signatures and checksums are OpenSSL's (ADR 0041): Ed25519 through EVP_DigestVerify and SHA-256
# through EVP_sha256, from the locked libcrypto that the dependency prefix builds for the carried
# Python interpreter (dependencies/superbuild/projects/openssl.cmake). Linux and macOS have it as
# a static library, which is linked into the programs. Windows has it as a library file beside
# the interpreter's modules; the programs import it late and load it by its full name from there
# (crypto_library.cpp), so no second copy is carried and nothing is taken from the search path.
# A developer build without the prefix uses the machine's OpenSSL 3.
add_library(bloom_update_crypto INTERFACE)
set(bloom_update_crypto_support "${BLOOM_DEPENDENCY_PREFIX}/python-support")
set(bloom_update_crypto_build_directory "")
if(WIN32 AND BLOOM_DEPENDENCY_PREFIX
   AND EXISTS "${bloom_update_crypto_support}/openssl/lib/libcrypto.lib")
    target_include_directories(bloom_update_crypto SYSTEM INTERFACE
        "${bloom_update_crypto_support}/openssl/include")
    target_link_libraries(bloom_update_crypto INTERFACE
        "${bloom_update_crypto_support}/openssl/lib/libcrypto.lib" delayimp)
    target_link_options(bloom_update_crypto INTERFACE "/DELAYLOAD:libcrypto-3-x64.dll")
    # Where a program run from the build folder finds the library file: the interpreter's
    # library folder of the prefix, the same file a package carries.
    set(bloom_update_crypto_build_directory "${BLOOM_DEPENDENCY_PREFIX}/python/DLLs")
    if(NOT EXISTS "${bloom_update_crypto_build_directory}/libcrypto-3-x64.dll")
        message(FATAL_ERROR
            "Bloom update: ${bloom_update_crypto_build_directory}/libcrypto-3-x64.dll is "
            "missing; the dependency prefix's interpreter carries it.")
    endif()
    message(STATUS "Bloom update: the locked libcrypto of the carried interpreter, loaded late")
elseif(NOT WIN32 AND BLOOM_DEPENDENCY_PREFIX
       AND EXISTS "${bloom_update_crypto_support}/lib/libcrypto.a")
    find_package(Threads REQUIRED)
    target_include_directories(bloom_update_crypto SYSTEM INTERFACE
        "${bloom_update_crypto_support}/include")
    target_link_libraries(bloom_update_crypto INTERFACE
        "${bloom_update_crypto_support}/lib/libcrypto.a" Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT APPLE)
        # The interpreter's modules hold the same library; a program must not offer its copy's
        # names to them.
        target_link_options(bloom_update_crypto INTERFACE "LINKER:--exclude-libs,libcrypto.a")
    endif()
    message(STATUS "Bloom update: the locked libcrypto, linked statically")
else()
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    target_link_libraries(bloom_update_crypto INTERFACE OpenSSL::Crypto)
    message(STATUS "Bloom update: the machine's OpenSSL ${OPENSSL_VERSION} (no locked libcrypto "
                   "in a dependency prefix)")
endif()

# Applying a prepared update: standard library and operating system calls only. The update
# helper (apps/bloom-update-helper) compiles these files itself, so that it links nothing of
# Bloom's and can be built against the static C++ runtime on Windows.
set(BLOOM_UPDATE_APPLY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/apply.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/apply_plan.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/apply_system_host.cpp"
    CACHE INTERNAL "The sources of the update helper's steps")
set(BLOOM_UPDATE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
    CACHE INTERNAL "The public include directory of src/update")

add_library(bloom_update STATIC
    ${BLOOM_UPDATE_APPLY_SOURCES}
    base64.cpp
    built_in_configuration.cpp
    check.cpp
    check_policy.cpp
    configuration.cpp
    crypto_library.cpp
    crypto_library.hpp
    ed25519.cpp
    feed.cpp
    file_verification.cpp
    install_kind.cpp
    keys.cpp
    mapped_file.cpp
    mapped_file.hpp
    refusal.cpp
    release_version.cpp
    sha256.cpp
    staging.cpp
    zsync.cpp
)
target_sources(bloom_update PUBLIC FILE_SET HEADERS BASE_DIRS include
    FILES include/bloom/update/apply.hpp
          include/bloom/update/base64.hpp
          include/bloom/update/check.hpp
          include/bloom/update/check_policy.hpp
          include/bloom/update/configuration.hpp
          include/bloom/update/ed25519.hpp
          include/bloom/update/feed.hpp
          include/bloom/update/file_verification.hpp
          include/bloom/update/install_kind.hpp
          include/bloom/update/keys.hpp
          include/bloom/update/refusal.hpp
          include/bloom/update/release_version.hpp
          include/bloom/update/sha256.hpp
          include/bloom/update/staging.hpp
          include/bloom/update/transport.hpp
          include/bloom/update/zsync.hpp)
target_compile_features(bloom_update PUBLIC cxx_std_20)
target_link_libraries(bloom_update PUBLIC bloom_core
    PRIVATE bloom_platform bloom_update_crypto yyjson::yyjson)
bloom_enable_warnings(bloom_update)
if(bloom_update_crypto_build_directory)
    # A path in a string literal: forward slashes, never the backslashes of a runner's spelling.
    bloom_path_definition(bloom_update_crypto_build_definition
        BLOOM_UPDATE_CRYPTO_BUILD_DIRECTORY "${bloom_update_crypto_build_directory}")
    set_source_files_properties(crypto_library.cpp PROPERTIES COMPILE_DEFINITIONS
        "${bloom_update_crypto_build_definition}")
endif()
# The settings reach one file, so a build under other settings recompiles that file alone.
if(BLOOM_UPDATE_TEST_BUILD)
    set(bloom_update_test_build 1)
else()
    set(bloom_update_test_build 0)
endif()
set_source_files_properties(built_in_configuration.cpp PROPERTIES COMPILE_DEFINITIONS
    "BLOOM_UPDATE_FEED_URL=\"${BLOOM_UPDATE_FEED_URL}\";BLOOM_UPDATE_PUBLIC_KEYS=\"${BLOOM_UPDATE_PUBLIC_KEYS}\";BLOOM_UPDATE_APPIMAGE_INFORMATION=\"${BLOOM_UPDATE_APPIMAGE_INFORMATION}\";BLOOM_UPDATE_TEST_BUILD=${bloom_update_test_build}")

# The writer of an AppImage's delta control file: a build tool of the release
# (cmake/package/make_archives.cmake), not part of any package.
add_executable(bloom-zsync-make zsync_make_main.cpp)
target_link_libraries(bloom-zsync-make PRIVATE bloom_update)
bloom_enable_warnings(bloom-zsync-make)

# Ed25519 with the same libcrypto, for the release tools where the machine's `openssl` program is
# not OpenSSL 3 (macOS): their tests and the end-to-end update test sign stand-in releases with
# it. A build tool; no package carries it and no release is signed with it.
add_executable(bloom-ed25519-tool ed25519_tool_main.cpp)
target_link_libraries(bloom-ed25519-tool PRIVATE bloom_update bloom_update_crypto)
bloom_enable_warnings(bloom-ed25519-tool)

if(BUILD_TESTING)
    include(BloomTesting)

    set(bloom_update_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    foreach(bloom_update_test IN ITEMS ed25519 feed verify policy install_kind helper staging zsync)
        add_executable(bloom_update_${bloom_update_test}_test
                       tests/${bloom_update_test}_tests.cpp)
        target_link_libraries(bloom_update_${bloom_update_test}_test PRIVATE bloom_update)
        target_compile_definitions(bloom_update_${bloom_update_test}_test
                                   PRIVATE BLOOM_UPDATE_FIXTURES="${bloom_update_fixtures}")
        bloom_enable_warnings(bloom_update_${bloom_update_test}_test)
        bloom_add_test(NAME bloom.update.${bloom_update_test}_tests
                       COMMAND bloom_update_${bloom_update_test}_test LABELS unit update)
    endforeach()

    # The helper as a program of its own, with a stand-in for the installed Bloom.
    add_executable(bloom_update_fake_app tests/fake_app.cpp)
    target_link_libraries(bloom_update_fake_app PRIVATE bloom_update)
    bloom_enable_warnings(bloom_update_fake_app)
    add_executable(bloom_update_helper_program_test tests/helper_program_tests.cpp)
    target_link_libraries(bloom_update_helper_program_test PRIVATE bloom_update)
    target_compile_definitions(bloom_update_helper_program_test
                               PRIVATE BLOOM_UPDATE_FIXTURES="${bloom_update_fixtures}")
    bloom_enable_warnings(bloom_update_helper_program_test)
    add_dependencies(bloom_update_helper_program_test bloom_update_fake_app)
    if(TARGET bloom-update-helper)
        add_dependencies(bloom_update_helper_program_test bloom-update-helper)
    endif()
    bloom_add_test(NAME bloom.update.helper_program_tests
                   COMMAND bloom_update_helper_program_test
                       "$<TARGET_FILE:bloom-update-helper>" "$<TARGET_FILE:bloom_update_fake_app>"
                   LABELS integration update TIMEOUT 240)
endif()

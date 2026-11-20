# bloom-crash-handler (issue #736): Crashpad's out-of-process crash handler as a Bloom program,
# built from the locked Crashpad's handler library. Every program that starts the crash reporter
# looks for it beside itself (src/crash/crash_reporter.cpp).
if(NOT BLOOM_CRASH_HAVE_CRASHPAD)
    return()
endif()
add_executable(bloom-crash-handler main.cpp)
target_link_libraries(bloom-crash-handler PRIVATE bloom_crashpad crashpad::handler crashpad::tools)
if(WIN32)
    target_link_libraries(bloom-crash-handler PRIVATE crashpad::getopt)
    # No console window beside the program it serves.
    set_target_properties(bloom-crash-handler PROPERTIES WIN32_EXECUTABLE ON)
endif()
bloom_enable_warnings(bloom-crash-handler)

# Beside the application and the tools in a package (cmake/BloomPackageLayout.cmake). The macOS
# bundle gets its copy in the build tree (apps/bloom/CMakeLists.txt); installed alone it goes to
# bin.
if(APPLE)
    install(TARGETS bloom-crash-handler RUNTIME DESTINATION bin)
else()
    install(TARGETS bloom-crash-handler RUNTIME DESTINATION "${BLOOM_PACKAGE_PROGRAM_DIR}")
    bloom_package_search_path(bloom-crash-handler "${BLOOM_PACKAGE_PROGRAM_DIR}")
endif()

# bloom-update-helper (ADR 0041): replaces an installed Bloom after it has exited. It compiles
# the steps itself (src/update's apply files, standard library and operating system calls only)
# and links nothing of Bloom's: it is started from a copy outside the installation, where none of
# the package's libraries are.
add_executable(bloom-update-helper main.cpp ${BLOOM_UPDATE_APPLY_SOURCES})
target_include_directories(bloom-update-helper PRIVATE "${BLOOM_UPDATE_INCLUDE_DIR}")
target_compile_features(bloom-update-helper PRIVATE cxx_std_20)
set_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS
    "BLOOM_UPDATE_HELPER_VERSION=\"${BLOOM_PACKAGE_VERSION}\"")
bloom_enable_warnings(bloom-update-helper)
if(MSVC)
    # The static C++ runtime, so the copy needs no runtime library beside it, and no console
    # window when Bloom starts it.
    set_target_properties(bloom-update-helper PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        WIN32_EXECUTABLE ON)
    target_link_options(bloom-update-helper PRIVATE /ENTRY:mainCRTStartup)
    target_link_libraries(bloom-update-helper PRIVATE shell32)
elseif(NOT APPLE)
    # The copy runs without the package's library directory, on the C library alone.
    target_link_options(bloom-update-helper PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Beside the color worker in a package (cmake/BloomPackageLayout.cmake), where
# bloom::update::installedHelperProgram looks. The macOS bundle gets its copy in the build tree
# (apps/bloom/CMakeLists.txt).
if(NOT APPLE)
    install(TARGETS bloom-update-helper RUNTIME DESTINATION "${BLOOM_PACKAGE_COLOR_DIR}")
endif()

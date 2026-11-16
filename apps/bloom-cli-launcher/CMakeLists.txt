# bloom-cli-launcher (Windows only): the bloom-cli.exe in the installer's `cli` folder. See
# main.cpp for what it does and why it exists. The program goes to cli/bloom-cli.exe in the
# package; the build puts it into a folder of its own so it never shares a name or a folder with
# the real bloom-cli.exe.
add_executable(bloom-cli-launcher main.cpp)
set_target_properties(bloom-cli-launcher PROPERTIES
    OUTPUT_NAME bloom-cli
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher")
target_compile_features(bloom-cli-launcher PRIVATE cxx_std_20)
bloom_enable_warnings(bloom-cli-launcher)
if(MSVC)
    # The static C++ runtime, so the program needs no runtime library beside it: nothing but
    # the program sits in the `cli` folder, and that folder is what goes into the user's PATH.
    set_target_properties(bloom-cli-launcher PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The install rule is in cmake/BloomPackage.cmake, after the Visual C++ runtime is staged, so the
# runtime libraries are not copied into the `cli` folder as well.

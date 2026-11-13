bloom_find_dependency(nanobind)

# Only native library dependencies of the extension need PIC. Do not change unrelated
# executables, tests, worker binaries or desktop libraries when enabling Python.
function(bloom_python_require_pic bloom_python_target)
    get_target_property(bloom_python_imported "${bloom_python_target}" IMPORTED)
    get_target_property(bloom_python_type "${bloom_python_target}" TYPE)
    get_target_property(bloom_python_visited "${bloom_python_target}" BLOOM_PYTHON_PIC_VISITED)
    if(bloom_python_imported OR bloom_python_visited)
        return()
    endif()
    set_property(TARGET "${bloom_python_target}" PROPERTY BLOOM_PYTHON_PIC_VISITED TRUE)
    if(bloom_python_type STREQUAL "STATIC_LIBRARY" OR bloom_python_type STREQUAL "OBJECT_LIBRARY")
        set_property(TARGET "${bloom_python_target}" PROPERTY POSITION_INDEPENDENT_CODE ON)
    endif()
    get_target_property(bloom_python_links "${bloom_python_target}" LINK_LIBRARIES)
    foreach(bloom_python_link IN LISTS bloom_python_links)
        if(TARGET "${bloom_python_link}")
            bloom_python_require_pic("${bloom_python_link}")
        endif()
    endforeach()
endfunction()
bloom_python_require_pic(bloom_scripting)

add_library(bloom_python_binding OBJECT native.cpp projections.cpp render.cpp scopes.cpp bindings.cpp task_ticket.cpp root_retirements.cpp)
target_link_libraries(bloom_python_binding PRIVATE bloom_scripting nanobind::core Python3::Module)
target_compile_features(bloom_python_binding PRIVATE cxx_std_20)
target_compile_definitions(bloom_python_binding PRIVATE BLOOM_PYTHON_VERSION="${PROJECT_VERSION}")
set_target_properties(bloom_python_binding PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
bloom_enable_warnings(bloom_python_binding)

Python3_add_library(_bloom MODULE WITH_SOABI $<TARGET_OBJECTS:bloom_python_binding>)
target_link_libraries(_bloom PRIVATE bloom_scripting nanobind::core)
set_target_properties(_bloom PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python")
file(GLOB_RECURSE bloom_python_files CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "bloom/*.py" "bloom/*.pyi" "bloom/py.typed")
foreach(bloom_python_file IN LISTS bloom_python_files)
    configure_file("${bloom_python_file}" "${CMAKE_BINARY_DIR}/python/${bloom_python_file}" COPYONLY)
endforeach()
# The extension module is for an interpreter outside Bloom (`cmake --install <build> --component
# python-module`). A package does not carry it: Bloom's programs have the same code built in, and
# cmake/package/stage_python.cmake puts the `bloom` package beside the carried interpreter.
install(TARGETS _bloom
        LIBRARY DESTINATION lib/bloom/python COMPONENT python-module EXCLUDE_FROM_ALL
        RUNTIME DESTINATION lib/bloom/python COMPONENT python-module EXCLUDE_FROM_ALL)
configure_file(_bloom.pyi "${CMAKE_BINARY_DIR}/python/_bloom.pyi" COPYONLY)
install(FILES _bloom.pyi DESTINATION lib/bloom/python COMPONENT python-module EXCLUDE_FROM_ALL)
install(DIRECTORY bloom DESTINATION lib/bloom/python COMPONENT python-module EXCLUDE_FROM_ALL
        PATTERN "__pycache__" EXCLUDE)

add_library(bloom_python_runtime STATIC runtime.cpp interpreter_home.cpp
    $<TARGET_OBJECTS:bloom_python_binding>)
target_link_libraries(bloom_python_runtime PUBLIC bloom_scripting
    PRIVATE bloom_platform nanobind::core Python3::Python)
target_include_directories(bloom_python_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(bloom_python_runtime PRIVATE
    BLOOM_PYTHON_PACKAGE_PATH="${CMAKE_BINARY_DIR}/python")
if(BLOOM_LOCKED_PYTHON_HOME)
    # CPython compiles in the directory it was installed to, which the superbuild reaches through
    # a temporary link. Programs run from the build tree are told where the interpreter is.
    target_compile_definitions(bloom_python_runtime PRIVATE
        BLOOM_PYTHON_HOME="${BLOOM_LOCKED_PYTHON_HOME}")
endif()
bloom_enable_warnings(bloom_python_runtime)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_python_root_retirements_test "${PROJECT_SOURCE_DIR}/tests/scripting/root_retirements_tests.cpp" root_retirements.cpp)
    target_include_directories(bloom_python_root_retirements_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_python_root_retirements_test PRIVATE bloom_scripting bloom_render)
    target_include_directories(bloom_python_root_retirements_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/render" "${CMAKE_CURRENT_SOURCE_DIR}")
    bloom_enable_warnings(bloom_python_root_retirements_test)
    bloom_add_test(NAME bloom.scripting.python-root-retirements
        COMMAND bloom_python_root_retirements_test
        LABELS scripting python host unit)
    add_executable(bloom_python_root_lifecycle_test "${PROJECT_SOURCE_DIR}/tests/scripting/root_lifecycle_tests.cpp")
    target_link_libraries(bloom_python_root_lifecycle_test PRIVATE
        bloom_python_runtime bloom_render nanobind::core Python3::Python)
    target_include_directories(bloom_python_root_lifecycle_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/render" "${CMAKE_CURRENT_SOURCE_DIR}")
    bloom_enable_warnings(bloom_python_root_lifecycle_test)
    bloom_add_test(NAME bloom.scripting.python-root-lifecycle
        COMMAND bloom_python_root_lifecycle_test
        LABELS scripting python host integration)
    bloom_add_test(NAME bloom.scripting.python-root-subprocess
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_root_lifecycle.py"
            "${CMAKE_BINARY_DIR}/python" $<TARGET_FILE:bloom-cli>
        LABELS scripting python host integration)
    bloom_add_test(NAME bloom.scripting.python-api
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_api.py"
            "${CMAKE_BINARY_DIR}/python" $<TARGET_FILE:bloom-cli>
        LABELS scripting python integration device
        # bloom-cli export probes the encoders and renders a saved queue end to end.
        TIMEOUT 180)
    # #650: every preset export.presets lists as available is accepted by the Python operation
    # and rendered by bloom-cli export (real encodes of two frames each). The walk runs on Linux
    # only, so it is registered there only: elsewhere it used to skip itself and count as passed.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        bloom_add_test(NAME bloom.scripting.python-export-presets
            COMMAND "${Python3_EXECUTABLE}" -I
                "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_export_presets.py"
                "${CMAKE_BINARY_DIR}/python" $<TARGET_FILE:bloom-cli>
            LABELS scripting python integration device
            TIMEOUT 600)
    endif()
    # Agent feedback: one time contract (an integer is a frame) and parameter value kinds.
    bloom_add_test(NAME bloom.scripting.python-agent-feedback
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_agent_feedback.py"
            "${CMAKE_BINARY_DIR}/python"
        LABELS scripting python integration)
    bloom_add_test(NAME bloom.scripting.python-parity-ops
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_parity_ops.py"
            "${CMAKE_BINARY_DIR}/python"
        LABELS scripting python integration)
    # #724: bloom.scopes reads tables, a summary and a picture of the Scopes panel's measure.
    bloom_add_test(NAME bloom.scripting.python-scopes
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_scopes.py"
            "${CMAKE_BINARY_DIR}/python"
        LABELS scripting python integration)
    # #357: several Hosts rendering through one interpreter device must exit promptly.
    bloom_add_test(NAME bloom.scripting.python-shared-device
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_shared_device.py"
            "${CMAKE_BINARY_DIR}/python"
        LABELS scripting python integration device)
    # #608: a scripted render through Apply LUT runs on the device, with CPU parity.
    bloom_add_test(NAME bloom.scripting.python-lut-render
        COMMAND bloom-cli run "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_lut_render.py"
        LABELS scripting python integration device TIMEOUT 300)
    bloom_add_test(NAME bloom.scripting.python-embedding
        COMMAND bloom-cli run "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/embedding.py"
        LABELS scripting python integration device)
    # The recipe in docs/user-guide/python.md, run the way an artist runs it.
    bloom_add_test(NAME bloom.scripting.python-first-five-minutes
        COMMAND bloom-cli run "${PROJECT_SOURCE_DIR}/examples/scripting/first_five_minutes.py"
        LABELS scripting python integration device)
    # The lookup of the carried interpreter inside a package (interpreter_home.hpp).
    add_executable(bloom_python_interpreter_home_test
        "${PROJECT_SOURCE_DIR}/tests/scripting/interpreter_home_tests.cpp" interpreter_home.cpp)
    target_compile_features(bloom_python_interpreter_home_test PRIVATE cxx_std_20)
    target_include_directories(bloom_python_interpreter_home_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    bloom_enable_warnings(bloom_python_interpreter_home_test)
    bloom_add_test(NAME bloom.scripting.python-interpreter-home
        COMMAND bloom_python_interpreter_home_test
        LABELS scripting python unit)
    # Issue #677: the carried interpreter has one module set and gives one result on Linux, macOS
    # and Windows. Registered only where the build embeds the locked interpreter of the dependency
    # prefix: a developer build against the host's Python (and a qualified build against a prefix
    # from before the interpreter was locked) has that host's modules and libraries, which these
    # tests would rightly fail.
    if(BLOOM_LOCKED_PYTHON_HOME)
        bloom_add_test(NAME bloom.scripting.python-module-set
            COMMAND bloom-cli run
                "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_module_set.py"
            ENVIRONMENT
                "BLOOM_PYTHON_MODULE_SET=${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_module_set.txt"
            LABELS scripting python integration)
        bloom_add_test(NAME bloom.scripting.python-same-result
            COMMAND bloom-cli run
                "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_same_result.py"
            ENVIRONMENT
                "BLOOM_PYTHON_SAME_RESULT=${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_same_result.txt"
            LABELS scripting python integration TIMEOUT 120)
        # The carried interpreter writes its compiled modules to the per-user cache, never into
        # its own home. Both variables are set: each platform reads its own.
        bloom_add_test(NAME bloom.scripting.python-bytecode-cache
            COMMAND bloom-cli run
                "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_bytecode_cache.py"
            ENVIRONMENT
                "BLOOM_PYTHON_CACHE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/bytecode-cache"
                "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/bytecode-cache"
                "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/bytecode-cache"
            LABELS scripting python integration)
    endif()
    # #343: import an SVG, move and key one point of a compound shape, read it back, undo.
    bloom_add_test(NAME bloom.scripting.python-svg-import
        COMMAND bloom-cli run "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/python_svg_import.py"
        LABELS scripting python integration)
endif()

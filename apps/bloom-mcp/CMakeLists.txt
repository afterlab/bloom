# ADR 0034: a thin composition root over the shared server core in src/mcp.
add_executable(bloom-mcp main.cpp)
target_link_libraries(bloom-mcp PRIVATE bloom_mcp)
target_compile_definitions(bloom-mcp PRIVATE BLOOM_VERSION="${PROJECT_VERSION}")
if(TARGET bloom-media-worker)
    add_dependencies(bloom-mcp bloom-media-worker)
endif()
bloom_enable_warnings(bloom-mcp)
install(TARGETS bloom-mcp RUNTIME DESTINATION bin)

# Stage the qualified GPU shader tools beside this executable so MCP-driven export shader
# compilation works out of the box; an unqualified or CPU-stub build gets a typed definition.
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom-mcp)
# macOS has no system Vulkan: package the locked loader and driver beside the executable.
bloom_package_vulkan_runtime(bloom-mcp)

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    include(BloomTesting)
    bloom_add_test(NAME bloom.scripting.mcp-pipes
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/mcp_protocol.py"
            $<TARGET_FILE:bloom-mcp> $<TARGET_FILE:bloom-cli>
        LABELS scripting mcp integration
        # Measured 21-25 s alone: 13 cases each start bloom-mcp, which enumerates the GPU (~1.2 s).
        TIMEOUT 120)
    # The node sweep (tests/fixtures/scripting/node_sweep): every registered node type with an image output rendered on
    # the GPU and on the CPU reference, compared, and held to the committed baseline for this
    # platform. The types come from the registry at run time, so a new node is swept without
    # editing the test. Without a GPU device the sweep is reported as not run, never as passed.
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" bloom_sweep_processor)
    if(bloom_sweep_processor MATCHES "^(arm64|aarch64)$")
        set(bloom_sweep_processor "arm64")
    elseif(bloom_sweep_processor MATCHES "^(x86_64|amd64)$")
        set(bloom_sweep_processor "x86_64")
    endif()
    if(APPLE)
        set(bloom_sweep_platform "macos-${bloom_sweep_processor}")
    elseif(WIN32)
        set(bloom_sweep_platform "windows-${bloom_sweep_processor}")
    else()
        set(bloom_sweep_platform "linux-${bloom_sweep_processor}")
    endif()
    set(bloom_sweep "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/node_sweep")
    bloom_add_test(NAME bloom.sweep.nodes
        COMMAND "${Python3_EXECUTABLE}" -I "${bloom_sweep}/node_sweep.py"
            --mcp $<TARGET_FILE:bloom-mcp>
            --baseline "${bloom_sweep}/baselines/${bloom_sweep_platform}.json"
        LABELS sweep gpu native integration
        # Two bloom-mcp processes per node type, about a hundred types.
        TIMEOUT 3600)
    set_tests_properties(bloom.sweep.nodes PROPERTIES SKIP_RETURN_CODE 77)
    # The sweep's own rules (an undeclared node fails, baselines disagree), without a device.
    bloom_add_test(NAME bloom.sweep.self-test
        COMMAND "${Python3_EXECUTABLE}" -I "${bloom_sweep}/node_sweep.py" --self-test
        LABELS sweep unit)
    # The platforms' baselines against each other: one CPU reference everywhere, and a GPU frame
    # that differs only for a node cross_platform.json lists.
    bloom_add_test(NAME bloom.sweep.baselines
        COMMAND "${Python3_EXECUTABLE}" -I "${bloom_sweep}/node_sweep.py"
            --compare "${bloom_sweep}/baselines/linux-x86_64.json"
                      "${bloom_sweep}/baselines/macos-arm64.json"
            --cross-platform "${bloom_sweep}/cross_platform.json"
        LABELS sweep unit)
    # Defects an agent found live-testing the MCP server, each pinned at the protocol level.
    bloom_add_test(NAME bloom.mcp.agent-feedback
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/mcp_agent_feedback.py"
            $<TARGET_FILE:bloom-mcp>
        LABELS mcp integration
        TIMEOUT 180)
    # MCP parity operations (layer delete/duplicate, layers from assets, node compound adds).
    bloom_add_test(NAME bloom.scripting.mcp-parity-ops
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/mcp_parity_ops.py"
            $<TARGET_FILE:bloom-mcp>
        LABELS scripting mcp integration
        # Takes about 11 s alone (each case starts bloom-mcp), so the default 30 s left less
        # than a threefold margin and a loaded runner timed it out. It has no timed assertions
        # of its own: under a twentyfold slowdown it passes in about 150 s (#379).
        TIMEOUT 240)
    # The agent surfaces describe themselves alike and read back what they write.
    if(BLOOM_BUILD_PYTHON)
        set(bloom_reference_python "${CMAKE_BINARY_DIR}/python")
    else()
        set(bloom_reference_python "-")
    endif()
    bloom_add_test(NAME bloom.mcp.reference
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/agent_reference.py"
            $<TARGET_FILE:bloom-mcp> $<TARGET_FILE:bloom-cli> "${bloom_reference_python}"
        LABELS scripting mcp integration
        TIMEOUT 180)
    if(BLOOM_GPU_SHADER_TOOLS_AVAILABLE)
        bloom_add_test(
            NAME bloom.gpu.shader-tools-verify-mcp
            COMMAND "${CMAKE_COMMAND}"
                "-DGPU_TOOLS_STAGED=$<TARGET_FILE_DIR:bloom-mcp>/bloom-gpu-tools"
                "-DGPU_TOOLS_PREFIX_BIN=${BLOOM_DEPENDENCY_PREFIX}/bin"
                "-DGPU_TOOLS_GLSLANG=glslangValidator${CMAKE_EXECUTABLE_SUFFIX}"
                "-DGPU_TOOLS_SPIRV=spirv-val${CMAKE_EXECUTABLE_SUFFIX}"
                -P "${PROJECT_SOURCE_DIR}/cmake/tests/verify_gpu_shader_tools.cmake"
            LABELS gpu unit)
    endif()
endif()

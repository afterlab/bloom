# ADR 0034: the shared Qt-free MCP server core (JSON-RPC, handshake, tools, schemas, projections,
# stdio transport), Streamable HTTP framing and admission, and the in-app server service.
bloom_find_dependency(yyjson)
add_library(bloom_mcp STATIC)

# The user guides the resources serve, embedded so an installed server never reads the source tree.
set(bloom_mcp_guide_names mcp scripting python)
set(bloom_mcp_guide_files
    "${PROJECT_SOURCE_DIR}/docs/user-guide/mcp.md"
    "${PROJECT_SOURCE_DIR}/docs/user-guide/scripting.md"
    "${PROJECT_SOURCE_DIR}/docs/user-guide/python.md")
set(bloom_mcp_guides_source "${CMAKE_CURRENT_BINARY_DIR}/embedded_guides.cpp")
string(REPLACE ";" "|" bloom_mcp_guide_name_arg "${bloom_mcp_guide_names}")
string(REPLACE ";" "|" bloom_mcp_guide_file_arg "${bloom_mcp_guide_files}")
add_custom_command(
    OUTPUT "${bloom_mcp_guides_source}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${bloom_mcp_guides_source}"
        "-DNAMES=${bloom_mcp_guide_name_arg}" "-DFILES=${bloom_mcp_guide_file_arg}"
        -P "${PROJECT_SOURCE_DIR}/cmake/BloomEmbedText.cmake"
    DEPENDS ${bloom_mcp_guide_files} "${PROJECT_SOURCE_DIR}/cmake/BloomEmbedText.cmake"
    COMMENT "Embedding the MCP user guides"
    VERBATIM)

target_sources(
    bloom_mcp
    PRIVATE
        core_internal.hpp
        embedded_guides.hpp
        "${bloom_mcp_guides_source}"
        frame_statistics.cpp
        frame_statistics.hpp
        json.cpp
        json.hpp
        projections.cpp
        server_core.cpp
        server_http.cpp
        server_service.cpp
        server_service_state.hpp
        stdio_transport.cpp
        streamable_http.cpp
        tool_schemas.cpp
        tools_document.cpp
        tools_profiler.cpp
        tools_project.cpp
        tools_export.cpp
        tools_inspection.cpp
        tools_file_session.cpp
        tools_readback.cpp
        tools_reference.cpp
        tools_render.cpp
        tools_scopes.cpp
        tools_session.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/mcp/live_ui_port.hpp
            include/bloom/mcp/protocol.hpp
            include/bloom/mcp/server_core.hpp
            include/bloom/mcp/server_service.hpp
            include/bloom/mcp/streamable_http.hpp
)

target_compile_features(bloom_mcp PUBLIC cxx_std_20)
target_link_libraries(bloom_mcp PUBLIC bloom_scripting bloom_host bloom_platform bloom_core)
target_link_libraries(bloom_mcp PRIVATE yyjson::yyjson)
target_include_directories(bloom_mcp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
bloom_enable_warnings(bloom_mcp)

# Test support: a driver that scripts a real McpServerService into every state the MCP Server panel
# shows (never linked into production).
add_library(bloom_mcp_testing STATIC testing/server_service_driver.cpp)
target_include_directories(bloom_mcp_testing PUBLIC testing/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bloom_mcp_testing PUBLIC bloom_mcp)
bloom_enable_warnings(bloom_mcp_testing)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_mcp_server_contract_test tests/mcp_server_contract_tests.cpp)
    target_link_libraries(bloom_mcp_server_contract_test PRIVATE bloom_mcp)
    bloom_enable_warnings(bloom_mcp_server_contract_test)
    bloom_add_test(
        NAME bloom.mcp.server-contract
        COMMAND bloom_mcp_server_contract_test
        LABELS unit mcp
    )
    # Streamable HTTP framing and admission against a hostile-input corpus (pure, byte strings).
    add_executable(bloom_mcp_core_tools_test tests/core_tools_tests.cpp)
    target_include_directories(bloom_mcp_core_tools_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_mcp_core_tools_test PRIVATE bloom_mcp bloom_host_export_testing
                                                    yyjson::yyjson)
    bloom_enable_warnings(bloom_mcp_core_tools_test)
    bloom_add_test(
        NAME bloom.mcp.core-tools
        COMMAND bloom_mcp_core_tools_test
        LABELS unit mcp
        TIMEOUT 120
    )
    # #724 Scopes: query kind `scopes` through the shared core, headless.
    add_executable(bloom_mcp_scope_query_test tests/scope_query_tests.cpp)
    target_link_libraries(bloom_mcp_scope_query_test PRIVATE bloom_mcp yyjson::yyjson)
    bloom_enable_warnings(bloom_mcp_scope_query_test)
    bloom_add_test(
        NAME bloom.mcp.scope-query
        COMMAND bloom_mcp_scope_query_test
        LABELS unit mcp runtime render
        TIMEOUT 300
    )
    add_executable(bloom_mcp_export_reopen_quality_test tests/export_reopen_quality_tests.cpp)
    target_link_libraries(bloom_mcp_export_reopen_quality_test PRIVATE bloom_mcp
                          bloom_host_export_testing yyjson::yyjson)
    bloom_enable_warnings(bloom_mcp_export_reopen_quality_test)
    bloom_add_test(
        NAME bloom.mcp.export-reopen-quality
        COMMAND bloom_mcp_export_reopen_quality_test
        LABELS unit mcp
        TIMEOUT 120
    )
    add_executable(bloom_mcp_http_parser_test tests/http_parser_tests.cpp)
    target_link_libraries(bloom_mcp_http_parser_test PRIVATE bloom_mcp)
    bloom_enable_warnings(bloom_mcp_http_parser_test)
    bloom_add_test(
        NAME bloom.mcp.http-parser
        COMMAND bloom_mcp_http_parser_test
        LABELS unit mcp
    )
    add_executable(bloom_mcp_server_service_test tests/server_service_tests.cpp)
    target_link_libraries(bloom_mcp_server_service_test PRIVATE bloom_mcp_testing)
    bloom_enable_warnings(bloom_mcp_server_service_test)
    bloom_add_test(
        NAME bloom.mcp.server-service
        COMMAND bloom_mcp_server_service_test
        LABELS unit mcp
        TIMEOUT 120
    )
    # Streamable HTTP over a real loopback socket against a headless McpServerService.
    add_executable(bloom_mcp_http_test_host tests/http_test_host.cpp)
    target_include_directories(bloom_mcp_http_test_host PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(bloom_mcp_http_test_host PRIVATE bloom_mcp bloom_mcp_testing)
    bloom_enable_warnings(bloom_mcp_http_test_host)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        bloom_add_test(
            NAME bloom.mcp.http-conformance
            COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/mcp_http_conformance.py"
                $<TARGET_FILE:bloom_mcp_http_test_host>
            LABELS mcp integration
            TIMEOUT 180
        )
        # The scenario suite runner's MCP client (against the same server), playback rate,
        # baseline comparison, pass criteria and parity gate (tools/bench, issue #540).
        # The bench tools import numpy. A build with the locked interpreter runs them with it:
        # it carries numpy (issue #677), so the suite needs no Python on the computer. A
        # developer build against the host's interpreter uses that one, and the tests that need
        # numpy skip themselves when it has none.
        bloom_add_test(
            NAME bloom.bench.scenario-suite
            COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/bench/test_run_scenarios.py"
                $<TARGET_FILE:bloom_mcp_http_test_host>
            LABELS mcp unit
            TIMEOUT 120
        )
    endif()
endif()
